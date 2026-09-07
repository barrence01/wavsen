#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WavsenAppleAudioCapture WavsenAppleAudioCapture;

typedef void (*WavsenAppleAudioCallback)(const float* samples,
                                         std::uint32_t frames,
                                         std::uint32_t channels,
                                         void* user);

WavsenAppleAudioCapture* wavsen_apple_create_audio_capture(
    WavsenAppleAudioCallback callback,
    void* user);

int wavsen_apple_start_audio_capture(WavsenAppleAudioCapture* capture);
void wavsen_apple_stop_audio_capture(WavsenAppleAudioCapture* capture);
void wavsen_apple_destroy_audio_capture(WavsenAppleAudioCapture* capture);
int wavsen_apple_audio_capture_is_running(const WavsenAppleAudioCapture* capture);

#ifdef __cplusplus
}
#endif
