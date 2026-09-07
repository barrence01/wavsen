#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WavsenAppleAudioPlayback WavsenAppleAudioPlayback;

typedef void (*WavsenAppleAudioPlaybackCallback)(float* samples,
                                                 std::uint32_t frames,
                                                 std::uint32_t channels,
                                                 void* user);

WavsenAppleAudioPlayback* wavsen_apple_create_audio_playback(
    std::uint32_t sample_rate,
    std::uint32_t channels,
    WavsenAppleAudioPlaybackCallback callback,
    void* user);

int wavsen_apple_start_audio_playback(WavsenAppleAudioPlayback* playback);
void wavsen_apple_stop_audio_playback(WavsenAppleAudioPlayback* playback);
void wavsen_apple_destroy_audio_playback(WavsenAppleAudioPlayback* playback);
int wavsen_apple_audio_playback_is_running(const WavsenAppleAudioPlayback* playback);

#ifdef __cplusplus
}
#endif
