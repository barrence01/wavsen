module wavsen.audio;

import rstd;
import rstd.cppstd;
import rstd.log;
import :byte_stream;
import wavsen.audio.core; // DeviceDesc
import :mixer;            // SoundStream
import :file;
import wavsen.ffi.ffmpeg;

using namespace rstd::prelude;

namespace wavsen::audio
{

namespace
{

constexpr rstd::size_t kAvioBuf = 32 * 1024;

int avio_read_cb(void* opaque, rstd::uint8_t* buf, int sz) {
    auto* source = static_cast<ByteStream*>(opaque);
    auto  bytes  = rstd::mut_ref<rstd::byte[]>::from_raw_parts(reinterpret_cast<rstd::byte*>(buf),
                                                               rstd::usize(sz));
    auto  r      = (*source)->read(rstd::as_u8_slice_mut(bytes));
    if (r.is_err()) return AVERROR_EOF;
    auto n = rstd::move(r).unwrap();
    if (n == rstd::usize()) return AVERROR_EOF;
    return static_cast<int>(n.to_primitive());
}

rstd::int64_t avio_seek_cb(void* opaque, rstd::int64_t off, int whence) {
    auto* source = static_cast<ByteStream*>(opaque);
    if (whence == AVSEEK_SIZE) return -1;
    rstd::io::SeekFrom from = (whence == rstd::cppstd::SEEK_FROM_START)
                                  ? rstd::io::SeekFrom::from_start(rstd::as_cast<rstd::u64>(off))
                                  : (whence == rstd::cppstd::SEEK_FROM_CURRENT
                                         ? rstd::io::SeekFrom::from_current(rstd::i64(off))
                                         : rstd::io::SeekFrom::from_end(rstd::i64(off)));
    auto               r    = (*source)->seek(from);
    if (r.is_err()) return -1;
    return static_cast<rstd::int64_t>(rstd::move(r).unwrap().to_primitive());
}

} // namespace

class StreamDecoder::Impl {
public:
    ~Impl() { teardown(); }

    bool open(ByteStream src, const DeviceDesc& target) {
        teardown();
        src_.insert(rstd::move(src));
        target_ = target;

        avio_buf_ = static_cast<rstd::uint8_t*>(av_malloc(kAvioBuf));
        if (! avio_buf_) {
            rstd::log::error("wavsen::audio: av_malloc avio buffer failed");
            return false;
        }

        avio_ = avio_alloc_context(avio_buf_,
                                   kAvioBuf,
                                   /*write_flag=*/0,
                                   /*opaque=*/rstd::addressof(*src_),
                                   &avio_read_cb,
                                   /*write_packet=*/nullptr,
                                   &avio_seek_cb);
        if (! avio_) {
            rstd::log::error("wavsen::audio: avio_alloc_context failed");
            return false;
        }

        fmt_ctx_     = avformat_alloc_context();
        fmt_ctx_->pb = avio_;
        fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr) != 0) {
            rstd::log::error("wavsen::audio: avformat_open_input failed");
            // avformat_open_input frees fmt_ctx_ on failure.
            fmt_ctx_ = nullptr;
            return false;
        }
        if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
            rstd::log::error("wavsen::audio: avformat_find_stream_info failed");
            return false;
        }

        const AVCodec* dec  = nullptr;
        int            sidx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
        if (sidx < 0 || ! dec) {
            rstd::log::error("wavsen::audio: no audio stream / codec");
            return false;
        }
        stream_idx_ = sidx;

        cctx_ = avcodec_alloc_context3(dec);
        if (! cctx_) {
            rstd::log::error("wavsen::audio: avcodec_alloc_context3 failed");
            return false;
        }
        if (avcodec_parameters_to_context(cctx_, fmt_ctx_->streams[sidx]->codecpar) < 0) {
            rstd::log::error("wavsen::audio: avcodec_parameters_to_context failed");
            return false;
        }
        cctx_->pkt_timebase = fmt_ctx_->streams[sidx]->time_base;
        if (avcodec_open2(cctx_, dec, nullptr) < 0) {
            rstd::log::error("wavsen::audio: avcodec_open2 failed");
            return false;
        }

        pkt_   = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (! pkt_ || ! frame_) {
            rstd::log::error("wavsen::audio: alloc packet/frame failed");
            return false;
        }

        return setup_resampler();
    }

    void retarget(const DeviceDesc& target) {
        if (cctx_ == nullptr) {
            target_ = target;
            return;
        }
        if (target.channels == target_.channels && target.sample_rate == target_.sample_rate) {
            return;
        }
        SwrContext* next = nullptr;
        if (! build_resampler(target, playback_rate_, &next)) return;
        if (swr_) swr_free(&swr_);
        swr_    = next;
        target_ = target;
        // Drop any pending resampled frames; the new format is incompatible.
        pending_offset_ = u32();
        pending_frames_ = u32();
    }

    auto set_playback_rate(f64 rate) -> bool {
        if (! rate.is_finite() || rate <= f64()) return false;
        if (rate == playback_rate_) return true;
        if (cctx_ == nullptr) {
            playback_rate_ = rate;
            return true;
        }

        SwrContext* next = nullptr;
        if (! build_resampler(target_, rate, &next)) return false;
        if (swr_) swr_free(&swr_);
        swr_            = next;
        playback_rate_  = rate;
        pending_offset_ = u32();
        pending_frames_ = u32();
        return true;
    }

    auto playback_rate() const -> f64 { return playback_rate_; }

    auto next_pcm(void* dst, u32 frames) -> u64 {
        if (cctx_ == nullptr || swr_ == nullptr) return u64();

        auto*      out      = static_cast<rstd::uint8_t*>(dst);
        const auto bps      = usize(sizeof(float)) * usize(target_.channels.to_primitive());
        auto       produced = u32();

        while (produced < frames) {
            // Flush any leftover from prior decode round.
            if (pending_frames_ > u32()) {
                const auto take = (frames - produced).min(pending_frames_);
                rstd::mem::memcpy(out + (usize(produced.to_primitive()) * bps).to_primitive(),
                                  pending_buf_.data() +
                                      (usize(pending_offset_.to_primitive()) * bps).to_primitive(),
                                  usize(take.to_primitive()) * bps);
                produced += take;
                pending_offset_ += take;
                pending_frames_ -= take;
                continue;
            }

            // Need a fresh decoded frame.
            if (! pull_decoded_frame()) {
                if (eof_) break;
                continue; // try again (decode may still be priming)
            }

            // Resample the new frame into pending_buf_.
            const rstd::int64_t max_out = swr_get_out_samples(swr_, frame_->nb_samples);
            if (max_out <= 0) continue;

            const auto need_bytes = usize(static_cast<rstd::size_t>(max_out)) * bps;
            if (pending_buf_.len() < need_bytes) {
                pending_buf_.resize(need_bytes, 0u);
            }
            rstd::uint8_t* out_ptr = pending_buf_.data();
            const int      converted =
                swr_convert(swr_,
                            &out_ptr,
                            static_cast<int>(max_out),
                            const_cast<const rstd::uint8_t**>(frame_->extended_data),
                            frame_->nb_samples);
            if (converted < 0) {
                rstd::log::warn("wavsen::audio: swr_convert error");
                continue;
            }
            pending_offset_ = u32();
            pending_frames_ = u32(converted);
        }

        return u64(produced.to_primitive());
    }

    bool seek_to(f64 seconds) {
        if (! fmt_ctx_ || stream_idx_ < 0) return false;
        const AVStream* st = fmt_ctx_->streams[stream_idx_];
        const auto      tb = st->time_base;
        const auto      ts =
            static_cast<rstd::int64_t>(seconds.to_primitive() / ffi::ffmpeg::av_q2d(tb));
        if (av_seek_frame(fmt_ctx_, stream_idx_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            rstd::log::warn("wavsen::audio: av_seek_frame failed");
            return false;
        }
        if (cctx_) avcodec_flush_buffers(cctx_);
        if (swr_) swr_free(&swr_);
        setup_resampler();
        pending_offset_   = u32();
        pending_frames_   = u32();
        eof_              = false;
        last_pts_seconds_ = seconds;
        return true;
    }

    auto current_pts_seconds() const -> f64 { return last_pts_seconds_; }
    bool is_eof() const { return eof_; }

    auto sample_rate() const -> u32 { return cctx_ ? u32(cctx_->sample_rate) : u32(); }
    auto channels() const -> u32 { return cctx_ ? u32(cctx_->ch_layout.nb_channels) : u32(); }

private:
    bool pull_decoded_frame() {
        for (;;) {
            int rc = avcodec_receive_frame(cctx_, frame_);
            if (rc == 0) {
                // Capture PTS of the freshly decoded frame for clock query.
                const auto pts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                                     ? frame_->best_effort_timestamp
                                     : frame_->pts;
                if (pts != AV_NOPTS_VALUE && fmt_ctx_ && stream_idx_ >= 0) {
                    last_pts_seconds_ =
                        f64(static_cast<double>(pts) *
                            ffi::ffmpeg::av_q2d(fmt_ctx_->streams[stream_idx_]->time_base));
                }
                return true;
            }
            if (rc == AVERROR(EAGAIN)) {
                // Need more input.
                rc = av_read_frame(fmt_ctx_, pkt_);
                if (rc == AVERROR_EOF) {
                    avcodec_send_packet(cctx_, nullptr); // flush
                    eof_ = true;
                    continue;
                }
                if (rc < 0) {
                    rstd::log::warn("wavsen::audio: av_read_frame error");
                    eof_ = true;
                    return false;
                }
                if (pkt_->stream_index != stream_idx_) {
                    av_packet_unref(pkt_);
                    continue;
                }
                avcodec_send_packet(cctx_, pkt_);
                av_packet_unref(pkt_);
                continue;
            }
            if (rc == AVERROR_EOF) {
                eof_ = true;
                return false;
            }
            rstd::log::warn("wavsen::audio: avcodec_receive_frame error");
            return false;
        }
    }

    bool build_resampler(const DeviceDesc& target, f64 rate, SwrContext** result) {
        const auto effective_input_rate = f64(cctx_->sample_rate) * rate;
        if (! effective_input_rate.is_finite() || effective_input_rate < f64(1.0) ||
            effective_input_rate > f64(i32::MAX.to_primitive())) {
            rstd::log::error("wavsen::audio: playback rate produces an invalid sample rate");
            return false;
        }

        AVChannelLayout out_layout {};
        av_channel_layout_default(&out_layout, static_cast<int>(target.channels.to_primitive()));

        AVChannelLayout in_layout {};
        if (cctx_->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_copy(&in_layout, &cctx_->ch_layout);
        } else {
            av_channel_layout_default(&in_layout, cctx_->ch_layout.nb_channels);
        }

        SwrContext* next = nullptr;
        if (swr_alloc_set_opts2(&next,
                                &out_layout,
                                AV_SAMPLE_FMT_FLT,
                                static_cast<int>(target.sample_rate.to_primitive()),
                                &in_layout,
                                cctx_->sample_fmt,
                                static_cast<int>(effective_input_rate.to_primitive() + 0.5),
                                /*log_offset=*/0,
                                /*log_ctx=*/nullptr) < 0) {
            av_channel_layout_uninit(&out_layout);
            av_channel_layout_uninit(&in_layout);
            rstd::log::error("wavsen::audio: swr_alloc_set_opts2 failed");
            return false;
        }
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&in_layout);

        if (swr_init(next) < 0) {
            rstd::log::error("wavsen::audio: swr_init failed");
            swr_free(&next);
            return false;
        }
        *result = next;
        return true;
    }

    bool setup_resampler() {
        SwrContext* next = nullptr;
        if (! build_resampler(target_, playback_rate_, &next)) return false;
        if (swr_) swr_free(&swr_);
        swr_ = next;
        return true;
    }

    void teardown() {
        if (swr_) swr_free(&swr_);
        if (frame_) av_frame_free(&frame_);
        if (pkt_) av_packet_free(&pkt_);
        if (cctx_) avcodec_free_context(&cctx_);
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
        if (avio_) {
            // avio_buf_ is owned by avio_; avio_context_free handles it iff
            // we set it. To be safe, free both explicitly.
            av_freep(&avio_->buffer);
            avio_context_free(&avio_);
        }
        avio_buf_ = nullptr;
        (void)src_.take();
        pending_offset_   = u32();
        pending_frames_   = u32();
        eof_              = false;
        last_pts_seconds_ = f64();
        playback_rate_    = f64(1.0);
    }

    Option<ByteStream> src_;
    DeviceDesc         target_ {};

    rstd::uint8_t*   avio_buf_   = nullptr;
    AVIOContext*     avio_       = nullptr;
    AVFormatContext* fmt_ctx_    = nullptr;
    AVCodecContext*  cctx_       = nullptr;
    SwrContext*      swr_        = nullptr;
    AVPacket*        pkt_        = nullptr;
    AVFrame*         frame_      = nullptr;
    int              stream_idx_ = -1;

    Vec<rstd::uint8_t> pending_buf_;
    u32                pending_offset_;
    u32                pending_frames_;
    bool               eof_ {};
    f64                last_pts_seconds_;
    f64                playback_rate_ { 1.0 };
};

StreamDecoder::StreamDecoder(): impl_(Box<Impl>::make()) {}
StreamDecoder::~StreamDecoder()                                   = default;
StreamDecoder::StreamDecoder(StreamDecoder&&) noexcept            = default;
StreamDecoder& StreamDecoder::operator=(StreamDecoder&&) noexcept = default;

bool StreamDecoder::open(ByteStream src, const DeviceDesc& target) {
    return impl_->open(rstd::move(src), target);
}

void StreamDecoder::retarget(const DeviceDesc& target) { impl_->retarget(target); }

auto StreamDecoder::set_playback_rate(f64 rate) -> bool { return impl_->set_playback_rate(rate); }

auto StreamDecoder::playback_rate() const -> f64 { return impl_->playback_rate(); }

auto StreamDecoder::next_pcm(void* dst, u32 frames) -> u64 { return impl_->next_pcm(dst, frames); }

bool StreamDecoder::seek_to(f64 seconds) { return impl_->seek_to(seconds); }
auto StreamDecoder::current_pts_seconds() const -> f64 { return impl_->current_pts_seconds(); }
bool StreamDecoder::is_eof() const { return impl_->is_eof(); }
auto StreamDecoder::sample_rate() const -> u32 { return impl_->sample_rate(); }
auto StreamDecoder::channels() const -> u32 { return impl_->channels(); }

namespace
{

// SoundStream backed by libav* decoder. Created via make_stream(); the
// CubebDevice pulls PCM through next_pcm in the audio thread.
class DecoderStream : public SoundStream {
public:
    explicit DecoderStream(StreamDecoder dec): dec_(rstd::move(dec)) {}

    auto next_pcm(void* dst, u32 frames) -> u64 override { return dec_.next_pcm(dst, frames); }
    void pass_desc(const Desc& d) override { dec_.retarget({ d.channels, d.sample_rate }); }

private:
    StreamDecoder dec_;
};

} // namespace

auto make_stream(ByteStream source, const SoundStream::Desc& desc) -> std::unique_ptr<SoundStream> {
    StreamDecoder dec;
    if (! dec.open(rstd::move(source), { desc.channels, desc.sample_rate })) {
        return nullptr;
    }
    return std::make_unique<DecoderStream>(rstd::move(dec));
}

} // namespace wavsen::audio
