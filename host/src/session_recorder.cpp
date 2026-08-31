// RemoteX — host/session_recorder.cpp
// Grava sessão remota em MP4 via FFmpeg (H.264).

#include "host/session_recorder.h"
#include "host/recording_utils.h"
#include "host/runtime_paths.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace remotex::host {

#ifdef REMOTEX_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// ─── Pimpl: contextos FFmpeg ─────────────────────────────────────────────────

struct SessionRecorder::Impl {
    AVFormatContext* fmt_ctx   = nullptr;
    AVCodecContext*  codec_ctx = nullptr;
    AVStream*        stream    = nullptr;
    SwsContext*      sws_ctx   = nullptr;
    AVFrame*         frame     = nullptr;
    AVPacket*        packet    = nullptr;
    RecordingFrameLayout layout{};
    int source_width  = 0;
    int source_height = 0;

    bool EnsureScaler(int source_width, int source_height, int canvas_width, int canvas_height);
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void LogFFmpeg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stderr, "[SessionRecorder] %s\n", buf);
}

static void FillFrameBlack(AVFrame* frame) {
    if (!frame) {
        return;
    }

    for (int y = 0; y < frame->height; ++y) {
        std::memset(frame->data[0] + (y * frame->linesize[0]), 16, frame->width);
    }
    for (int y = 0; y < frame->height / 2; ++y) {
        std::memset(frame->data[1] + (y * frame->linesize[1]), 128, frame->width / 2);
        std::memset(frame->data[2] + (y * frame->linesize[2]), 128, frame->width / 2);
    }
}

bool SessionRecorder::Impl::EnsureScaler(int source_width,
                                         int source_height,
                                         int canvas_width,
                                         int canvas_height) {
    if (source_width <= 0 || source_height <= 0 || canvas_width <= 0 || canvas_height <= 0) {
        return false;
    }

    const RecordingFrameLayout next_layout =
        ComputeRecordingFrameLayout(source_width, source_height, canvas_width, canvas_height);
    if (next_layout.width <= 0 || next_layout.height <= 0) {
        return false;
    }

    if (sws_ctx &&
        this->source_width == source_width &&
        this->source_height == source_height &&
        layout.width == next_layout.width &&
        layout.height == next_layout.height) {
        return true;
    }

    sws_freeContext(sws_ctx);
    sws_ctx = sws_getContext(
        source_width, source_height, AV_PIX_FMT_BGRA,
        next_layout.width, next_layout.height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        this->source_width = 0;
        this->source_height = 0;
        layout = {};
        return false;
    }

    this->source_width = source_width;
    this->source_height = source_height;
    layout = next_layout;
    return true;
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

SessionRecorder::SessionRecorder() = default;

SessionRecorder::~SessionRecorder() {
    if (recording_.load()) {
        Stop();
    }
}

// ─── Start ──────────────────────────────────────────────────────────────────

bool SessionRecorder::Start(const RecorderConfig& config) {
    if (recording_.load()) return false;
    if (config.width <= 0 || config.height <= 0) return false;

    config_ = config;
    frame_count_ = 0;
    stop_requested_.store(false);

    const auto output_dir = EnsureDirectoryExists(config_.output_dir,
                                                  "recording output directory");
    if (!output_dir.ok()) {
        LogFFmpeg("%s", output_dir.error.c_str());
        return false;
    }

    // Nome do arquivo: timestamp.mp4
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    output_path_ = output_dir.path / (std::to_string(epoch) + ".mp4");

    impl_ = std::make_unique<Impl>();
    auto cleanup_partial_start = [this]() {
        if (!impl_) {
            return;
        }
        sws_freeContext(impl_->sws_ctx);
        av_frame_free(&impl_->frame);
        av_packet_free(&impl_->packet);
        avcodec_free_context(&impl_->codec_ctx);
        if (impl_->fmt_ctx) {
            if (!(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE) && impl_->fmt_ctx->pb) {
                avio_closep(&impl_->fmt_ctx->pb);
            }
            avformat_free_context(impl_->fmt_ctx);
        }
        impl_.reset();
    };

    // Aloca output context.
    int ret = avformat_alloc_output_context2(&impl_->fmt_ctx, nullptr, nullptr,
                                              output_path_.string().c_str());
    if (ret < 0 || !impl_->fmt_ctx) {
        LogFFmpeg("Failed to create output context");
        cleanup_partial_start();
        return false;
    }

    // Tenta h264_nvenc primeiro, depois libx264.
    const AVCodec* codec = nullptr;
    if (config_.use_hw_encoder) {
        codec = avcodec_find_encoder_by_name("h264_nvenc");
    }
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libx264");
    }
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        LogFFmpeg("No H.264 encoder found");
        avformat_free_context(impl_->fmt_ctx);
        impl_.reset();
        return false;
    }
    LogFFmpeg("Using encoder: %s", codec->name);

    // Stream.
    impl_->stream = avformat_new_stream(impl_->fmt_ctx, nullptr);
    if (!impl_->stream) {
        LogFFmpeg("Failed to create stream");
        avformat_free_context(impl_->fmt_ctx);
        impl_.reset();
        return false;
    }

    // Codec context.
    impl_->codec_ctx = avcodec_alloc_context3(codec);
    impl_->codec_ctx->codec_id  = AV_CODEC_ID_H264;
    impl_->codec_ctx->bit_rate  = config_.bitrate;
    impl_->codec_ctx->width     = config_.width;
    impl_->codec_ctx->height    = config_.height;
    impl_->codec_ctx->time_base = {1, config_.fps};
    impl_->codec_ctx->framerate = {config_.fps, 1};
    impl_->codec_ctx->gop_size  = config_.fps * 2;   // keyframe every 2 seconds
    impl_->codec_ctx->max_b_frames = 0;
    impl_->codec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    impl_->codec_ctx->thread_count = 2;

    if (impl_->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        impl_->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Opções para libx264.
    if (std::string(codec->name) == "libx264") {
        av_opt_set(impl_->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(impl_->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(impl_->codec_ctx, codec, nullptr);
    if (ret < 0) {
        // Se nvenc falhou, tenta libx264 como fallback.
        if (config_.use_hw_encoder && std::string(codec->name) != "libx264") {
            LogFFmpeg("HW encoder failed, trying libx264 fallback");
            avcodec_free_context(&impl_->codec_ctx);

            codec = avcodec_find_encoder_by_name("libx264");
            if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                LogFFmpeg("No fallback encoder found");
                avformat_free_context(impl_->fmt_ctx);
                impl_.reset();
                return false;
            }

            impl_->codec_ctx = avcodec_alloc_context3(codec);
            impl_->codec_ctx->codec_id  = AV_CODEC_ID_H264;
            impl_->codec_ctx->bit_rate  = config_.bitrate;
            impl_->codec_ctx->width     = config_.width;
            impl_->codec_ctx->height    = config_.height;
            impl_->codec_ctx->time_base = {1, config_.fps};
            impl_->codec_ctx->framerate = {config_.fps, 1};
            impl_->codec_ctx->gop_size  = config_.fps * 2;
            impl_->codec_ctx->max_b_frames = 0;
            impl_->codec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
            impl_->codec_ctx->thread_count = 2;

            if (impl_->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                impl_->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            av_opt_set(impl_->codec_ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(impl_->codec_ctx->priv_data, "tune", "zerolatency", 0);

            ret = avcodec_open2(impl_->codec_ctx, codec, nullptr);
            if (ret < 0) {
                LogFFmpeg("Fallback encoder also failed");
                avcodec_free_context(&impl_->codec_ctx);
                avformat_free_context(impl_->fmt_ctx);
                impl_.reset();
                return false;
            }
            LogFFmpeg("Using fallback encoder: %s", codec->name);
        } else {
            LogFFmpeg("Failed to open encoder");
            avcodec_free_context(&impl_->codec_ctx);
            avformat_free_context(impl_->fmt_ctx);
            impl_.reset();
            return false;
        }
    }

    avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codec_ctx);
    impl_->stream->time_base = impl_->codec_ctx->time_base;

    // Abre arquivo de saída.
    if (!(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&impl_->fmt_ctx->pb, output_path_.string().c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LogFFmpeg("Failed to open output file");
            avcodec_free_context(&impl_->codec_ctx);
            avformat_free_context(impl_->fmt_ctx);
            impl_.reset();
            return false;
        }
    }

    ret = avformat_write_header(impl_->fmt_ctx, nullptr);
    if (ret < 0) {
        LogFFmpeg("Failed to write header");
        avio_closep(&impl_->fmt_ctx->pb);
        avcodec_free_context(&impl_->codec_ctx);
        avformat_free_context(impl_->fmt_ctx);
        impl_.reset();
        return false;
    }

    // Aloca frame e packet.
    impl_->frame = av_frame_alloc();
    if (!impl_->frame) {
        LogFFmpeg("Failed to allocate encoder frame");
        cleanup_partial_start();
        return false;
    }
    impl_->frame->format = AV_PIX_FMT_YUV420P;
    impl_->frame->width  = config_.width;
    impl_->frame->height = config_.height;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) {
        LogFFmpeg("Failed to allocate encoder frame buffer");
        cleanup_partial_start();
        return false;
    }

    impl_->packet = av_packet_alloc();
    if (!impl_->packet) {
        LogFFmpeg("Failed to allocate encoder packet");
        cleanup_partial_start();
        return false;
    }

    recording_.store(true);
    last_frame_time_ = std::chrono::steady_clock::time_point{};

    // Inicia thread de encoding.
    encoder_thread_ = std::thread(&SessionRecorder::EncoderThread, this);

    LogFFmpeg("Recording started: %s (%dx%d @ %dfps)",
              output_path_.string().c_str(), config_.width, config_.height, config_.fps);
    return true;
}

// ─── PushFrame ──────────────────────────────────────────────────────────────

void SessionRecorder::PushFrame(const uint8_t* bgra, size_t size,
                                 int width, int height, int pitch) {
    if (!recording_.load()) return;
    if (!bgra || size == 0 || width <= 0 || height <= 0 || pitch <= 0) return;

    // Decimação de framerate.
    auto now = std::chrono::steady_clock::now();
    auto min_interval = std::chrono::milliseconds(1000 / config_.fps);
    if (last_frame_time_.time_since_epoch().count() != 0 &&
        (now - last_frame_time_) < min_interval) {
        return;
    }
    last_frame_time_ = now;

    // Copia frame e enfileira.
    Frame f;
    f.data.assign(bgra, bgra + size);
    f.width  = width;
    f.height = height;
    f.pitch  = pitch;

    {
        std::lock_guard lock(queue_mutex_);
        if (frame_queue_.size() >= kMaxQueueSize) {
            frame_queue_.pop();  // Descarta frame mais antigo.
        }
        frame_queue_.push(std::move(f));
    }
    queue_cv_.notify_one();
}

// ─── Stop ───────────────────────────────────────────────────────────────────

std::filesystem::path SessionRecorder::Stop() {
    if (!recording_.load()) return {};

    stop_requested_.store(true);
    queue_cv_.notify_all();

    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    recording_.store(false);

    if (!impl_) return {};

    // Flush encoder.
    avcodec_send_frame(impl_->codec_ctx, nullptr);
    while (avcodec_receive_packet(impl_->codec_ctx, impl_->packet) == 0) {
        av_packet_rescale_ts(impl_->packet,
                             impl_->codec_ctx->time_base,
                             impl_->stream->time_base);
        impl_->packet->stream_index = impl_->stream->index;
        av_interleaved_write_frame(impl_->fmt_ctx, impl_->packet);
        av_packet_unref(impl_->packet);
    }

    av_write_trailer(impl_->fmt_ctx);

    // Cleanup.
    sws_freeContext(impl_->sws_ctx);
    av_frame_free(&impl_->frame);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->codec_ctx);
    if (!(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&impl_->fmt_ctx->pb);
    }
    avformat_free_context(impl_->fmt_ctx);
    impl_.reset();

    if (frame_count_ > 0) {
        LogFFmpeg("Recording stopped: %s (%lld frames)",
                  output_path_.string().c_str(), static_cast<long long>(frame_count_));
        return output_path_;
    }

    // Nenhum frame gravado — remove arquivo vazio.
    std::error_code ec;
    fs::remove(output_path_, ec);
    return {};
}

// ─── EncoderThread ──────────────────────────────────────────────────────────

void SessionRecorder::EncoderThread() {
    while (true) {
        Frame f;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !frame_queue_.empty() || stop_requested_.load();
            });
            if (frame_queue_.empty() && stop_requested_.load()) break;
            if (frame_queue_.empty()) continue;
            f = std::move(frame_queue_.front());
            frame_queue_.pop();
        }

        // Converte BGRA para YUV420P no canvas fixo da gravação.
        const uint8_t* src_data[1] = { f.data.data() };
        int src_linesize[1] = { f.pitch };

        if (!impl_->EnsureScaler(f.width, f.height, config_.width, config_.height)) {
            LogFFmpeg("Failed to prepare frame scaler for %dx%d input", f.width, f.height);
            continue;
        }

        if (av_frame_make_writable(impl_->frame) < 0) {
            continue;
        }
        FillFrameBlack(impl_->frame);

        std::array<uint8_t*, 4> dst_data = {
            impl_->frame->data[0] + (impl_->layout.offset_y * impl_->frame->linesize[0]) + impl_->layout.offset_x,
            impl_->frame->data[1] + ((impl_->layout.offset_y / 2) * impl_->frame->linesize[1]) + (impl_->layout.offset_x / 2),
            impl_->frame->data[2] + ((impl_->layout.offset_y / 2) * impl_->frame->linesize[2]) + (impl_->layout.offset_x / 2),
            nullptr,
        };
        std::array<int, 4> dst_linesize = {
            impl_->frame->linesize[0],
            impl_->frame->linesize[1],
            impl_->frame->linesize[2],
            0,
        };

        sws_scale(impl_->sws_ctx,
                  src_data, src_linesize,
                  0, f.height,
                  dst_data.data(), dst_linesize.data());

        impl_->frame->pts = frame_count_++;

        // Encode.
        int ret = avcodec_send_frame(impl_->codec_ctx, impl_->frame);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_packet(impl_->codec_ctx, impl_->packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            av_packet_rescale_ts(impl_->packet,
                                 impl_->codec_ctx->time_base,
                                 impl_->stream->time_base);
            impl_->packet->stream_index = impl_->stream->index;
            av_interleaved_write_frame(impl_->fmt_ctx, impl_->packet);
            av_packet_unref(impl_->packet);
        }
    }
}

#else // !REMOTEX_HAS_FFMPEG — stubs no-op

SessionRecorder::SessionRecorder()  = default;
SessionRecorder::~SessionRecorder() = default;

bool SessionRecorder::Start(const RecorderConfig&) {
    fprintf(stderr,
            "[SessionRecorder] Session recording unavailable: host built without FFmpeg support. "
            "Set FFMPEG_ROOT before building the host.\n");
    return false;
}

void SessionRecorder::PushFrame(const uint8_t*, size_t, int, int, int) {}

std::filesystem::path SessionRecorder::Stop() { return {}; }

#endif

} // namespace remotex::host
