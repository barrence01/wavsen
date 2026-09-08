#include "apple_audio_capture_bridge.hpp"

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareTapping.h>
#include <CoreAudio/CATapDescription.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <unistd.h>

namespace
{

constexpr OSStatus kNoErr = 0;
constexpr AudioObjectID kUnknownObject = kAudioObjectUnknown;
constexpr UInt32 kDriftCompensationMediumQuality = 0x40;
constexpr std::size_t kScratchFrames = 8192;
constexpr std::size_t kScratchChannels = 2;

struct WavsenAppleAudioCaptureImpl {
    AudioObjectID tap_id = kUnknownObject;
    AudioObjectID aggregate_id = kUnknownObject;
    AudioDeviceIOProcID io_proc_id = nullptr;
    WavsenAppleAudioCallback callback = nullptr;
    void* user = nullptr;
    std::atomic_bool running { false };
    std::array<float, kScratchFrames * kScratchChannels> scratch {};
};

static AudioObjectID current_process_object() {
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    const pid_t pid = getpid();
    AudioObjectID process_object = kUnknownObject;
    UInt32 data_size = sizeof(process_object);
    const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                   &address,
                                                   sizeof(pid),
                                                   &pid,
                                                   &data_size,
                                                   &process_object);
    return status == kNoErr ? process_object : kUnknownObject;
}

static CFStringRef make_uuid_string() {
    NSUUID* uuid = [[NSUUID alloc] init];
    NSString* string = [uuid UUIDString];
    CFStringRef result = CFStringCreateWithCString(kCFAllocatorDefault,
                                                   [string UTF8String],
                                                   kCFStringEncodingUTF8);
    [uuid release];
    return result;
}

static CFDictionaryRef make_tap_entry(CFStringRef tap_uid) {
    const void* keys[] = {
        CFSTR("uid"),
        CFSTR("drift"),
        CFSTR("drift quality"),
    };
    SInt32 quality = static_cast<SInt32>(kDriftCompensationMediumQuality);
    CFNumberRef drift_quality = CFNumberCreate(kCFAllocatorDefault,
                                               kCFNumberSInt32Type,
                                               &quality);
    const void* values[] = {
        tap_uid,
        kCFBooleanTrue,
        drift_quality,
    };
    CFDictionaryRef entry = CFDictionaryCreate(kCFAllocatorDefault,
                                                keys,
                                                values,
                                                3,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    if (drift_quality != nullptr) CFRelease(drift_quality);
    return entry;
}

static OSStatus create_process_tap(WavsenAppleAudioCaptureImpl* capture,
                                   CFStringRef* tap_uid_out) {
    @autoreleasepool {
        NSMutableArray<NSNumber*>* excluded = [[NSMutableArray alloc] init];
        const auto process_object = current_process_object();
        if (process_object != kUnknownObject) {
            [excluded addObject:[NSNumber numberWithUnsignedInt:process_object]];
        }

        CATapDescription* description =
            [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:excluded];
        [excluded release];
        if (description == nil) return -1;

        NSUUID* uuid = [[NSUUID alloc] init];
        [description setPrivate:YES];
        [description setMuteBehavior:CATapUnmuted];
        [description setName:@"Wallpaper Engine System Audio Tap"];
        [description setUUID:uuid];
        CFStringRef tap_uid = CFStringCreateWithCString(kCFAllocatorDefault,
                                                        [[uuid UUIDString] UTF8String],
                                                        kCFStringEncodingUTF8);
        [uuid release];

        AudioObjectID tap_id = kUnknownObject;
        const auto status = AudioHardwareCreateProcessTap(description, &tap_id);
        [description release];
        if (status != kNoErr || tap_uid == nullptr) {
            if (tap_uid != nullptr) CFRelease(tap_uid);
            return status != kNoErr ? status : -1;
        }

        capture->tap_id = tap_id;
        *tap_uid_out = tap_uid;
        return kNoErr;
    }
}

static OSStatus create_aggregate_device(WavsenAppleAudioCaptureImpl* capture,
                                         CFStringRef tap_uid) {
    CFDictionaryRef tap_entry = make_tap_entry(tap_uid);
    if (tap_entry == nullptr) return -1;

    const void* tap_values[] = { tap_entry };
    CFArrayRef taps = CFArrayCreate(kCFAllocatorDefault,
                                    tap_values,
                                    1,
                                    &kCFTypeArrayCallBacks);
    CFStringRef aggregate_uid = make_uuid_string();
    if (taps == nullptr || aggregate_uid == nullptr) {
        if (taps != nullptr) CFRelease(taps);
        if (aggregate_uid != nullptr) CFRelease(aggregate_uid);
        CFRelease(tap_entry);
        return -1;
    }

    const void* keys[] = {
        CFSTR(kAudioAggregateDeviceNameKey),
        CFSTR(kAudioAggregateDeviceUIDKey),
        CFSTR(kAudioAggregateDeviceIsPrivateKey),
        CFSTR(kAudioAggregateDeviceTapListKey),
        CFSTR(kAudioAggregateDeviceTapAutoStartKey),
    };
    const void* values[] = {
        CFSTR("Wallpaper Engine System Audio Capture"),
        aggregate_uid,
        kCFBooleanTrue,
        taps,
        kCFBooleanFalse,
    };
    CFDictionaryRef aggregate = CFDictionaryCreate(kCFAllocatorDefault,
                                                    keys,
                                                    values,
                                                    5,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);

    AudioObjectID aggregate_id = kUnknownObject;
    const auto status = aggregate != nullptr
                            ? AudioHardwareCreateAggregateDevice(aggregate, &aggregate_id)
                            : static_cast<OSStatus>(-1);
    if (aggregate != nullptr) CFRelease(aggregate);
    CFRelease(aggregate_uid);
    CFRelease(taps);
    CFRelease(tap_entry);
    if (status != kNoErr) return status;

    capture->aggregate_id = aggregate_id;
    return kNoErr;
}

static void destroy_resources(WavsenAppleAudioCaptureImpl* capture) {
    if (capture == nullptr) return;
    capture->running.store(false, std::memory_order_release);
    if (capture->io_proc_id != nullptr && capture->aggregate_id != kUnknownObject) {
        AudioDeviceStop(capture->aggregate_id, capture->io_proc_id);
        AudioDeviceDestroyIOProcID(capture->aggregate_id, capture->io_proc_id);
    }
    capture->io_proc_id = nullptr;
    if (capture->aggregate_id != kUnknownObject) {
        AudioHardwareDestroyAggregateDevice(capture->aggregate_id);
        capture->aggregate_id = kUnknownObject;
    }
    if (capture->tap_id != kUnknownObject) {
        AudioHardwareDestroyProcessTap(capture->tap_id);
        capture->tap_id = kUnknownObject;
    }
}

static OSStatus audio_io_proc(AudioObjectID,
                              const AudioTimeStamp*,
                              const AudioBufferList* input,
                              const AudioTimeStamp*,
                              AudioBufferList*,
                              const AudioTimeStamp*,
                              void* user) {
    auto* capture = static_cast<WavsenAppleAudioCaptureImpl*>(user);
    if (capture == nullptr || ! capture->running.load(std::memory_order_acquire) ||
        capture->callback == nullptr ||
        input == nullptr || input->mNumberBuffers == 0)
        return kNoErr;

    const auto buffer_count = static_cast<std::size_t>(input->mNumberBuffers);
    const auto* buffers = input->mBuffers;
    if (buffer_count == 1) {
        const auto& buffer = buffers[0];
        const auto channels = buffer.mNumberChannels == 0 ? 1u : buffer.mNumberChannels;
        const auto bytes_per_frame = static_cast<std::size_t>(channels) * sizeof(float);
        if (buffer.mData == nullptr || bytes_per_frame == 0) return kNoErr;
        const auto frames = static_cast<std::size_t>(buffer.mDataByteSize) / bytes_per_frame;
        if (frames == 0 || frames > UINT32_MAX) return kNoErr;
        capture->callback(static_cast<const float*>(buffer.mData),
                          static_cast<std::uint32_t>(frames),
                          channels,
                          capture->user);
        return kNoErr;
    }

    // Aggregate devices can expose one non-interleaved buffer per channel. The
    // tap is normally mono, but normalize that representation without any
    // callback-time allocation so the lito-side publisher sees interleaved f32.
    const auto channels = buffer_count < kScratchChannels ? buffer_count : kScratchChannels;
    std::size_t frames = SIZE_MAX;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto& buffer = buffers[channel];
        if (buffer.mData == nullptr) return kNoErr;
        frames = std::min(frames,
                          static_cast<std::size_t>(buffer.mDataByteSize) / sizeof(float));
    }
    if (frames == 0 || frames == SIZE_MAX) return kNoErr;

    for (std::size_t offset = 0; offset < frames; offset += kScratchFrames) {
        const auto count = std::min(kScratchFrames, frames - offset);
        for (std::size_t frame = 0; frame < count; ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto* source = static_cast<const float*>(buffers[channel].mData);
                capture->scratch[frame * channels + channel] = source[offset + frame];
            }
        }
        capture->callback(capture->scratch.data(),
                          static_cast<std::uint32_t>(count),
                          static_cast<std::uint32_t>(channels),
                          capture->user);
    }
    return kNoErr;
}

} // namespace

struct WavsenAppleAudioCapture {
    WavsenAppleAudioCaptureImpl impl;
};

extern "C" WavsenAppleAudioCapture* wavsen_apple_create_audio_capture(
    WavsenAppleAudioCallback callback,
    void* user) {
    auto* capture = new (std::nothrow) WavsenAppleAudioCapture();
    if (capture == nullptr) return nullptr;
    capture->impl.callback = callback;
    capture->impl.user = user;
    return capture;
}

extern "C" int wavsen_apple_start_audio_capture(WavsenAppleAudioCapture* capture) {
    if (capture == nullptr) return -1;
    auto& impl = capture->impl;
    if (impl.running.load(std::memory_order_acquire)) return 0;

    CFStringRef tap_uid = nullptr;
    auto status = create_process_tap(&impl, &tap_uid);
    if (status != kNoErr) {
        destroy_resources(&impl);
        return status;
    }

    status = create_aggregate_device(&impl, tap_uid);
    CFRelease(tap_uid);
    if (status != kNoErr) {
        destroy_resources(&impl);
        return status;
    }

    status = AudioDeviceCreateIOProcID(impl.aggregate_id,
                                       &audio_io_proc,
                                       &impl,
                                       &impl.io_proc_id);
    if (status != kNoErr) {
        destroy_resources(&impl);
        return status;
    }

    impl.running.store(true, std::memory_order_release);
    status = AudioDeviceStart(impl.aggregate_id, impl.io_proc_id);
    if (status != kNoErr) {
        destroy_resources(&impl);
        return status;
    }
    return kNoErr;
}

extern "C" void wavsen_apple_stop_audio_capture(WavsenAppleAudioCapture* capture) {
    if (capture == nullptr) return;
    destroy_resources(&capture->impl);
}

extern "C" void wavsen_apple_destroy_audio_capture(WavsenAppleAudioCapture* capture) {
    if (capture == nullptr) return;
    destroy_resources(&capture->impl);
    delete capture;
}

extern "C" int wavsen_apple_audio_capture_is_running(
    const WavsenAppleAudioCapture* capture) {
    return capture != nullptr && capture->impl.running.load(std::memory_order_acquire) ? 1 : 0;
}
