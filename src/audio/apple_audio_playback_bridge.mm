#include "apple_audio_playback_bridge.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace
{

constexpr OSStatus kNoErr = 0;

struct WavsenAppleAudioPlaybackImpl {
    AudioUnit unit = nullptr;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    WavsenAppleAudioPlaybackCallback callback = nullptr;
    void* user = nullptr;
    std::atomic_bool running { false };
};

static OSStatus render(void* user,
                       AudioUnitRenderActionFlags*,
                       const AudioTimeStamp*,
                       UInt32,
                       UInt32 frames,
                       AudioBufferList* output) {
    auto* playback = static_cast<WavsenAppleAudioPlaybackImpl*>(user);
    if (playback == nullptr || output == nullptr || playback->channels == 0) return kNoErr;

    // The unit is configured as interleaved f32, so the default output should
    // always provide one buffer. Clear unexpected layouts rather than writing
    // through a possibly non-interleaved buffer with the wrong stride.
    if (output->mNumberBuffers != 1 || output->mBuffers[0].mData == nullptr) {
        for (UInt32 index = 0; index < output->mNumberBuffers; ++index) {
            auto& buffer = output->mBuffers[index];
            if (buffer.mData != nullptr) std::memset(buffer.mData, 0, buffer.mDataByteSize);
        }
        return kNoErr;
    }

    auto& buffer = output->mBuffers[0];
    const auto frame_bytes = static_cast<std::size_t>(playback->channels) * sizeof(float);
    const auto available_frames = frame_bytes == 0
                                      ? std::size_t(0)
                                      : (buffer.mDataByteSize == 0
                                             ? static_cast<std::size_t>(frames)
                                             : static_cast<std::size_t>(buffer.mDataByteSize) /
                                                   frame_bytes);
    const auto writable_frames = available_frames < frames ? available_frames : frames;
    auto* samples = static_cast<float*>(buffer.mData);
    const auto sample_count = writable_frames * playback->channels;
    std::memset(samples, 0, sample_count * sizeof(float));

    if (playback->running.load(std::memory_order_acquire) && playback->callback != nullptr &&
        writable_frames != 0) {
        playback->callback(samples,
                           static_cast<std::uint32_t>(writable_frames),
                           playback->channels,
                           playback->user);
    }

    buffer.mDataByteSize = static_cast<UInt32>(sample_count * sizeof(float));
    return kNoErr;
}

static void destroy_unit(WavsenAppleAudioPlaybackImpl* playback) {
    if (playback == nullptr) return;
    playback->running.store(false, std::memory_order_release);
    if (playback->unit == nullptr) return;
    AudioOutputUnitStop(playback->unit);
    AudioUnitUninitialize(playback->unit);
    AudioComponentInstanceDispose(playback->unit);
    playback->unit = nullptr;
}

} // namespace

struct WavsenAppleAudioPlayback {
    WavsenAppleAudioPlaybackImpl impl;
};

extern "C" WavsenAppleAudioPlayback* wavsen_apple_create_audio_playback(
    std::uint32_t sample_rate,
    std::uint32_t channels,
    WavsenAppleAudioPlaybackCallback callback,
    void* user) {
    if (sample_rate == 0 || channels == 0 || callback == nullptr) return nullptr;

    auto* playback = new (std::nothrow) WavsenAppleAudioPlayback();
    if (playback == nullptr) return nullptr;
    playback->impl.sample_rate = sample_rate;
    playback->impl.channels = channels;
    playback->impl.callback = callback;
    playback->impl.user = user;

    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_DefaultOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr ||
        AudioComponentInstanceNew(component, &playback->impl.unit) != noErr) {
        delete playback;
        return nullptr;
    }

    AudioStreamBasicDescription format {};
    format.mSampleRate = static_cast<Float64>(sample_rate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = channels * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = channels * sizeof(float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = sizeof(float) * 8;

    auto status = AudioUnitSetProperty(playback->impl.unit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input,
                                       0,
                                       &format,
                                       sizeof(format));
    if (status != noErr) {
        destroy_unit(&playback->impl);
        delete playback;
        return nullptr;
    }

    AURenderCallbackStruct callback_info { &render, &playback->impl };
    status = AudioUnitSetProperty(playback->impl.unit,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input,
                                  0,
                                  &callback_info,
                                  sizeof(callback_info));
    if (status != noErr) {
        destroy_unit(&playback->impl);
        delete playback;
        return nullptr;
    }
    return playback;
}

extern "C" int wavsen_apple_start_audio_playback(WavsenAppleAudioPlayback* playback) {
    if (playback == nullptr || playback->impl.unit == nullptr) return -1;
    if (playback->impl.running.load(std::memory_order_acquire)) return 0;

    auto status = AudioUnitInitialize(playback->impl.unit);
    if (status != noErr) return status;
    playback->impl.running.store(true, std::memory_order_release);
    status = AudioOutputUnitStart(playback->impl.unit);
    if (status != noErr) {
        playback->impl.running.store(false, std::memory_order_release);
        AudioUnitUninitialize(playback->impl.unit);
        return status;
    }
    return noErr;
}

extern "C" void wavsen_apple_stop_audio_playback(WavsenAppleAudioPlayback* playback) {
    if (playback == nullptr) return;
    playback->impl.running.store(false, std::memory_order_release);
    if (playback->impl.unit != nullptr) {
        AudioOutputUnitStop(playback->impl.unit);
        AudioUnitUninitialize(playback->impl.unit);
    }
}

extern "C" void wavsen_apple_destroy_audio_playback(WavsenAppleAudioPlayback* playback) {
    if (playback == nullptr) return;
    destroy_unit(&playback->impl);
    delete playback;
}

extern "C" int wavsen_apple_audio_playback_is_running(
    const WavsenAppleAudioPlayback* playback) {
    return playback != nullptr &&
                   playback->impl.running.load(std::memory_order_acquire)
               ? 1
               : 0;
}
