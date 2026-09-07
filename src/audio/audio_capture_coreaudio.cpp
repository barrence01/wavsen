module;

#include "apple_audio_capture_bridge.hpp"

module wavsen.audio.capture;

import rstd;
import rstd.log;
import wavsen.audio.capture_window;

using namespace rstd::prelude;

namespace wavsen::audio
{

class AudioCapture::Impl {
public:
    ~Impl() { uninit(); }

    bool init() {
        if (is_inited()) return true;

        publisher_.restart();
        native_ = wavsen_apple_create_audio_capture(&Impl::audio_callback, this);
        if (native_ == nullptr) {
            rstd::log::error("wavsen::audio: failed to allocate Core Audio capture");
            return false;
        }

        const auto status = wavsen_apple_start_audio_capture(native_);
        if (status != 0) {
            rstd::log::error("wavsen::audio: Core Audio system tap failed ({})", status);
            wavsen_apple_destroy_audio_capture(native_);
            native_ = nullptr;
            publisher_.restart();
            return false;
        }

        started_ = true;
        rstd::log::info("wavsen::audio: Core Audio system-output capture inited ({} Hz)",
                        kAudioSampleRate);
        return true;
    }

    void uninit() {
        if (native_ != nullptr) {
            wavsen_apple_stop_audio_capture(native_);
            wavsen_apple_destroy_audio_capture(native_);
        }
        native_ = nullptr;
        started_ = false;
        publisher_.restart();
        last_generation_ = 0;
        last_sequence_   = 0;
    }

    bool is_inited() const {
        return native_ != nullptr && started_ &&
               wavsen_apple_audio_capture_is_running(native_) != 0;
    }

    bool snapshot(AudioPcmWindow& out) {
        AudioPcmWindow candidate {};
        if (! publisher_.snapshot(candidate)) return false;
        if (candidate.generation == last_generation_ && candidate.sequence == last_sequence_)
            return false;
        last_generation_ = candidate.generation;
        last_sequence_   = candidate.sequence;
        out              = candidate;
        return true;
    }

private:
    static void audio_callback(const float* samples,
                               std::uint32_t frames,
                               std::uint32_t channels,
                               void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self == nullptr) return;
        self->publisher_.ingest(samples, frames, channels);
    }

    WavsenAppleAudioCapture* native_ = nullptr;
    bool                    started_ = false;
    capture::PcmWindowPublisher publisher_;
    std::uint64_t                 last_generation_ = 0;
    std::uint64_t                 last_sequence_   = 0;
};

AudioCapture::AudioCapture(): impl_(Box<Impl>::make()) {}
AudioCapture::~AudioCapture() = default;

bool AudioCapture::init() { return impl_->init(); }
void AudioCapture::uninit() { impl_->uninit(); }
bool AudioCapture::is_inited() const { return impl_->is_inited(); }
bool AudioCapture::snapshot(AudioPcmWindow& out) { return impl_->snapshot(out); }

} // namespace wavsen::audio
