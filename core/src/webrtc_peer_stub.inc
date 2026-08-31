#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <chrono>
#include <cstring>
#include <zlib.h>
#include "core/video_encoder.h"

#ifdef REMOTEX_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace remotex::core {

// Stub WebRTC com relay de vídeo via TCP socket.
// Tanto Host quanto Viewer conectam ao servidor de relay (VPS).
// O relay pareia conexões por session_id e faz bridge bidirecional.
//
// Protocolo:
//   1. Conectar ao relay TCP (relay_host:relay_port)
//   2. Enviar "host:<session_id>\n" ou "viewer:<session_id>\n"
//   3. Aguardar "OK\n" (relay pareou host+viewer)
//   4. Enviar/receber frames com header de 16 bytes + dados BGRA

class WebRTCPeerImpl {
public:
    static constexpr uint8_t kPacketTypeFrame = 0x01;
    static constexpr uint8_t kPacketTypeDataChannel = 0x02;
    static constexpr uint8_t kDataChannelFlagBinary = 0x01;
    static constexpr const char* kLaneVideo = "video";
    static constexpr const char* kLaneRealtime = "realtime";
    static constexpr const char* kLaneBulk = "bulk";
    static constexpr const char* kFilesChannelLabel = "files";
    static constexpr auto kRealtimePriorityWindow = std::chrono::milliseconds(250);

    void SetListener(WebRTCPeerListener* l) { listener_ = l; }

    bool Initialize(const PeerConfig& config) {
        StopTransport(false);

        config_ = config;
        state_ = PeerConnectionState::New;
        shutdown_requested_.store(false);
        using_pipe_ = false;
        remote_desc_ = {};
        while (frame_slot_.TryDequeue().has_value()) {
        }
        frame_slot_.ResetDropCount();
        bulk_available_.store(false);
        last_realtime_send_tick_ms_.store(0);
        target_video_fps_ = 30;
        codec_probed_ = false;
        session_codec_ = remotex::core::SessionCodec::FallbackI420;
        encode_quality_.reset();
        CleanupVideoCodecState();

        // Inicializar Winsock (idempotente).
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        return true;
    }

    void Shutdown() {
        StopTransport(true);
    }

    void CreateOffer() {
        if (!listener_) return;
        SessionDescription desc;
        desc.type = "offer";
        desc.sdp  = GenerateStubSDP("offer");
        listener_->OnLocalDescription(desc);
    }

    void CreateAnswer() {
        if (!listener_) return;
        SessionDescription desc;
        desc.type = "answer";
        desc.sdp  = GenerateStubSDP("answer");
        listener_->OnLocalDescription(desc);
        TransitionToConnected();
    }

    bool SetRemoteDescription(const SessionDescription& desc) {
        remote_desc_ = desc;
        if (desc.type == "answer")
            TransitionToConnected();
        return true;
    }

    bool AddIceCandidate(const IceCandidate&) { return true; }

    bool AddVideoTrack(int w, int h, int fps) {
        video_width_  = w;
        video_height_ = h;
        target_video_fps_ = std::clamp(fps > 0 ? fps : 30, 1, 30);
        return true;
    }

    void UpdateVideoDimensions(int w, int h) {
        const uint32_t new_width = static_cast<uint32_t>(w);
        const uint32_t new_height = static_cast<uint32_t>(h);
        if (video_width_ != new_width || video_height_ != new_height) {
            CleanupVideoEncoder();
            ResetResolutionAdaptation();
            resolution_step_changed_at_ = std::chrono::steady_clock::time_point{};
        }
        video_width_  = new_width;
        video_height_ = new_height;
    }

    bool SendVideoFrame(const uint8_t* data,
                        size_t size,
                        int64_t ts,
                        const VideoFrameMetrics& metrics) {
        remotex::core::FrameSlotData slot_data;
        slot_data.bgra_data.assign(data, data + size);
        slot_data.width = video_width_;
        slot_data.height = video_height_;
        slot_data.pitch = video_height_ > 0
            ? static_cast<uint32_t>(size / video_height_)
            : video_width_ * 4;
        slot_data.metrics = metrics;
        if (slot_data.metrics.capture_ready_timestamp_us == 0) {
            slot_data.metrics.capture_ready_timestamp_us = ts;
        }
        if (slot_data.metrics.send_started_timestamp_us == 0) {
            slot_data.metrics.send_started_timestamp_us = ts;
        }
        slot_data.frame_id = metrics.frame_id;
        frame_slot_.TryEnqueue(std::move(slot_data));
        return true;
    }

    bool CreateDataChannel(const std::string& label, bool, bool) {
        if (listener_)
            listener_->OnDataChannelOpen(label);
        return true;
    }

    bool SendDataChannelMessage(const std::string& label,
                                const uint8_t* data,
                                size_t size,
                                bool is_binary) {
        SOCKET socket_handle = SelectDataSocket(label);
        if (!running_ || state_ != PeerConnectionState::Connected || socket_handle == INVALID_SOCKET) {
            return false;
        }

        if (label == kFilesChannelLabel) {
            ApplyBulkPacing();
        } else {
            last_realtime_send_tick_ms_.store(static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()));
        }

        std::lock_guard lock(data_send_mutex_);
        // Wire format: [1B packet_type][1B label_len][label][1B flags][4B data_len LE][data]
        const uint8_t packet_type = kPacketTypeDataChannel;
        uint8_t label_len = static_cast<uint8_t>(std::min(label.size(), size_t(255)));
        uint8_t flags = is_binary ? kDataChannelFlagBinary : 0;
        uint32_t data_len = static_cast<uint32_t>(size);
        if (!SendAll(socket_handle, &packet_type, 1)) return false;
        if (!SendAll(socket_handle, &label_len, 1)) return false;
        if (!SendAll(socket_handle, label.data(), label_len)) return false;
        if (!SendAll(socket_handle, &flags, 1)) return false;
        if (!SendAll(socket_handle, &data_len, 4)) return false;
        if (data_len > 0 && !SendAll(socket_handle, data, data_len)) return false;
        return true;
    }

    bool SendDataChannelMessage(const std::string& label, const std::string& text) {
        return SendDataChannelMessage(label,
            reinterpret_cast<const uint8_t*>(text.data()), text.size(), false);
    }

    void RestartIce() {}
    bool UpdateIceServers(const std::vector<IceServer>&) { return true; }
    PeerConnectionState GetConnectionState() const { return state_; }
    int GetEncodeQualityStep() const {
        return encode_quality_ ? encode_quality_->GetCurrentStep() : 0;
    }
    bool ApplyViewerResolutionFeedback(double avg_upload_ms,
                                       double dropped_fps,
                                       bool& downscaled,
                                       int& step,
                                       double& scale_factor) {
        if (!resolution_adaptation_) {
            return false;
        }

        resolution_adaptation_->UpdateViewerMetrics(avg_upload_ms, dropped_fps);
        const auto now = std::chrono::steady_clock::now();
        if (resolution_adaptation_->ShouldDownscale()) {
            resolution_adaptation_->Downscale(now);
            downscaled = true;
        } else if (resolution_adaptation_->ShouldUpscale(now)) {
            resolution_adaptation_->Upscale(now);
            downscaled = false;
        } else {
            return false;
        }

        step = resolution_adaptation_->GetCurrentStep();
        scale_factor = resolution_adaptation_->GetScaleFactor();
        resolution_step_changed_at_ = now;
        return true;
    }
    int GetResolutionAdaptationStep() const {
        return resolution_adaptation_ ? resolution_adaptation_->GetCurrentStep() : 0;
    }
    double GetResolutionScaleFactor() const {
        return resolution_adaptation_ ? resolution_adaptation_->GetScaleFactor() : 1.0;
    }
    void CleanupVideoEncoder() {
        CleanupVideoEncoderInternal();
    }
    ResolutionAdaptationState* GetResolutionAdaptation() {
        return resolution_adaptation_.get();
    }
    void ResetResolutionAdaptation() {
        if (resolution_adaptation_) {
            resolution_adaptation_->Reset();
        }
    }
    uint64_t ResetFrameDropCount() {
        return frame_slot_.ResetDropCount();
    }

private:
    void StopTransport(bool notify_closed) {
        shutdown_requested_.store(true);
        running_ = false;
        CloseTransportHandles();

        JoinThread(net_thread_);
        JoinThread(dc_thread_);
        JoinThread(bulk_thread_);

        using_pipe_ = false;
        while (frame_slot_.TryDequeue().has_value()) {
        }
        frame_slot_.ResetDropCount();
        bulk_available_.store(false);
        CleanupVideoCodecState();
        ResetResolutionAdaptation();

        if (notify_closed && state_ != PeerConnectionState::Closed) {
            NotifyStateChange(PeerConnectionState::Closed);
        }
    }

    void CloseSocket(SOCKET& socket_handle) {
        if (socket_handle == INVALID_SOCKET) {
            return;
        }
        ::shutdown(socket_handle, SD_BOTH);
        closesocket(socket_handle);
        socket_handle = INVALID_SOCKET;
    }

    void CloseTransportHandles() {
        CloseSocket(video_sock_);
        CloseSocket(data_sock_);
        CloseSocket(bulk_sock_);

        if (pipe_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe_, nullptr);
            if (!config_.is_offerer) {
                DisconnectNamedPipe(pipe_);
            }
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    void JoinThread(std::thread& thread) {
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return;
        }
        thread.join();
    }

    void NotifyStateChange(PeerConnectionState state) {
        state_ = state;
        if (listener_) {
            listener_->OnConnectionStateChanged(state_);
        }
    }

    void MarkTransportConnected() {
        if (shutdown_requested_.load() || !running_) {
            return;
        }
        if (state_ != PeerConnectionState::Connected) {
            NotifyStateChange(PeerConnectionState::Connected);
        }
    }

    void HandleTransportEnded(PeerConnectionState state) {
        if (shutdown_requested_.load()) {
            return;
        }

        const bool was_running = running_.exchange(false);
        CloseTransportHandles();

        if (!was_running &&
            (state_ == PeerConnectionState::Disconnected ||
             state_ == PeerConnectionState::Failed ||
             state_ == PeerConnectionState::Closed)) {
            return;
        }

        if (state_ != PeerConnectionState::Closed && state_ != state) {
            NotifyStateChange(state);
        }
    }

    void HandleBulkTransportEnded() {
        bulk_available_.store(false);
        CloseSocket(bulk_sock_);
    }

    SOCKET SelectDataSocket(const std::string& label) const {
        if (label == kFilesChannelLabel && bulk_available_.load() && bulk_sock_ != INVALID_SOCKET) {
            return bulk_sock_;
        }
        return data_sock_;
    }

    void ApplyBulkPacing() {
        const auto now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const int64_t last_realtime_ms = last_realtime_send_tick_ms_.load();
        if (last_realtime_ms > 0 &&
            now_ms - last_realtime_ms <
                std::chrono::duration_cast<std::chrono::milliseconds>(kRealtimePriorityWindow).count()) {
            Sleep(10);
        }
    }

    enum FrameCodec : uint32_t {
        kFrameCodecI420Raw = 0,
        kFrameCodecI420Zlib = 1,
        kFrameCodecH264 = 2,
    };

#pragma pack(push, 1)
    struct FrameHeader {
        uint32_t width;
        uint32_t height;
        uint32_t codec;
        uint32_t data_size;
        uint32_t aux_size;
        uint64_t frame_id;
        int64_t  timestamp_us;
        int64_t  capture_ready_timestamp_us;
        int64_t  encode_ready_timestamp_us;
        int64_t  send_started_timestamp_us;
    };
#pragma pack(pop)

    template<typename WriteFn>
    bool WriteFramePacket(WriteFn& writeFn,
                          uint32_t width,
                          uint32_t height,
                          FrameCodec codec,
                          const uint8_t* data,
                          uint32_t data_size,
                          uint32_t aux_size,
                          int64_t timestamp_us,
                          const VideoFrameMetrics& metrics) {
        FrameHeader hdr{};
        hdr.width = width;
        hdr.height = height;
        hdr.codec = static_cast<uint32_t>(codec);
        hdr.data_size = data_size;
        hdr.aux_size = aux_size;
        hdr.frame_id = metrics.frame_id;
        hdr.timestamp_us = timestamp_us;
        hdr.capture_ready_timestamp_us = metrics.capture_ready_timestamp_us;
        hdr.encode_ready_timestamp_us = metrics.encode_ready_timestamp_us;
        hdr.send_started_timestamp_us = metrics.send_started_timestamp_us;
        if (!writeFn(&hdr, sizeof(hdr))) {
            return false;
        }
        return data_size == 0 || writeFn(data, static_cast<int>(data_size));
    }

#ifdef REMOTEX_HAS_FFMPEG
    struct VideoEncoderState {
        const AVCodec* codec = nullptr;
        AVCodecContext* context = nullptr;
        SwsContext* scaler = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* packet = nullptr;
        int source_width = 0;
        int source_height = 0;
        int encoded_width = 0;
        int encoded_height = 0;
        int fps = 30;
        int64_t next_pts = 0;
    };

    struct VideoDecoderState {
        AVCodecContext* context = nullptr;
        SwsContext* scaler = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* yuv420_frame = nullptr;
        AVPacket* packet = nullptr;
        int output_width = 0;
        int output_height = 0;
    };

    static int RecommendVideoBitrate(int width, int height, int fps) {
        const int64_t estimated = static_cast<int64_t>(width) * height * fps / 10;
        return static_cast<int>(std::clamp<int64_t>(estimated, 2'000'000, 12'000'000));
    }

    static void ConfigureH264EncoderOptions(AVCodecContext* context,
                                            const AVCodec* codec,
                                            int fps) {
        if (!context || !codec || !codec->name) {
            return;
        }

        const std::string codec_name = codec->name;
        if (codec_name == "libx264") {
            av_opt_set(context->priv_data, "preset", "ultrafast", 0);
            av_opt_set(context->priv_data, "tune", "zerolatency", 0);
            const std::string params =
                "keyint=" + std::to_string(fps) +
                ":min-keyint=" + std::to_string(fps) +
                ":scenecut=0:repeat-headers=1";
            av_opt_set(context->priv_data, "x264-params", params.c_str(), 0);
            return;
        }

        if (codec_name == "h264_nvenc") {
            av_opt_set(context->priv_data, "preset", "p1", 0);
            av_opt_set(context->priv_data, "tune", "ll", 0);
            av_opt_set(context->priv_data, "zerolatency", "1", 0);
            av_opt_set(context->priv_data, "repeat_headers", "1", 0);
        }
    }

    static void ApplyQualityStep(AVCodecContext* context,
                                 const AVCodec* codec,
                                 int step) {
        if (!context || !codec || !codec->name) {
            return;
        }

        const std::string codec_name = codec->name;
        const remotex::core::EncodePreset* steps = nullptr;
        int step_count = 0;
        if (codec_name == "libx264") {
            steps = remotex::core::kLibx264Steps;
            step_count = remotex::core::kLibx264StepCount;
        } else if (codec_name == "h264_nvenc") {
            steps = remotex::core::kNvencSteps;
            step_count = remotex::core::kNvencStepCount;
        }
        if (!steps || step < 0 || step >= step_count) {
            return;
        }

        av_opt_set(context->priv_data, "preset", steps[step].preset, 0);

        const int fps = context->framerate.num > 0 ? context->framerate.num : 30;
        const int base_bitrate =
            RecommendVideoBitrate(context->width, context->height, fps);
        context->bit_rate = static_cast<int64_t>(base_bitrate) *
            steps[step].bitrate_factor_percent / 100;
        context->rc_max_rate = context->bit_rate;
        context->rc_buffer_size = static_cast<int>(context->bit_rate);
    }

    void ProbeSessionCodec() {
        if (codec_probed_) {
            return;
        }

        codec_probed_ = true;
        auto try_probe = [&](const char* codec_name,
                             remotex::core::SessionCodec codec_id,
                             int step_count) -> bool {
            const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
            if (!codec) {
                return false;
            }

            AVCodecContext* test_ctx = avcodec_alloc_context3(codec);
            if (!test_ctx) {
                return false;
            }

            test_ctx->width = 64;
            test_ctx->height = 64;
            test_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            test_ctx->time_base = AVRational{1, 30};
            test_ctx->framerate = AVRational{30, 1};
            test_ctx->bit_rate = 1000000;
            test_ctx->rc_max_rate = test_ctx->bit_rate;
            test_ctx->rc_buffer_size = static_cast<int>(test_ctx->bit_rate);
            test_ctx->max_b_frames = 0;
            test_ctx->gop_size = 30;
            test_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

            ConfigureH264EncoderOptions(test_ctx, codec, 30);
            ApplyQualityStep(test_ctx, codec, 0);

            const bool opened = avcodec_open2(test_ctx, codec, nullptr) == 0;
            avcodec_free_context(&test_ctx);
            if (!opened) {
                return false;
            }

            session_codec_ = codec_id;
            encode_quality_ =
                std::make_unique<remotex::core::EncodeQualityState>(step_count);
            resolution_adaptation_ = std::make_unique<remotex::core::ResolutionAdaptationState>();
            return true;
        };

        if (!try_probe("h264_nvenc",
                       remotex::core::SessionCodec::H264Nvenc,
                       remotex::core::kNvencStepCount)) {
            try_probe("libx264",
                      remotex::core::SessionCodec::H264Libx264,
                      remotex::core::kLibx264StepCount);
        }

        if (session_codec_ == remotex::core::SessionCodec::FallbackI420) {
            resolution_adaptation_.reset();
        }
    }

    void CleanupVideoEncoderInternal() {
        if (video_encoder_.packet) {
            av_packet_free(&video_encoder_.packet);
        }
        if (video_encoder_.frame) {
            av_frame_free(&video_encoder_.frame);
        }
        if (video_encoder_.context) {
            avcodec_free_context(&video_encoder_.context);
        }
        if (video_encoder_.scaler) {
            sws_freeContext(video_encoder_.scaler);
            video_encoder_.scaler = nullptr;
        }
        video_encoder_ = {};
    }

    void CleanupVideoDecoder() {
        if (video_decoder_.packet) {
            av_packet_free(&video_decoder_.packet);
        }
        if (video_decoder_.frame) {
            av_frame_free(&video_decoder_.frame);
        }
        if (video_decoder_.yuv420_frame) {
            av_frame_free(&video_decoder_.yuv420_frame);
        }
        if (video_decoder_.context) {
            avcodec_free_context(&video_decoder_.context);
        }
        if (video_decoder_.scaler) {
            sws_freeContext(video_decoder_.scaler);
            video_decoder_.scaler = nullptr;
        }
        video_decoder_ = {};
    }

    void CleanupVideoCodecState() {
        CleanupVideoEncoderInternal();
        CleanupVideoDecoder();
    }

    bool EnsureVideoEncoder(uint32_t source_width, uint32_t source_height) {
        const int fps = std::clamp(target_video_fps_, 1, 30);
        const double scale = resolution_adaptation_
            ? resolution_adaptation_->GetScaleFactor()
            : 1.0;
        int encoded_width =
            static_cast<int>(static_cast<double>(source_width) * scale) & ~1;
        int encoded_height =
            static_cast<int>(static_cast<double>(source_height) * scale) & ~1;
        encoded_width = std::max(encoded_width, ResolutionAdaptationState::kMinDimension);
        encoded_height = std::max(encoded_height, ResolutionAdaptationState::kMinDimension);
        if (encoded_width <= 0 || encoded_height <= 0) {
            return false;
        }

        if (video_encoder_.context &&
            video_encoder_.source_width == static_cast<int>(source_width) &&
            video_encoder_.source_height == static_cast<int>(source_height) &&
            video_encoder_.encoded_width == encoded_width &&
            video_encoder_.encoded_height == encoded_height &&
            video_encoder_.fps == fps) {
            return true;
        }

        CleanupVideoEncoderInternal();

        auto try_open_encoder = [&](const AVCodec* codec) -> bool {
            if (!codec) {
                return false;
            }

            AVCodecContext* context = avcodec_alloc_context3(codec);
            if (!context) {
                return false;
            }

            context->width = encoded_width;
            context->height = encoded_height;
            context->pix_fmt = AV_PIX_FMT_YUV420P;
            context->time_base = AVRational{1, fps};
            context->framerate = AVRational{fps, 1};
            context->gop_size = fps;
            context->max_b_frames = 0;
            context->bit_rate = RecommendVideoBitrate(encoded_width, encoded_height, fps);
            context->rc_max_rate = context->bit_rate;
            context->rc_buffer_size = context->bit_rate;
            context->flags |= AV_CODEC_FLAG_LOW_DELAY;

            ConfigureH264EncoderOptions(context, codec, fps);
            if (encode_quality_) {
                ApplyQualityStep(context, codec, encode_quality_->GetCurrentStep());
            }

            if (avcodec_open2(context, codec, nullptr) < 0) {
                avcodec_free_context(&context);
                return false;
            }

            AVFrame* frame = av_frame_alloc();
            AVPacket* packet = av_packet_alloc();
            if (!frame || !packet) {
                if (frame) {
                    av_frame_free(&frame);
                }
                if (packet) {
                    av_packet_free(&packet);
                }
                avcodec_free_context(&context);
                return false;
            }

            frame->format = context->pix_fmt;
            frame->width = context->width;
            frame->height = context->height;
            if (av_frame_get_buffer(frame, 32) < 0) {
                av_frame_free(&frame);
                av_packet_free(&packet);
                avcodec_free_context(&context);
                return false;
            }

            SwsContext* scaler = sws_getCachedContext(
                nullptr,
                static_cast<int>(source_width),
                static_cast<int>(source_height),
                AV_PIX_FMT_BGRA,
                encoded_width,
                encoded_height,
                AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (!scaler) {
                av_frame_free(&frame);
                av_packet_free(&packet);
                avcodec_free_context(&context);
                return false;
            }

            video_encoder_.codec = codec;
            video_encoder_.context = context;
            video_encoder_.scaler = scaler;
            video_encoder_.frame = frame;
            video_encoder_.packet = packet;
            video_encoder_.source_width = static_cast<int>(source_width);
            video_encoder_.source_height = static_cast<int>(source_height);
            video_encoder_.encoded_width = encoded_width;
            video_encoder_.encoded_height = encoded_height;
            video_encoder_.fps = fps;
            video_encoder_.next_pts = 0;
            return true;
        };

        switch (session_codec_) {
            case remotex::core::SessionCodec::H264Nvenc:
                return try_open_encoder(avcodec_find_encoder_by_name("h264_nvenc"));
            case remotex::core::SessionCodec::H264Libx264:
                return try_open_encoder(avcodec_find_encoder_by_name("libx264"));
            case remotex::core::SessionCodec::FallbackI420:
            default:
                return false;
        }
    }

    template<typename WriteFn>
    bool EncodeH264Frame(WriteFn& writeFn,
                         const uint8_t* bgra,
                         uint32_t width,
                         uint32_t height,
                         uint32_t pitch,
                         int64_t timestamp_us,
                         const VideoFrameMetrics& metrics) {
        if (!EnsureVideoEncoder(width, height)) {
            return false;
        }

        const uint8_t* src_slices[4] = { bgra, nullptr, nullptr, nullptr };
        const int src_stride[4] = { static_cast<int>(pitch), 0, 0, 0 };

        AVFrame* frame = video_encoder_.frame;
        if (av_frame_make_writable(frame) < 0) {
            CleanupVideoEncoderInternal();
            return false;
        }

        sws_scale(video_encoder_.scaler,
                  src_slices,
                  src_stride,
                  0,
                  video_encoder_.source_height,
                  frame->data,
                  frame->linesize);
        frame->pts = video_encoder_.next_pts++;

        const int send_result = avcodec_send_frame(video_encoder_.context, frame);
        if (send_result < 0) {
            CleanupVideoEncoderInternal();
            return false;
        }

        bool sent_packet = false;
        while (running_) {
            AVPacket* packet = video_encoder_.packet;
            av_packet_unref(packet);
            const int receive_result = avcodec_receive_packet(video_encoder_.context, packet);
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                break;
            }
            if (receive_result < 0) {
                CleanupVideoEncoderInternal();
                return false;
            }

            if (!WriteFramePacket(writeFn,
                                  static_cast<uint32_t>(video_encoder_.encoded_width),
                                  static_cast<uint32_t>(video_encoder_.encoded_height),
                                  kFrameCodecH264,
                                  packet->data,
                                  static_cast<uint32_t>(packet->size),
                                  0,
                                  timestamp_us,
                                  metrics)) {
                return false;
            }
            sent_packet = true;
        }

        return sent_packet;
    }

    bool EnsureVideoDecoder() {
        if (video_decoder_.context) {
            return true;
        }

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            return false;
        }

        AVCodecContext* context = avcodec_alloc_context3(codec);
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        if (!context || !frame || !packet) {
            if (context) {
                avcodec_free_context(&context);
            }
            if (frame) {
                av_frame_free(&frame);
            }
            if (packet) {
                av_packet_free(&packet);
            }
            return false;
        }

        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
            av_frame_free(&frame);
            av_packet_free(&packet);
            return false;
        }

        video_decoder_.context = context;
        video_decoder_.frame = frame;
        video_decoder_.packet = packet;
        return true;
    }

    bool EnsureDecoderOutputFrame(int width, int height, AVPixelFormat input_format) {
        if (video_decoder_.yuv420_frame &&
            video_decoder_.output_width == width &&
            video_decoder_.output_height == height &&
            video_decoder_.scaler) {
            return true;
        }

        if (video_decoder_.yuv420_frame) {
            av_frame_free(&video_decoder_.yuv420_frame);
        }
        if (video_decoder_.scaler) {
            sws_freeContext(video_decoder_.scaler);
            video_decoder_.scaler = nullptr;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return false;
        }

        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return false;
        }

        SwsContext* scaler = sws_getCachedContext(
            nullptr,
            width,
            height,
            input_format,
            width,
            height,
            AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!scaler) {
            av_frame_free(&frame);
            return false;
        }

        video_decoder_.yuv420_frame = frame;
        video_decoder_.scaler = scaler;
        video_decoder_.output_width = width;
        video_decoder_.output_height = height;
        return true;
    }

    bool EmitDecodedFrame(const AVFrame* frame, const FrameHeader& hdr) {
        if (!listener_ || !frame) {
            return false;
        }

        VideoFrame vf{};
        vf.data_y = frame->data[0];
        vf.data_u = frame->data[1];
        vf.data_v = frame->data[2];
        vf.stride_y = frame->linesize[0];
        vf.stride_u = frame->linesize[1];
        vf.stride_v = frame->linesize[2];
        vf.width = frame->width;
        vf.height = frame->height;
        vf.frame_id = hdr.frame_id;
        vf.timestamp_us = hdr.timestamp_us;
        vf.capture_ready_timestamp_us = hdr.capture_ready_timestamp_us;
        vf.encode_ready_timestamp_us = hdr.encode_ready_timestamp_us;
        vf.send_started_timestamp_us = hdr.send_started_timestamp_us;
        listener_->OnVideoFrame(vf);
        return true;
    }

    bool DecodeH264Frame(const FrameHeader& hdr, const std::vector<uint8_t>& payload) {
        if (!EnsureVideoDecoder()) {
            return false;
        }

        AVPacket* packet = video_decoder_.packet;
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(payload.size())) < 0) {
            return false;
        }
        std::memcpy(packet->data, payload.data(), payload.size());
        packet->pts = hdr.timestamp_us;
        packet->dts = hdr.timestamp_us;

        const int send_result = avcodec_send_packet(video_decoder_.context, packet);
        if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
            CleanupVideoDecoder();
            return false;
        }

        bool emitted = false;
        while (running_) {
            AVFrame* frame = video_decoder_.frame;
            const int receive_result = avcodec_receive_frame(video_decoder_.context, frame);
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                break;
            }
            if (receive_result < 0) {
                CleanupVideoDecoder();
                return false;
            }

            if (frame->format == AV_PIX_FMT_YUV420P) {
                emitted = EmitDecodedFrame(frame, hdr) || emitted;
            } else if (EnsureDecoderOutputFrame(frame->width,
                                                frame->height,
                                                static_cast<AVPixelFormat>(frame->format))) {
                AVFrame* output = video_decoder_.yuv420_frame;
                if (av_frame_make_writable(output) == 0) {
                    sws_scale(video_decoder_.scaler,
                              frame->data,
                              frame->linesize,
                              0,
                              frame->height,
                              output->data,
                              output->linesize);
                    output->width = frame->width;
                    output->height = frame->height;
                    emitted = EmitDecodedFrame(output, hdr) || emitted;
                }
            }

            av_frame_unref(frame);
        }

        return emitted;
    }
#else
    void CleanupVideoEncoderInternal() {}
    void CleanupVideoDecoder() {}
    void CleanupVideoCodecState() {}
    void ProbeSessionCodec() {}

    template<typename WriteFn>
    bool EncodeH264Frame(WriteFn&,
                         const uint8_t*,
                         uint32_t,
                         uint32_t,
                         uint32_t,
                         int64_t,
                         const VideoFrameMetrics&) {
        return false;
    }

    bool DecodeH264Frame(const FrameHeader&, const std::vector<uint8_t>&) {
        return false;
    }
#endif
    // ── Protocolo de frame ───────────────────────────────────────────────

    // ── Conectar ao relay TCP ────────────────────────────────────────────

    SOCKET ConnectToRelay() {
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string port_str = std::to_string(config_.relay_port);
        if (getaddrinfo(config_.relay_host.c_str(), port_str.c_str(), &hints, &result) != 0)
            return INVALID_SOCKET;

        SOCKET s = INVALID_SOCKET;
        for (auto* ptr = result; ptr; ptr = ptr->ai_next) {
            s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            EnableLowLatencySocket(s);
            if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
            break;
        }
        freeaddrinfo(result);
        return s;
    }

    bool ConnectRelaySocketWithRetries(SOCKET& socket_handle) {
        socket_handle = INVALID_SOCKET;
        for (int i = 0; i < 3 && running_; ++i) {
            socket_handle = ConnectToRelay();
            if (socket_handle != INVALID_SOCKET) {
                return true;
            }
            Sleep(500);
        }
        return false;
    }

    void EnableLowLatencySocket(SOCKET socket_handle) {
        const int no_delay = 1;
        const int keep_alive = 1;
        setsockopt(socket_handle,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   reinterpret_cast<const char*>(&no_delay),
                   sizeof(no_delay));
        setsockopt(socket_handle,
                   SOL_SOCKET,
                   SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&keep_alive),
                   sizeof(keep_alive));
    }

    bool HandshakeRelaySocket(SOCKET socket_handle, const char* role, const char* lane) {
        if (config_.session_id.empty() || config_.relay_ticket.empty()) {
            return false;
        }
        std::string handshake = "HELLO " +
            std::to_string(config_.relay_protocol_version > 0 ? config_.relay_protocol_version : 1) +
            " " + role +
            " " + lane +
            " " + config_.session_id +
            " " + std::to_string(config_.relay_ticket_expires_unix) +
            " " + std::to_string(config_.relay_capabilities) +
            " " + config_.relay_ticket + "\n";
        if (!SendAll(socket_handle, handshake.c_str(), static_cast<int>(handshake.size()))) {
            return false;
        }
        char ok_buf[6]{};
        return RecvAll(socket_handle, ok_buf, 6) && std::strncmp(ok_buf, "READY\n", 6) == 0;
    }

    bool SendAll(SOCKET s, const void* data, int len) {
        const char* ptr = static_cast<const char*>(data);
        int sent = 0;
        while (sent < len) {
            int n = send(s, ptr + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    bool RecvAll(SOCKET s, void* data, int len) {
        char* ptr = static_cast<char*>(data);
        int received = 0;
        while (received < len && running_) {
            int n = recv(s, ptr + received, len - received, 0);
            if (n <= 0) return false;
            received += n;
        }
        return received == len;
    }

    // ── Named Pipe fallback (localhost) ─────────────────────────────────

    static constexpr const char* kPipeName = "\\\\.\\pipe\\remotex_video";
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool using_pipe_ = false;

    void StartHostPipeFallback() {
        pipe_ = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1, 16 * 1024 * 1024, 0, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            HandleTransportEnded(PeerConnectionState::Failed);
            return;
        }

        using_pipe_ = true;
        net_thread_ = std::thread([this]() {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ConnectNamedPipe(pipe_, &ov);
            while (running_) {
                DWORD wait = WaitForSingleObject(ov.hEvent, 200);
                if (wait == WAIT_OBJECT_0) break;
            }
            CloseHandle(ov.hEvent);
            if (!running_) return;

            MarkTransportConnected();

            SendFrameLoop([this](const void* d, int n) {
                DWORD w; return WriteFile(pipe_, d, n, &w, nullptr) != FALSE;
            });

            if (!shutdown_requested_.load()) {
                HandleTransportEnded(PeerConnectionState::Disconnected);
            }
        });
    }

    void StartViewerPipeFallback() {
        using_pipe_ = true;
        net_thread_ = std::thread([this]() {
            for (int i = 0; i < 100 && running_; ++i) {
                pipe_ = CreateFileA(kPipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe_ != INVALID_HANDLE_VALUE) break;
                Sleep(100);
            }
            if (pipe_ == INVALID_HANDLE_VALUE) {
                HandleTransportEnded(PeerConnectionState::Failed);
                return;
            }

            MarkTransportConnected();

            RecvFrameLoop([this](void* d, int n) -> bool {
                DWORD total = 0;
                auto* p = static_cast<uint8_t*>(d);
                while ((int)total < n && running_) {
                    DWORD r; if (!ReadFile(pipe_, p + total, n - total, &r, nullptr)) return false;
                    total += r;
                }
                return (int)total == n;
            });

            if (!shutdown_requested_.load()) {
                HandleTransportEnded(PeerConnectionState::Disconnected);
            }
        });
    }

    // ── Lógica genérica de envio/recepção de frames ─────────────────────

    // Envio de frames: tenta H.264 via FFmpeg; faz fallback para I420 quando necessário.
    template<typename WriteFn>
    void SendFrameLoop(WriteFn writeFn) {
        ProbeSessionCodec();
        while (running_) {
            auto slot_data = frame_slot_.TryDequeue();
            if (!slot_data) {
                Sleep(2);
                continue;
            }

            auto metrics = slot_data->metrics;
            const auto encode_start = std::chrono::steady_clock::now();
            metrics.encode_ready_timestamp_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    encode_start.time_since_epoch()).count();

            bool encoded = false;

#ifdef REMOTEX_HAS_FFMPEG
            if (session_codec_ != remotex::core::SessionCodec::FallbackI420) {
                encoded = EncodeH264Frame(writeFn,
                                          slot_data->bgra_data.data(),
                                          slot_data->width,
                                          slot_data->height,
                                          slot_data->pitch,
                                          metrics.encode_ready_timestamp_us,
                                          metrics);
            }
#endif

            // Converter BGRA → I420 antes de enviar (muito menor).
            const auto encode_end = std::chrono::steady_clock::now();
            const double encode_ms =
                std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
            if (encode_quality_) {
                encode_quality_->UpdateEncodeTime(encode_ms);
                const auto now = std::chrono::steady_clock::now();
                const bool resolution_settling =
                    resolution_step_changed_at_ != std::chrono::steady_clock::time_point{} &&
                    (now - resolution_step_changed_at_) < std::chrono::seconds(5);
                if (!resolution_settling) {
                    if (encode_quality_->ShouldStepDown()) {
                        encode_quality_->StepDown(now);
                        CleanupVideoEncoder();
                    } else if (encode_quality_->ShouldStepUp(now)) {
                        encode_quality_->StepUp(now);
                        CleanupVideoEncoder();
                    }
                }
            }

            if (encoded) {
                continue;
            }

            BGRAtoI420(slot_data->bgra_data.data(),
                       slot_data->width,
                       slot_data->height,
                       slot_data->pitch,
                       i420_buffer_);

            // Fase 1: ZLIB Compression no I420
            uLongf compressed_len = compressBound(static_cast<uLong>(i420_buffer_.size()));
            compressed_frame_buffer_.resize(compressed_len);
            
            if (compress2(compressed_frame_buffer_.data(),
                          &compressed_len,
                          i420_buffer_.data(),
                          static_cast<uLong>(i420_buffer_.size()),
                          Z_BEST_SPEED) == Z_OK) {
                if (!WriteFramePacket(writeFn,
                                      slot_data->width,
                                      slot_data->height,
                                      kFrameCodecI420Zlib,
                                      compressed_frame_buffer_.data(),
                                      static_cast<uint32_t>(compressed_len),
                                      static_cast<uint32_t>(i420_buffer_.size()),
                                      metrics.encode_ready_timestamp_us,
                                      metrics)) {
                    break;
                }
            } else {
                // Fallback (deveria ser quase impossível zlib falhar havendo memória)
                if (!WriteFramePacket(writeFn,
                                      slot_data->width,
                                      slot_data->height,
                                      kFrameCodecI420Raw,
                                      i420_buffer_.data(),
                                      static_cast<uint32_t>(i420_buffer_.size()),
                                      static_cast<uint32_t>(i420_buffer_.size()),
                                      metrics.encode_ready_timestamp_us,
                                      metrics)) {
                    break;
                }
            }
        }
    }

    template<typename ReadFn>
    void RecvFrameLoop(ReadFn readFn) {
        while (running_) {
            FrameHeader hdr{};
            if (!readFn(&hdr, sizeof(hdr))) break;
            if (hdr.data_size == 0 || hdr.width == 0 || hdr.height == 0) continue;
            if (hdr.data_size > 32 * 1024 * 1024) break;

            std::vector<uint8_t> payload(hdr.data_size);
            if (!readFn(payload.data(), hdr.data_size)) break;

            DispatchFrame(hdr, payload);
        }
    }

    // ── Receber data channel messages do peer (roda em dc_thread_) ──────

    void RecvDataChannelLoop(SOCKET socket_handle, bool is_bulk_lane) {
        while (running_) {
            uint8_t packet_type = 0;
            if (!RecvAll(socket_handle, &packet_type, 1)) break;

            if (packet_type != kPacketTypeDataChannel) {
                break;
            }

            // Wire format: [1B packet_type][1B label_len][label][1B flags][4B data_len LE][data]
            uint8_t label_len = 0;
            if (!RecvAll(socket_handle, &label_len, 1)) break;

            std::string label(label_len, '\0');
            if (label_len > 0 && !RecvAll(socket_handle, label.data(), label_len)) break;

            uint8_t flags = 0;
            if (!RecvAll(socket_handle, &flags, 1)) break;

            uint32_t data_len = 0;
            if (!RecvAll(socket_handle, &data_len, 4)) break;
            if (data_len > 4 * 1024 * 1024) break;

            std::vector<uint8_t> data(data_len);
            if (data_len > 0 && !RecvAll(socket_handle, data.data(), data_len)) break;

            if (listener_) {
                DataChannelMessage msg;
                msg.channel_label = label;
                msg.data = std::move(data);
                msg.is_binary = (flags & kDataChannelFlagBinary) != 0;
                listener_->OnDataChannelMessage(msg);
            }
        }

        if (!shutdown_requested_.load()) {
            if (is_bulk_lane) {
                HandleBulkTransportEnded();
            } else {
                HandleTransportEnded(PeerConnectionState::Disconnected);
            }
        }
    }

    void DispatchFrame(const FrameHeader& hdr, const std::vector<uint8_t>& payload) {
        if (!listener_) {
            return;
        }

        int y_stride = hdr.width;
        int uv_stride = hdr.width / 2;
        int y_size = y_stride * hdr.height;
        int uv_size = uv_stride * (hdr.height / 2);

        if (hdr.codec == kFrameCodecH264) {
            DecodeH264Frame(hdr, payload);
            return;
        }

        if (hdr.codec == kFrameCodecI420Zlib) {
            std::vector<uint8_t> uncompressed(hdr.aux_size);
            uLongf dest_len = hdr.aux_size;

            if (uncompress(uncompressed.data(), &dest_len, payload.data(), hdr.data_size) == Z_OK) {
                VideoFrame vf{};
                vf.data_y = uncompressed.data();
                vf.data_u = uncompressed.data() + y_size;
                vf.data_v = uncompressed.data() + y_size + uv_size;
                vf.stride_y = y_stride;
                vf.stride_u = uv_stride;
                vf.stride_v = uv_stride;
                vf.width  = static_cast<int>(hdr.width);
                vf.height = static_cast<int>(hdr.height);
                vf.frame_id = hdr.frame_id;
                vf.timestamp_us = hdr.timestamp_us;
                vf.capture_ready_timestamp_us = hdr.capture_ready_timestamp_us;
                vf.encode_ready_timestamp_us = hdr.encode_ready_timestamp_us;
                vf.send_started_timestamp_us = hdr.send_started_timestamp_us;
                listener_->OnVideoFrame(vf);
            }
            return;
        }

        if (hdr.codec == kFrameCodecI420Raw) {
            VideoFrame vf{};
            vf.data_y = payload.data();
            vf.data_u = payload.data() + y_size;
            vf.data_v = payload.data() + y_size + uv_size;
            vf.stride_y = y_stride;
            vf.stride_u = uv_stride;
            vf.stride_v = uv_stride;
            vf.width  = static_cast<int>(hdr.width);
            vf.height = static_cast<int>(hdr.height);
            vf.frame_id = hdr.frame_id;
            vf.timestamp_us = hdr.timestamp_us;
            vf.capture_ready_timestamp_us = hdr.capture_ready_timestamp_us;
            vf.encode_ready_timestamp_us = hdr.encode_ready_timestamp_us;
            vf.send_started_timestamp_us = hdr.send_started_timestamp_us;
            listener_->OnVideoFrame(vf);
            return;
        }

        if (DecodeH264Frame(hdr, payload)) {
            return;
        }

        auto i420 = BGRAtoI420(payload.data(), hdr.width, hdr.height, hdr.aux_size);
        VideoFrame vf{};
        vf.data_y = i420.data();
        vf.data_u = i420.data() + y_size;
        vf.data_v = i420.data() + y_size + uv_size;
        vf.stride_y = y_stride;
        vf.stride_u = uv_stride;
        vf.stride_v = uv_stride;
        vf.width  = static_cast<int>(hdr.width);
        vf.height = static_cast<int>(hdr.height);
        vf.frame_id = hdr.frame_id;
        vf.timestamp_us = hdr.timestamp_us;
        vf.capture_ready_timestamp_us = hdr.capture_ready_timestamp_us;
        vf.encode_ready_timestamp_us = hdr.encode_ready_timestamp_us;
        vf.send_started_timestamp_us = hdr.send_started_timestamp_us;
        listener_->OnVideoFrame(vf);
    }

    // ── Host: conecta ao relay TCP (fallback: named pipe) ───────────────
    // Host usa 2 threads: net_thread_ envia video, dc_thread_ recebe input.

    void StartHostRelay() {
        running_ = true;
        using_pipe_ = false;
        bulk_available_.store(false);

        if (!ConnectRelaySocketWithRetries(video_sock_) ||
            !ConnectRelaySocketWithRetries(data_sock_)) {
            CloseSocket(video_sock_);
            CloseSocket(data_sock_);
            StartHostPipeFallback();
            return;
        }
        ConnectRelaySocketWithRetries(bulk_sock_);

        net_thread_ = std::thread([this]() {
            if (!HandshakeRelaySocket(video_sock_, "host", kLaneVideo) ||
                !HandshakeRelaySocket(data_sock_, "host", kLaneRealtime)) {
                HandleTransportEnded(PeerConnectionState::Failed);
                return;
            }
            if (bulk_sock_ != INVALID_SOCKET &&
                HandshakeRelaySocket(bulk_sock_, "host", kLaneBulk)) {
                bulk_available_.store(true);
                bulk_thread_ = std::thread([this]() { RecvDataChannelLoop(bulk_sock_, true); });
            } else {
                CloseSocket(bulk_sock_);
            }

            MarkTransportConnected();
            dc_thread_ = std::thread([this]() { RecvDataChannelLoop(data_sock_, false); });

            SendFrameLoop([this](const void* d, int n) {
                return SendAll(video_sock_, d, n);
            });

            if (!shutdown_requested_.load()) {
                HandleTransportEnded(PeerConnectionState::Disconnected);
            }
        });
    }

    // ── Viewer: conecta ao relay TCP (fallback: named pipe) ─────────────
    // Viewer usa 2 threads: net_thread_ recebe video, dc_thread_ pode enviar input.
    // (Input é enviado de forma síncrona via SendDataChannelMessage)

    void StartViewerRelay() {
        running_ = true;
        using_pipe_ = false;
        bulk_available_.store(false);

        if (!ConnectRelaySocketWithRetries(video_sock_) ||
            !ConnectRelaySocketWithRetries(data_sock_)) {
            CloseSocket(video_sock_);
            CloseSocket(data_sock_);
            StartViewerPipeFallback();
            return;
        }
        ConnectRelaySocketWithRetries(bulk_sock_);

        net_thread_ = std::thread([this]() {
            if (!HandshakeRelaySocket(video_sock_, "viewer", kLaneVideo) ||
                !HandshakeRelaySocket(data_sock_, "viewer", kLaneRealtime)) {
                HandleTransportEnded(PeerConnectionState::Failed);
                return;
            }
            if (bulk_sock_ != INVALID_SOCKET &&
                HandshakeRelaySocket(bulk_sock_, "viewer", kLaneBulk)) {
                bulk_available_.store(true);
                bulk_thread_ = std::thread([this]() { RecvDataChannelLoop(bulk_sock_, true); });
            } else {
                CloseSocket(bulk_sock_);
            }

            MarkTransportConnected();
            dc_thread_ = std::thread([this]() { RecvDataChannelLoop(data_sock_, false); });

            RecvFrameLoop([this](void* d, int n) {
                return RecvAll(video_sock_, d, n);
            });

            if (!shutdown_requested_.load()) {
                HandleTransportEnded(PeerConnectionState::Disconnected);
            }
        });
    }

    // ── Conversão BGRA → I420 (BT.601) ──────────────────────────────────

    static void BGRAtoI420(
        const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch, std::vector<uint8_t>& yuv)
    {
        uint32_t y_size  = w * h;
        uint32_t uv_size = (w / 2) * (h / 2);
        yuv.resize(y_size + 2 * uv_size);

        uint8_t* Y = yuv.data();
        uint8_t* U = Y + y_size;
        uint8_t* V = U + uv_size;

        for (uint32_t row = 0; row < h; ++row) {
            const uint8_t* line = bgra + row * pitch;
            for (uint32_t col = 0; col < w; ++col) {
                const uint8_t* px = line + col * 4;
                int B = px[0], G = px[1], R = px[2];

                Y[row * w + col] = static_cast<uint8_t>(
                    std::clamp((66 * R + 129 * G + 25 * B + 128) / 256 + 16, 0, 255));

                if ((row & 1) == 0 && (col & 1) == 0) {
                    uint32_t ui = (row / 2) * (w / 2) + (col / 2);
                    U[ui] = static_cast<uint8_t>(
                        std::clamp((-38 * R - 74 * G + 112 * B + 128) / 256 + 128, 0, 255));
                    V[ui] = static_cast<uint8_t>(
                        std::clamp((112 * R - 94 * G - 18 * B + 128) / 256 + 128, 0, 255));
                }
            }
        }
    }

    static std::vector<uint8_t> BGRAtoI420(
        const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch)
    {
        std::vector<uint8_t> yuv;
        BGRAtoI420(bgra, w, h, pitch, yuv);
        return yuv;
    }

    // ── Signaling handshake + relay start ────────────────────────────────

    void TransitionToConnected() {
        if (state_ == PeerConnectionState::Connected ||
            state_ == PeerConnectionState::Connecting) {
            return;
        }

        shutdown_requested_.store(false);
        NotifyStateChange(PeerConnectionState::Connecting);

        // Iniciar transporte de vídeo via TCP relay.
        if (config_.is_offerer) {
            StartViewerRelay();
        } else {
            StartHostRelay();
        }

    }

    static std::string GenerateStubSDP(const char* type) {
        return std::string("v=0\r\n"
            "o=remotex-stub 0 0 IN IP4 127.0.0.1\r\n"
            "s=RemoteX Stub Session\r\n"
            "t=0 0\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "a=setup:") + (std::string(type) == "offer" ? "actpass" : "active") +
            "\r\na=mid:0\r\n";
    }

    // ── Members ──────────────────────────────────────────────────────────

    WebRTCPeerListener* listener_ = nullptr;
    PeerConfig config_;
    SessionDescription remote_desc_;
    PeerConnectionState state_ = PeerConnectionState::New;

    // Video dimensions.
    uint32_t video_width_  = 0;
    uint32_t video_height_ = 0;
    int target_video_fps_ = 30;

    // TCP socket.
    SOCKET video_sock_ = INVALID_SOCKET;
    SOCKET data_sock_ = INVALID_SOCKET;
    SOCKET bulk_sock_ = INVALID_SOCKET;
    std::thread net_thread_;
    std::thread dc_thread_;         // data channel recv (Host side)
    std::thread bulk_thread_;
    std::mutex  data_send_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> bulk_available_{false};
    std::atomic<int64_t> last_realtime_send_tick_ms_{0};

    // Frame buffer (host → relay).
    remotex::core::BoundedFrameSlot frame_slot_;
    std::vector<uint8_t> i420_buffer_;
    std::vector<uint8_t> compressed_frame_buffer_;
    remotex::core::SessionCodec session_codec_ =
        remotex::core::SessionCodec::FallbackI420;
    std::unique_ptr<remotex::core::EncodeQualityState> encode_quality_;
    std::unique_ptr<remotex::core::ResolutionAdaptationState> resolution_adaptation_;
    std::chrono::steady_clock::time_point resolution_step_changed_at_{};
    bool codec_probed_ = false;
#ifdef REMOTEX_HAS_FFMPEG
    VideoEncoderState video_encoder_;
    VideoDecoderState video_decoder_;
#endif
};

} // namespace remotex::core
