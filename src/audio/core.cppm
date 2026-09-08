export module wavsen.audio.core;

import rstd.cppstd;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace wavsen::audio
{

// Negotiated stream format — wavsen always asks the backend for f32 interleaved.
struct DeviceDesc {
    u32 channels;
    u32 sample_rate;
};

struct AudioClientIdentity {
    String application_name { String::make("wavsen"_str) };
    String application_id { String::make("org.wavsen"_str) };
    String stream_prefix { String::make("wavsen."_str) };
    String component { String::make("audio"_str) };
    String media_name { String::make("wavsen audio output"_str) };
    String media_role { String::make("music"_str) };

    auto clone() const -> AudioClientIdentity {
        return {
            .application_name = application_name.clone(),
            .application_id   = application_id.clone(),
            .stream_prefix    = stream_prefix.clone(),
            .component        = component.clone(),
            .media_name       = media_name.clone(),
            .media_role       = media_role.clone(),
        };
    }

    auto playback_stream_name() const -> Option<String> {
        if (component.is_empty()) return None();
        for (const auto value : component) {
            const bool valid = (value >= u8('a') && value <= u8('z')) ||
                               (value >= u8('0') && value <= u8('9')) || value == u8('-');
            if (! valid) return None();
        }
        return Some(rstd::format("{}{}.playback", stream_prefix, component));
    }
};

// Pulled by the audio thread to fill an output buffer. Implementations
// must NOT block (the backend data thread is realtime). `frames` is the
// number of interleaved frames to write into `dst`.
class IPullChannel {
public:
    virtual ~IPullChannel()                             = default;
    virtual auto next_pcm(void* dst, u32 frames) -> u64 = 0;
    virtual void pass_desc(const DeviceDesc&)           = 0;
};

enum class AudioDeviceState : rstd::uint8_t
{
    Idle,
    Connecting,
    ReadyPaused,
    ReadyPlaying,
    Failed,
    Stopped,
};

struct AudioDeviceEvent {
    u64              generation;
    AudioDeviceState state { AudioDeviceState::Idle };
    String           error;
};

using AudioDeviceEventSink = std::function<void(AudioDeviceEvent)>;

struct AudioDeviceDesiredState {
    u64                 generation;
    bool                active {};
    bool                playing {};
    AudioClientIdentity identity;
    f32                 volume { f32(1.0f) };
    bool                muted {};
    f32                 volume_scale { f32(1.0f) };
    u64                 volume_scale_revision;
    u32                 volume_scale_fade_ms;
    u64                 playback_buffer_revision;

    auto clone() const -> AudioDeviceDesiredState {
        return {
            .generation               = generation,
            .active                   = active,
            .playing                  = playing,
            .identity                 = identity.clone(),
            .volume                   = volume,
            .muted                    = muted,
            .volume_scale             = volume_scale,
            .volume_scale_revision    = volume_scale_revision,
            .volume_scale_fade_ms     = volume_scale_fade_ms,
            .playback_buffer_revision = playback_buffer_revision,
        };
    }
};

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&)            = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    void set_event_sink(AudioDeviceEventSink);
    auto apply(AudioDeviceDesiredState) -> bool;
    auto mount(std::unique_ptr<IPullChannel>, u64 stream_revision) -> bool;
    auto unmount_all(u64 stream_revision) -> bool;
    void shutdown();
    void wait_stopped();

    auto state() const -> AudioDeviceState;
    auto desc() const -> DeviceDesc;

    // Frames the audio device has actually played back since the stream
    // was created. The backend thread publishes this cached value so callers
    // never enter the native audio API from another thread.
    auto stream_position_frames() const -> u64;

private:
    class Impl;
    Box<Impl> impl_;
};

} // namespace wavsen::audio
