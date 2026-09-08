module;

#if defined(__APPLE__)
#include "apple_audio_playback_bridge.hpp"

#include <cstring>
#include <mutex>
#include <vector>
#endif

module wavsen.audio;

#if !defined(__APPLE__)
import rstd.cppstd;
import rstd;
import rstd.log;
import wavsen.ffi.pulse;
import wavsen.audio.gain;
import :core;

using namespace rstd::prelude;

namespace wavsen::audio
{

namespace pulse_ffi = wavsen::ffi::pulse;

namespace
{

constexpr rstd::uint32_t kDefaultRate     = 48000;
constexpr rstd::uint32_t kDefaultChannels = 2;
constexpr rstd::uint32_t kQuantum         = 1024;

enum class DeviceCommandKind : rstd::uint8_t
{
    Apply,
    Mount,
    UnmountAll,
    Shutdown,
};

enum class PlaybackOperation : rstd::uint8_t
{
    None,
    Cork,
    Flush,
};

struct DeviceCommand {
    DeviceCommandKind             kind { DeviceCommandKind::Apply };
    AudioDeviceDesiredState       desired;
    std::unique_ptr<IPullChannel> channel;
    u64                           stream_revision;
};

struct CommandQueue {
    Vec<DeviceCommand> pending;
    bool               scheduled {};
    bool               accepting { true };
    bool               shutdown_queued {};
};

} // namespace

class AudioDevice::Impl {
public:
    Impl()
        : commands_(CommandQueue {}),
          event_sink_(AudioDeviceEventSink {}),
          shutdown_complete_(false),
          shutdown_cv_(rstd::sync::Condvar::make()) {
        api_ = pulse_ffi::load();
        if (! api_) {
            rstd::log::error("wavsen::audio: failed to load PulseAudio: {}",
                             pulse_ffi::load_error());
            return;
        }

        loop_ = api_->pa_threaded_mainloop_new();
        if (! loop_) {
            rstd::log::error("wavsen::audio: pa_threaded_mainloop_new failed");
            return;
        }
        if (api_->pa_threaded_mainloop_start(loop_) < 0) {
            rstd::log::error("wavsen::audio: pa_threaded_mainloop_start failed");
            api_->pa_threaded_mainloop_free(loop_);
            loop_ = nullptr;
            return;
        }
    }

    ~Impl() {
        shutdown();
        wait_stopped();
        if (loop_) {
            api_->pa_threaded_mainloop_stop(loop_);
            api_->pa_threaded_mainloop_free(loop_);
            loop_ = nullptr;
        }
    }

    void set_event_sink(AudioDeviceEventSink sink) {
        auto current = event_sink_.lock().unwrap_unchecked();
        *current     = rstd::move(sink);
    }

    bool apply(AudioDeviceDesiredState desired) {
        return enqueue(DeviceCommand {
            .kind    = DeviceCommandKind::Apply,
            .desired = rstd::move(desired),
        });
    }

    bool mount(std::unique_ptr<IPullChannel> channel, u64 stream_revision) {
        if (! channel) return false;
        return enqueue(DeviceCommand {
            .kind            = DeviceCommandKind::Mount,
            .channel         = rstd::move(channel),
            .stream_revision = stream_revision,
        });
    }

    bool unmount_all(u64 stream_revision) {
        return enqueue(DeviceCommand {
            .kind            = DeviceCommandKind::UnmountAll,
            .stream_revision = stream_revision,
        });
    }

    void shutdown() {
        if (! loop_) {
            mark_shutdown_complete();
            return;
        }

        bool schedule = false;
        {
            auto commands = commands_.lock().unwrap_unchecked();
            if (commands->shutdown_queued) return;
            commands->shutdown_queued = true;
            commands->accepting       = false;
            commands->pending.push(DeviceCommand { .kind = DeviceCommandKind::Shutdown });
            if (! commands->scheduled) {
                commands->scheduled = true;
                schedule            = true;
            }
        }
        if (schedule) schedule_commands();
    }

    void wait_stopped() {
        if (! loop_) return;
        auto complete = shutdown_complete_.lock().unwrap_unchecked();
        shutdown_cv_.wait_while(complete, [](bool value) {
            return ! value;
        });
    }

    AudioDeviceState state() const { return state_.load(rstd::sync::atomic::Ordering::Acquire); }
    DeviceDesc       desc() const { return desc_; }
    auto             stream_position_frames() const -> u64 {
        return stream_position_frames_.load(rstd::sync::atomic::Ordering::Relaxed);
    }

private:
    bool enqueue(DeviceCommand command) {
        if (! loop_) return false;

        bool schedule = false;
        {
            auto commands = commands_.lock().unwrap_unchecked();
            if (! commands->accepting) return false;
            commands->pending.push(rstd::move(command));
            if (! commands->scheduled) {
                commands->scheduled = true;
                schedule            = true;
            }
        }
        if (schedule) schedule_commands();
        return true;
    }

    void schedule_commands() {
        api_->pa_threaded_mainloop_lock(loop_);
        api_->pa_mainloop_api_once(
            api_->pa_threaded_mainloop_get_api(loop_), &Impl::on_commands, this);
        api_->pa_threaded_mainloop_unlock(loop_);
    }

    static void on_commands(pulse_ffi::pa_mainloop_api* /*api*/, void* user) {
        static_cast<Impl*>(user)->drain_commands();
    }

    void drain_commands() {
        for (;;) {
            Vec<DeviceCommand> batch;
            {
                auto commands = commands_.lock().unwrap_unchecked();
                if (commands->pending.is_empty()) {
                    commands->scheduled = false;
                    return;
                }
                batch             = rstd::move(commands->pending);
                commands->pending = Vec<DeviceCommand>();
            }

            for (auto& command : batch) {
                process(rstd::move(command));
            }
        }
    }

    void process(DeviceCommand command) {
        switch (command.kind) {
        case DeviceCommandKind::Apply: apply_desired(rstd::move(command.desired)); break;
        case DeviceCommandKind::Mount:
            if (command.stream_revision < stream_revision_) return;
            stream_revision_ = command.stream_revision;
            if (stream_ && api_->pa_stream_get_state(stream_) == pulse_ffi::stream_ready) {
                command.channel->pass_desc(desc_);
            }
            channels_.push(rstd::move(command.channel));
            break;
        case DeviceCommandKind::UnmountAll:
            if (command.stream_revision < stream_revision_) return;
            stream_revision_ = command.stream_revision;
            channels_.clear();
            break;
        case DeviceCommandKind::Shutdown:
            shutting_down_ = true;
            cleanup_device();
            channels_.clear();
            emit_state(AudioDeviceState::Stopped);
            mark_shutdown_complete();
            break;
        }
    }

    void apply_desired(AudioDeviceDesiredState desired) {
        if (shutting_down_) return;

        const bool generation_changed = desired.generation != desired_.generation;
        if (generation_changed) cleanup_device();
        desired_ = rstd::move(desired);
        apply_gain_state();

        if (! desired_.active) {
            cleanup_device();
            emit_state(AudioDeviceState::Idle);
            return;
        }
        if (! ctx_) {
            start_context();
            return;
        }
        if (stream_ && api_->pa_stream_get_state(stream_) == pulse_ffi::stream_ready)
            reconcile_playback();
    }

    void apply_gain_state() {
        volume_ = desired_.volume;
        muted_  = desired_.muted;
        if (desired_.volume_scale_revision == volume_scale_revision_) return;

        volume_scale_revision_ = desired_.volume_scale_revision;
        volume_scale_.redirect(
            desired_.volume_scale, desc_.sample_rate, desired_.volume_scale_fade_ms);
    }

    void start_context() {
        const auto stream_name = desired_.identity.playback_stream_name();
        if (! stream_name) {
            fail("invalid audio client component");
            return;
        }

        auto application_name =
            rstd::ffi::CString::make(desired_.identity.application_name.clone()).unwrap();
        auto application_id =
            rstd::ffi::CString::make(desired_.identity.application_id.clone()).unwrap();
        auto* properties = api_->pa_proplist_new();
        if (! properties ||
            api_->pa_proplist_sets(
                properties, pulse_ffi::prop_application_name, application_name.as_ptr()) < 0 ||
            api_->pa_proplist_sets(
                properties, pulse_ffi::prop_application_id, application_id.as_ptr()) < 0) {
            if (properties) api_->pa_proplist_free(properties);
            fail("failed to build PulseAudio context properties");
            return;
        }

        ctx_ = api_->pa_context_new_with_proplist(
            api_->pa_threaded_mainloop_get_api(loop_), application_name.as_ptr(), properties);
        api_->pa_proplist_free(properties);
        if (! ctx_) {
            fail("pa_context_new failed");
            return;
        }
        api_->pa_context_set_state_callback(ctx_, &Impl::on_context_state, this);
        if (api_->pa_context_connect(ctx_, nullptr, pulse_ffi::context_noflags, nullptr) < 0) {
            auto error = rstd::format("pa_context_connect failed: {}",
                                      api_->pa_strerror(api_->pa_context_errno(ctx_)));
            cleanup_device();
            fail(rstd::move(error));
            return;
        }
        emit_state(AudioDeviceState::Connecting);
    }

    void start_stream() {
        if (stream_ || ! ctx_ || ! desired_.active) return;
        const auto stream_name = desired_.identity.playback_stream_name();
        if (! stream_name) {
            fail("invalid audio client component");
            return;
        }

        pulse_ffi::pa_sample_spec sample_spec {};
        sample_spec.format   = pulse_ffi::sample_float32le;
        sample_spec.rate     = kDefaultRate;
        sample_spec.channels = static_cast<rstd::uint8_t>(kDefaultChannels);

        pulse_ffi::pa_channel_map channel_map {};
        api_->pa_channel_map_init_stereo(&channel_map);

        auto application_name =
            rstd::ffi::CString::make(desired_.identity.application_name.clone()).unwrap();
        auto application_id =
            rstd::ffi::CString::make(desired_.identity.application_id.clone()).unwrap();
        auto  media_name = rstd::ffi::CString::make(desired_.identity.media_name.clone()).unwrap();
        auto  media_role = rstd::ffi::CString::make(desired_.identity.media_role.clone()).unwrap();
        auto  stream_name_c = rstd::ffi::CString::make(stream_name->clone()).unwrap();
        auto* properties    = api_->pa_proplist_new();
        if (! properties ||
            api_->pa_proplist_sets(
                properties, pulse_ffi::prop_application_name, application_name.as_ptr()) < 0 ||
            api_->pa_proplist_sets(
                properties, pulse_ffi::prop_application_id, application_id.as_ptr()) < 0 ||
            api_->pa_proplist_sets(properties, pulse_ffi::prop_media_name, media_name.as_ptr()) <
                0 ||
            api_->pa_proplist_sets(properties, pulse_ffi::prop_media_role, media_role.as_ptr()) <
                0) {
            if (properties) api_->pa_proplist_free(properties);
            fail("failed to build PulseAudio stream properties");
            return;
        }

        stream_ = api_->pa_stream_new_with_proplist(
            ctx_, stream_name_c.as_ptr(), &sample_spec, &channel_map, properties);
        api_->pa_proplist_free(properties);
        if (! stream_) {
            fail(rstd::format("pa_stream_new failed: {}",
                              api_->pa_strerror(api_->pa_context_errno(ctx_))));
            return;
        }

        api_->pa_stream_set_state_callback(stream_, &Impl::on_stream_state, this);
        api_->pa_stream_set_write_callback(stream_, &Impl::on_write, this);

        const auto frame_bytes = kDefaultChannels * static_cast<rstd::uint32_t>(sizeof(float));
        pulse_ffi::pa_buffer_attr buffer_attr {};
        buffer_attr.maxlength = static_cast<rstd::uint32_t>(-1);
        buffer_attr.tlength   = kQuantum * frame_bytes * 4;
        buffer_attr.prebuf    = static_cast<rstd::uint32_t>(-1);
        buffer_attr.minreq    = kQuantum * frame_bytes;
        buffer_attr.fragsize  = static_cast<rstd::uint32_t>(-1);

        const auto flags = static_cast<pulse_ffi::pa_stream_flags_t>(
            pulse_ffi::stream_adjust_latency | pulse_ffi::stream_auto_timing_update |
            pulse_ffi::stream_interpolate_timing | pulse_ffi::stream_start_corked);
        if (api_->pa_stream_connect_playback(
                stream_, nullptr, &buffer_attr, flags, nullptr, nullptr) < 0) {
            auto error = rstd::format("pa_stream_connect_playback failed: {}",
                                      api_->pa_strerror(api_->pa_context_errno(ctx_)));
            cleanup_stream();
            fail(rstd::move(error));
        }
    }

    void reconcile_playback() {
        if (! stream_ || api_->pa_stream_get_state(stream_) != pulse_ffi::stream_ready) return;
        if (playback_operation_ != PlaybackOperation::None) return;

        const int corked = api_->pa_stream_is_corked(stream_);
        if (corked < 0) {
            fail(rstd::format("pa_stream_is_corked failed: {}",
                              api_->pa_strerror(api_->pa_context_errno(ctx_))));
            return;
        }
        if (desired_.playback_buffer_revision != applied_playback_buffer_revision_) {
            if (corked == 0) {
                start_cork(true);
            } else {
                start_flush();
            }
            return;
        }
        if ((corked == 0) == desired_.playing) {
            emit_state(desired_.playing ? AudioDeviceState::ReadyPlaying
                                        : AudioDeviceState::ReadyPaused);
            return;
        }

        start_cork(! desired_.playing);
    }

    void start_cork(bool corked) {
        playback_operation_        = PlaybackOperation::Cork;
        playback_operation_stream_ = stream_;
        auto* operation            = api_->pa_stream_cork(
            stream_, corked ? 1 : 0, &Impl::on_playback_operation_complete, this);
        if (! operation) {
            playback_operation_        = PlaybackOperation::None;
            playback_operation_stream_ = nullptr;
            fail(rstd::format("pa_stream_cork failed: {}",
                              api_->pa_strerror(api_->pa_context_errno(ctx_))));
            return;
        }
        api_->pa_operation_unref(operation);
    }

    void start_flush() {
        playback_operation_        = PlaybackOperation::Flush;
        playback_operation_stream_ = stream_;
        auto* operation =
            api_->pa_stream_flush(stream_, &Impl::on_playback_operation_complete, this);
        if (! operation) {
            playback_operation_        = PlaybackOperation::None;
            playback_operation_stream_ = nullptr;
            fail(rstd::format("pa_stream_flush failed: {}",
                              api_->pa_strerror(api_->pa_context_errno(ctx_))));
            return;
        }
        api_->pa_operation_unref(operation);
    }

    void cleanup_stream() {
        if (! stream_) return;
        playback_operation_               = PlaybackOperation::None;
        playback_operation_stream_        = nullptr;
        applied_playback_buffer_revision_ = u64();
        api_->pa_stream_set_state_callback(stream_, nullptr, nullptr);
        api_->pa_stream_set_write_callback(stream_, nullptr, nullptr);
        api_->pa_stream_disconnect(stream_);
        api_->pa_stream_unref(stream_);
        stream_ = nullptr;
        stream_position_frames_.store(u64(), rstd::sync::atomic::Ordering::Relaxed);
    }

    void cleanup_device() {
        cleanup_stream();
        if (! ctx_) return;
        api_->pa_context_set_state_callback(ctx_, nullptr, nullptr);
        api_->pa_context_disconnect(ctx_);
        api_->pa_context_unref(ctx_);
        ctx_ = nullptr;
    }

    void fail(String error) {
        rstd::log::error("wavsen::audio: {}", error);
        emit_state(AudioDeviceState::Failed, rstd::move(error));
    }

    void fail(const char* error) {
        fail(String::make(rstd::ffi::CStr::from_ptr(error).to_str().unwrap()));
    }

    void emit_state(AudioDeviceState state, String error = {}) {
        state_.store(state, rstd::sync::atomic::Ordering::Release);
        AudioDeviceEventSink sink;
        {
            auto current = event_sink_.lock().unwrap_unchecked();
            sink         = *current;
        }
        if (sink) sink(AudioDeviceEvent { desired_.generation, state, rstd::move(error) });
    }

    void mark_shutdown_complete() {
        auto complete = shutdown_complete_.lock().unwrap_unchecked();
        if (*complete) return;
        *complete = true;
        shutdown_cv_.notify_all();
    }

    void apply_output_gain(float* output, rstd::uint32_t frames) {
        volume_scale_.apply(
            rstd::mut_ref<float[]>::from_raw_parts(
                output, rstd::usize(frames) * rstd::usize(desc_.channels.to_primitive())),
            desc_.channels,
            volume_);
    }

    static void on_context_state(pulse_ffi::pa_context* context, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (context != self->ctx_) return;
        switch (self->api_->pa_context_get_state(context)) {
        case pulse_ffi::context_ready: self->start_stream(); break;
        case pulse_ffi::context_failed: {
            self->fail(
                rstd::format("PulseAudio context failed: {}",
                             self->api_->pa_strerror(self->api_->pa_context_errno(context))));
            break;
        }
        case pulse_ffi::context_terminated:
            if (! self->shutting_down_ && self->desired_.active)
                self->fail("PulseAudio context terminated");
            break;
        default: break;
        }
    }

    static void on_stream_state(pulse_ffi::pa_stream* stream, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (stream != self->stream_) return;
        switch (self->api_->pa_stream_get_state(stream)) {
        case pulse_ffi::stream_ready:
            for (auto& channel : self->channels_) channel->pass_desc(self->desc_);
            self->applied_playback_buffer_revision_ = self->desired_.playback_buffer_revision;
            self->reconcile_playback();
            rstd::log::info("wavsen::audio: pulse device ready ({} ch @ {} Hz)",
                            self->desc_.channels,
                            self->desc_.sample_rate);
            break;
        case pulse_ffi::stream_failed: {
            self->fail(
                rstd::format("PulseAudio stream failed: {}",
                             self->api_->pa_strerror(self->api_->pa_context_errno(self->ctx_))));
            break;
        }
        case pulse_ffi::stream_terminated:
            if (! self->shutting_down_ && self->desired_.active)
                self->fail("PulseAudio stream terminated");
            break;
        default: break;
        }
    }

    static void on_playback_operation_complete(pulse_ffi::pa_stream* stream, int success,
                                               void* user) {
        auto* self = static_cast<Impl*>(user);
        if (stream != self->stream_ || stream != self->playback_operation_stream_) return;

        const auto operation             = self->playback_operation_;
        self->playback_operation_        = PlaybackOperation::None;
        self->playback_operation_stream_ = nullptr;
        if (! success) {
            const char* name =
                operation == PlaybackOperation::Flush ? "pa_stream_flush" : "pa_stream_cork";
            self->fail(
                rstd::format("{} failed: {}",
                             name,
                             self->api_->pa_strerror(self->api_->pa_context_errno(self->ctx_))));
            return;
        }
        if (operation == PlaybackOperation::Flush) {
            self->applied_playback_buffer_revision_ = self->desired_.playback_buffer_revision;
        }
        self->reconcile_playback();
    }

    static void on_write(pulse_ffi::pa_stream* stream, size_t bytes, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (bytes == 0 || self->desc_.channels == u32()) return;

        pulse_ffi::pa_usec_t usec = 0;
        if (self->api_->pa_stream_get_time(stream, &usec) >= 0) {
            self->stream_position_frames_.store(
                u64(usec) * u64(self->desc_.sample_rate.to_primitive()) / u64(1'000'000),
                rstd::sync::atomic::Ordering::Relaxed);
        }

        const auto channel_count = self->desc_.channels.to_primitive();
        const auto stride        = channel_count * static_cast<rstd::uint32_t>(sizeof(float));
        size_t     remaining     = bytes;
        while (remaining > 0) {
            void*  buffer = nullptr;
            size_t wanted = remaining;
            if (self->api_->pa_stream_begin_write(stream, &buffer, &wanted) < 0 || ! buffer ||
                wanted == 0) {
                return;
            }

            const auto frames       = static_cast<rstd::uint32_t>(wanted / stride);
            const auto sample_count = static_cast<rstd::size_t>(frames) * channel_count;
            auto*      output       = static_cast<float*>(buffer);
            rstd::mem::memset(output, u8(), usize(sample_count * sizeof(float)));

            if (! self->muted_) {
                self->scratch_.resize(usize(sample_count), 0.0f);
                for (auto& channel : self->channels_) {
                    rstd::mem::memset(
                        self->scratch_.data(), u8(), usize(sample_count * sizeof(float)));
                    const auto produced = channel->next_pcm(self->scratch_.data(), u32(frames));
                    const auto produced_samples =
                        static_cast<rstd::size_t>(produced.to_primitive()) * channel_count;
                    for (rstd::size_t index = 0; index < produced_samples; ++index) {
                        output[index] += self->scratch_[usize(index)];
                    }
                }
                self->apply_output_gain(output, frames);
            }

            const size_t written = static_cast<size_t>(frames) * stride;
            if (self->api_->pa_stream_write(
                    stream, buffer, written, nullptr, 0, pulse_ffi::seek_relative) < 0) {
                return;
            }
            if (written == 0) return;
            remaining = remaining > written ? remaining - written : 0;
        }
    }

    const pulse_ffi::Api*            api_    = nullptr;
    pulse_ffi::pa_threaded_mainloop* loop_   = nullptr;
    pulse_ffi::pa_context*           ctx_    = nullptr;
    pulse_ffi::pa_stream*            stream_ = nullptr;

    AudioDeviceDesiredState desired_;
    DeviceDesc              desc_ { u32(kDefaultChannels), u32(kDefaultRate) };
    u64                     stream_revision_;
    u64                     volume_scale_revision_;
    u64                     applied_playback_buffer_revision_;
    bool                    shutting_down_ {};
    PlaybackOperation       playback_operation_ { PlaybackOperation::None };
    pulse_ffi::pa_stream*   playback_operation_stream_ = nullptr;

    Vec<std::unique_ptr<IPullChannel>> channels_;
    Vec<float>                         scratch_;

    f32                     volume_ { f32(1.0f) };
    detail::VolumeScaleRamp volume_scale_;
    bool                    muted_ {};

    rstd::sync::Mutex<CommandQueue>              commands_;
    rstd::sync::Mutex<AudioDeviceEventSink>      event_sink_;
    rstd::sync::Mutex<bool>                      shutdown_complete_;
    rstd::sync::Condvar                          shutdown_cv_;
    rstd::sync::atomic::Atomic<AudioDeviceState> state_ { AudioDeviceState::Idle };
    rstd::sync::atomic::Atomic<u64>              stream_position_frames_ { u64() };
};

AudioDevice::AudioDevice(): impl_(Box<Impl>::make()) {}
AudioDevice::~AudioDevice() = default;

void AudioDevice::set_event_sink(AudioDeviceEventSink sink) {
    impl_->set_event_sink(rstd::move(sink));
}
bool AudioDevice::apply(AudioDeviceDesiredState desired) {
    return impl_->apply(rstd::move(desired));
}
bool AudioDevice::mount(std::unique_ptr<IPullChannel> channel, u64 stream_revision) {
    return impl_->mount(rstd::move(channel), stream_revision);
}
bool AudioDevice::unmount_all(u64 stream_revision) { return impl_->unmount_all(stream_revision); }
void AudioDevice::shutdown() { impl_->shutdown(); }
void AudioDevice::wait_stopped() { impl_->wait_stopped(); }
AudioDeviceState AudioDevice::state() const { return impl_->state(); }
DeviceDesc       AudioDevice::desc() const { return impl_->desc(); }
auto AudioDevice::stream_position_frames() const -> u64 { return impl_->stream_position_frames(); }

} // namespace wavsen::audio
#else
import rstd.cppstd;
import rstd;
import rstd.log;
import wavsen.audio.gain;
import :core;

using namespace rstd::prelude;

namespace wavsen::audio
{

namespace
{

constexpr rstd::uint32_t kDefaultRate     = 48000;
constexpr rstd::uint32_t kDefaultChannels = 2;
constexpr rstd::uint32_t kQuantum         = 1024;

} // namespace

class AudioDevice::Impl {
public:
    Impl() {
        playback_ = wavsen_apple_create_audio_playback(kDefaultRate,
                                                       kDefaultChannels,
                                                       &Impl::on_audio,
                                                       this);
        if (playback_ == nullptr) {
            rstd::log::error("wavsen::audio: failed to create CoreAudio output unit");
            return;
        }
        scratch_.reserve(static_cast<std::size_t>(kQuantum) * kDefaultChannels);
    }

    ~Impl() {
        shutdown();
        if (playback_ != nullptr) {
            wavsen_apple_destroy_audio_playback(playback_);
            playback_ = nullptr;
        }
    }

    void set_event_sink(AudioDeviceEventSink sink) {
        auto current = event_sink_.lock().unwrap_unchecked();
        *current     = rstd::move(sink);
    }

    bool apply(AudioDeviceDesiredState desired) {
        if (playback_ == nullptr) return false;

        bool start = false;
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (shutting_down_) return false;

            if (desired.generation != desired_.generation) stop_locked();
            desired_ = rstd::move(desired);
            active   = desired_.active;
            playing_ = desired_.playing;
            muted_   = desired_.muted;
            apply_gain_state();

            if (! active) {
                stop_locked();
            } else {
                const bool running = wavsen_apple_audio_playback_is_running(playback_) != 0;
                // PulseAudio/PipeWire flush their queued samples when the
                // playback buffer revision changes. CoreAudio's pull unit
                // has no equivalent queue API; restarting the output unit
                // discards any already-rendered block before the new stream
                // is pulled, preserving the same observable semantics.
                if (running &&
                    desired_.playback_buffer_revision != applied_playback_buffer_revision_) {
                    stop_locked();
                    start = true;
                } else {
                    start = ! running;
                }
            }
        }

        if (! active) {
            emit_state(AudioDeviceState::Idle);
            return true;
        }

        if (start) {
            emit_state(AudioDeviceState::Connecting);
            int status = 0;
            {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                if (shutting_down_ || ! desired_.active) return false;
                for (auto& channel : channels_) channel->pass_desc(desc_);
                status = wavsen_apple_start_audio_playback(playback_);
                if (status == 0) {
                    applied_playback_buffer_revision_ = desired_.playback_buffer_revision;
                } else {
                    stop_locked();
                }
            }
            if (status != 0) {
                fail(rstd::format("CoreAudio output unit start failed ({})", status));
                return false;
            }
        }

        bool is_playing = false;
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            is_playing = desired_.playing;
        }
        emit_state(is_playing ? AudioDeviceState::ReadyPlaying
                              : AudioDeviceState::ReadyPaused);
        return true;
    }

    bool mount(std::unique_ptr<IPullChannel> channel, u64 stream_revision) {
        if (! channel || playback_ == nullptr) return false;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (shutting_down_) return false;
        if (stream_revision < stream_revision_) return true;
        stream_revision_ = stream_revision;
        if (wavsen_apple_audio_playback_is_running(playback_) != 0) {
            channel->pass_desc(desc_);
        }
        channels_.push(rstd::move(channel));
        return true;
    }

    bool unmount_all(u64 stream_revision) {
        if (playback_ == nullptr) return false;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (shutting_down_) return false;
        if (stream_revision < stream_revision_) return true;
        stream_revision_ = stream_revision;
        channels_.clear();
        return true;
    }

    void shutdown() {
        bool notify = false;
        u64 generation = u64();
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (shutting_down_) return;
            shutting_down_ = true;
            generation = desired_.generation;
            stop_locked();
            channels_.clear();
            notify = true;
        }
        if (notify) {
            emit_state(AudioDeviceState::Stopped, {}, generation);
        }
    }

    void wait_stopped() {}

    AudioDeviceState state() const {
        return state_.load(rstd::sync::atomic::Ordering::Acquire);
    }

    DeviceDesc desc() const { return desc_; }

    u64 stream_position_frames() const {
        return stream_position_frames_.load(rstd::sync::atomic::Ordering::Relaxed);
    }

private:
    void apply_gain_state() {
        if (desired_.volume_scale_revision == volume_scale_revision_) {
            volume_ = desired_.volume;
            muted_  = desired_.muted;
            return;
        }
        volume_ = desired_.volume;
        muted_  = desired_.muted;
        volume_scale_revision_ = desired_.volume_scale_revision;
        volume_scale_.redirect(
            desired_.volume_scale, desc_.sample_rate, desired_.volume_scale_fade_ms);
    }

    void stop_locked() {
        if (playback_ != nullptr) wavsen_apple_stop_audio_playback(playback_);
        applied_playback_buffer_revision_ = u64();
        stream_position_frames_.store(u64(), rstd::sync::atomic::Ordering::Relaxed);
    }

    void fail(String error) {
        rstd::log::error("wavsen::audio: {}", error);
        emit_state(AudioDeviceState::Failed, rstd::move(error));
    }

    void emit_state(AudioDeviceState state, String error = {}, u64 generation = u64(-1)) {
        state_.store(state, rstd::sync::atomic::Ordering::Release);
        AudioDeviceEventSink sink;
        {
            auto current = event_sink_.lock().unwrap_unchecked();
            sink         = *current;
        }
        if (sink) {
            if (generation == u64(-1)) {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                generation = desired_.generation;
            }
            sink(AudioDeviceEvent { generation, state, rstd::move(error) });
        }
    }

    void apply_output_gain(float* output, rstd::uint32_t frames) {
        volume_scale_.apply(
            rstd::mut_ref<float[]>::from_raw_parts(
                output, rstd::usize(frames) * rstd::usize(desc_.channels.to_primitive())),
            desc_.channels,
            volume_);
    }

    static void on_audio(float* output,
                         rstd::uint32_t frames,
                         rstd::uint32_t channels,
                         void* user) {
        static_cast<Impl*>(user)->render(output, frames, channels);
    }

    void render(float* output, rstd::uint32_t frames, rstd::uint32_t channels) {
        if (output == nullptr || channels != desc_.channels.to_primitive()) return;

        const auto sample_count = static_cast<std::size_t>(frames) * channels;
        std::memset(output, 0, sample_count * sizeof(float));

        std::unique_lock<std::mutex> lock(audio_mutex_, std::try_to_lock);
        if (! lock.owns_lock() || ! playing_) return;

        stream_position_frames_.fetch_add(
            u64(frames), rstd::sync::atomic::Ordering::Relaxed);
        if (muted_) return;

        scratch_.resize(sample_count);
        for (auto& channel : channels_) {
            std::memset(scratch_.data(), 0, sample_count * sizeof(float));
            auto produced = channel->next_pcm(scratch_.data(), u32(frames)).to_primitive();
            if (produced > frames) produced = frames;
            const auto produced_samples = static_cast<std::size_t>(produced) * channels;
            for (std::size_t index = 0; index < produced_samples; ++index) {
                output[index] += scratch_[index];
            }
        }
        apply_output_gain(output, frames);
    }

    WavsenAppleAudioPlayback* playback_ = nullptr;
    AudioDeviceDesiredState     desired_;
    DeviceDesc                  desc_ { u32(kDefaultChannels), u32(kDefaultRate) };
    u64                         stream_revision_;
    u64                         volume_scale_revision_;
    u64                         applied_playback_buffer_revision_;
    bool                        shutting_down_ {};
    bool                        playing_ {};
    bool                        muted_ {};

    Vec<std::unique_ptr<IPullChannel>> channels_;
    std::vector<float>                 scratch_;
    std::mutex                         audio_mutex_;

    f32                     volume_ { f32(1.0f) };
    detail::VolumeScaleRamp volume_scale_;

    rstd::sync::Mutex<AudioDeviceEventSink> event_sink_ { AudioDeviceEventSink {} };
    rstd::sync::atomic::Atomic<AudioDeviceState> state_ { AudioDeviceState::Idle };
    rstd::sync::atomic::Atomic<u64> stream_position_frames_ { u64() };
};

AudioDevice::AudioDevice(): impl_(Box<Impl>::make()) {}
AudioDevice::~AudioDevice() = default;

void AudioDevice::set_event_sink(AudioDeviceEventSink sink) { impl_->set_event_sink(rstd::move(sink)); }
bool AudioDevice::apply(AudioDeviceDesiredState desired) { return impl_->apply(rstd::move(desired)); }
bool AudioDevice::mount(std::unique_ptr<IPullChannel> channel, u64 stream_revision) {
    return impl_->mount(rstd::move(channel), stream_revision);
}
bool AudioDevice::unmount_all(u64 stream_revision) { return impl_->unmount_all(stream_revision); }
void AudioDevice::shutdown() { impl_->shutdown(); }
void AudioDevice::wait_stopped() { impl_->wait_stopped(); }
AudioDeviceState AudioDevice::state() const { return impl_->state(); }
DeviceDesc       AudioDevice::desc() const { return impl_->desc(); }
auto AudioDevice::stream_position_frames() const -> u64 { return impl_->stream_position_frames(); }

} // namespace wavsen::audio
#endif
