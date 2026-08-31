// RemoteX — core/webrtc_peer.cpp
// Wrapper da libwebrtc PeerConnection.
//
// A implementação real depende da libwebrtc (REMOTEX_HAS_LIBWEBRTC).
// Sem ela, todas as operações retornam erro graciosamente.

#include "core/webrtc_peer.h"

#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Implementação com libwebrtc
// ─────────────────────────────────────────────────────────────────────────────
#ifdef REMOTEX_HAS_LIBWEBRTC

#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/media_stream_interface.h>
#include <api/data_channel_interface.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/jsep.h>
#include <rtc_base/thread.h>
#include <rtc_base/logging.h>

#include <map>

namespace remotex::core {

// ─── Custom VideoTrackSource ────────────────────────────────────────────────

// Adaptador que permite injetar frames de vídeo na PeerConnection.
class FrameInjectorSource : public webrtc::VideoTrackSourceInterface {
public:
    FrameInjectorSource(int width, int height, int fps)
        : width_(width), height_(height), fps_(fps) {}

    // Chamado pelo Host para injetar um frame.
    void InjectFrame(const uint8_t* data,
                     size_t size,
                     int64_t timestamp_us,
                     const VideoFrameMetrics& metrics) {
        // Criar um VideoFrame a partir dos dados encodados.
        // Na prática, com NVENC, injetamos um encoded frame via
        // EncodedImageCallback, não aqui. Este path é para frames raw I420.
        auto buffer = webrtc::I420Buffer::Create(width_, height_);
        // Copiar dados Y/U/V (assumindo layout I420 packed).
        if (size >= static_cast<size_t>(width_ * height_ * 3 / 2)) {
            const uint8_t* y = data;
            const uint8_t* u = data + width_ * height_;
            const uint8_t* v = u + (width_ * height_ / 4);
            libyuv::I420Copy(
                y, width_, u, width_ / 2, v, width_ / 2,
                buffer->MutableDataY(), buffer->StrideY(),
                buffer->MutableDataU(), buffer->StrideU(),
                buffer->MutableDataV(), buffer->StrideV(),
                width_, height_);
        }

        auto frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_us(timestamp_us)
            .build();

        {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            last_injected_metrics_ = metrics;
            last_injected_metrics_.encode_ready_timestamp_us =
                metrics.encode_ready_timestamp_us != 0
                    ? metrics.encode_ready_timestamp_us
                    : timestamp_us;
            last_injected_metrics_.send_started_timestamp_us =
                metrics.send_started_timestamp_us != 0
                    ? metrics.send_started_timestamp_us
                    : timestamp_us;
        }
        broadcaster_.OnFrame(frame);
    }

    // VideoTrackSourceInterface
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return true; }
    absl::optional<bool> needs_denoising() const override { return false; }

    void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                         const rtc::VideoSinkWants& wants) override {
        broadcaster_.AddOrUpdateSink(sink, wants);
    }

    void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
        broadcaster_.RemoveSink(sink);
    }

    void UpdateDimensions(int w, int h) { width_ = w; height_ = h; }

    void RegisterObserver(webrtc::ObserverInterface* observer) override {}
    void UnregisterObserver(webrtc::ObserverInterface* observer) override {}

private:
    int width_;
    int height_;
    int fps_;
    rtc::VideoBroadcaster broadcaster_;
    std::mutex metrics_mutex_;
    VideoFrameMetrics last_injected_metrics_{};
};

// ─── Observer de criação de SDP ─────────────────────────────────────────────

class CreateSDPObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    using Callback = std::function<void(webrtc::SessionDescriptionInterface*)>;
    using ErrorCallback = std::function<void(webrtc::RTCError)>;

    CreateSDPObserver(Callback on_success, ErrorCallback on_failure)
        : on_success_(std::move(on_success)), on_failure_(std::move(on_failure)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (on_success_) on_success_(desc);
    }

    void OnFailure(webrtc::RTCError error) override {
        if (on_failure_) on_failure_(std::move(error));
    }

private:
    Callback on_success_;
    ErrorCallback on_failure_;
};

// ─── Observer de SetLocalDescription/SetRemoteDescription ───────────────────

class SetSDPObserver : public webrtc::SetSessionDescriptionObserver {
public:
    using Callback = std::function<void()>;
    using ErrorCallback = std::function<void(webrtc::RTCError)>;

    SetSDPObserver(Callback on_success, ErrorCallback on_failure)
        : on_success_(std::move(on_success)), on_failure_(std::move(on_failure)) {}

    void OnSuccess() override { if (on_success_) on_success_(); }
    void OnFailure(webrtc::RTCError error) override { if (on_failure_) on_failure_(std::move(error)); }

private:
    Callback on_success_;
    ErrorCallback on_failure_;
};

// ─── WebRTCPeerImpl (libwebrtc) ─────────────────────────────────────────────

class WebRTCPeerImpl : public webrtc::PeerConnectionObserver {
public:
    WebRTCPeerImpl() = default;
    ~WebRTCPeerImpl() override { Shutdown(); }

    void SetListener(WebRTCPeerListener* listener) { listener_ = listener; }

    bool Initialize(const PeerConfig& config) {
        config_ = config;

        // Criar threads de rede e worker.
        network_thread_ = rtc::Thread::CreateWithSocketServer();
        worker_thread_  = rtc::Thread::Create();
        signaling_thread_ = rtc::Thread::Create();

        network_thread_->Start();
        worker_thread_->Start();
        signaling_thread_->Start();

        // Criar a PeerConnectionFactory.
        factory_ = webrtc::CreatePeerConnectionFactory(
            network_thread_.get(),
            worker_thread_.get(),
            signaling_thread_.get(),
            nullptr, // default ADM
            webrtc::CreateBuiltinAudioEncoderFactory(),
            webrtc::CreateBuiltinAudioDecoderFactory(),
            webrtc::CreateBuiltinVideoEncoderFactory(),
            webrtc::CreateBuiltinVideoDecoderFactory(),
            nullptr, nullptr);

        if (!factory_)
            return false;

        // Configurar ICE servers.
        webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
        rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

        for (const auto& ice : config.ice_servers) {
            webrtc::PeerConnectionInterface::IceServer server;
            for (const auto& url : ice.urls)
                server.urls.push_back(url);
            server.username   = ice.username;
            server.password   = ice.credential;
            rtc_config.servers.push_back(server);
        }

        // Criar a PeerConnection.
        auto result = factory_->CreatePeerConnectionOrError(
            rtc_config,
            webrtc::PeerConnectionDependencies(this));

        if (!result.ok())
            return false;

        peer_connection_ = result.MoveValue();
        connection_state_ = PeerConnectionState::New;
        return true;
    }

    void Shutdown() {
        for (auto& [label, dc] : data_channels_) {
            dc->UnregisterObserver();
            dc->Close();
        }
        data_channel_observers_.clear();
        data_channels_.clear();

        if (peer_connection_) {
            peer_connection_->Close();
            peer_connection_ = nullptr;
        }

        factory_         = nullptr;
        video_source_    = nullptr;

        if (signaling_thread_) signaling_thread_->Stop();
        if (worker_thread_)    worker_thread_->Stop();
        if (network_thread_)   network_thread_->Stop();

        connection_state_ = PeerConnectionState::Closed;
    }

    // ── SDP ─────────────────────────────────────────────────────────────

    void CreateOffer() {
        if (!peer_connection_) return;

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_video = 1;
        options.offer_to_receive_audio = 0;

        auto observer = rtc::make_ref_counted<CreateSDPObserver>(
            [this](webrtc::SessionDescriptionInterface* desc) { OnSDPCreated(desc); },
            [this](webrtc::RTCError err) {
                // Falha ao criar offer — notificar via estado.
                connection_state_ = PeerConnectionState::Failed;
                if (listener_) listener_->OnConnectionStateChanged(connection_state_);
            });

        peer_connection_->CreateOffer(observer.get(), options);
    }

    void CreateAnswer() {
        if (!peer_connection_) return;

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

        auto observer = rtc::make_ref_counted<CreateSDPObserver>(
            [this](webrtc::SessionDescriptionInterface* desc) { OnSDPCreated(desc); },
            [this](webrtc::RTCError err) {
                connection_state_ = PeerConnectionState::Failed;
                if (listener_) listener_->OnConnectionStateChanged(connection_state_);
            });

        peer_connection_->CreateAnswer(observer.get(), options);
    }

    bool SetRemoteDescription(const SessionDescription& desc) {
        if (!peer_connection_) return false;

        auto type = (desc.type == "offer")
            ? webrtc::SdpType::kOffer
            : webrtc::SdpType::kAnswer;

        webrtc::SdpParseError error;
        auto sdp = webrtc::CreateSessionDescription(type, desc.sdp, &error);
        if (!sdp)
            return false;

        auto observer = rtc::make_ref_counted<SetSDPObserver>(
            [this, &desc]() {
                // Se recebemos um offer e somos answerer, criar answer automaticamente.
                if (desc.type == "offer" && !config_.is_offerer)
                    CreateAnswer();
            },
            [](webrtc::RTCError) {});

        peer_connection_->SetRemoteDescription(observer.get(), sdp.release());
        return true;
    }

    bool AddIceCandidate(const IceCandidate& candidate) {
        if (!peer_connection_) return false;

        webrtc::SdpParseError error;
        auto ice = webrtc::CreateIceCandidate(
            candidate.sdp_mid, candidate.sdp_mline_index,
            candidate.candidate, &error);
        if (!ice)
            return false;

        return peer_connection_->AddIceCandidate(ice);
    }

    // ── Mídia ───────────────────────────────────────────────────────────

    bool AddVideoTrack(int width, int height, int fps) {
        if (!factory_ || !peer_connection_) return false;

        video_source_ = rtc::make_ref_counted<FrameInjectorSource>(width, height, fps);
        auto track = factory_->CreateVideoTrack("screen_video", video_source_.get());
        if (!track) return false;

        auto result = peer_connection_->AddTrack(track, {"screen_stream"});
        return result.ok();
    }

    bool SendVideoFrame(const uint8_t* data,
                        size_t size,
                        int64_t timestamp_us,
                        const VideoFrameMetrics& metrics) {
        if (!video_source_) return false;
        video_source_->InjectFrame(data, size, timestamp_us, metrics);
        return true;
    }

    void UpdateVideoDimensions(int w, int h) {
        if (video_source_) {
            video_source_->UpdateDimensions(w, h);
        }
    }

    // ── Data Channels ───────────────────────────────────────────────────

    bool CreateDataChannel(const std::string& label, bool ordered, bool reliable) {
        if (!peer_connection_) return false;

        webrtc::DataChannelInit init;
        init.ordered  = ordered;
        init.reliable = reliable;

        auto result = peer_connection_->CreateDataChannelOrError(label, &init);
        if (!result.ok()) return false;

        auto dc = result.MoveValue();
        AttachDataChannel(dc);
        return true;
    }

    bool SendDataChannelMessage(const std::string& label,
                                 const uint8_t* data, size_t size, bool is_binary) {
        auto it = data_channels_.find(label);
        if (it == data_channels_.end()) return false;

        rtc::CopyOnWriteBuffer buf(data, size);
        webrtc::DataBuffer buffer(buf, is_binary);
        return it->second->Send(buffer);
    }

    bool SendDataChannelMessage(const std::string& label, const std::string& text) {
        return SendDataChannelMessage(label,
            reinterpret_cast<const uint8_t*>(text.data()), text.size(), false);
    }

    void RestartIce() {
        if (peer_connection_)
            peer_connection_->RestartIce();
    }

    bool UpdateIceServers(const std::vector<IceServer>& servers) {
        if (!peer_connection_) return false;

        webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
        for (const auto& s : servers) {
            webrtc::PeerConnectionInterface::IceServer ice;
            for (const auto& url : s.urls) ice.urls.push_back(url);
            ice.username   = s.username;
            ice.password   = s.credential;
            rtc_config.servers.push_back(ice);
        }

        webrtc::RTCError error = peer_connection_->SetConfiguration(rtc_config);
        return error.ok();
    }

    PeerConnectionState GetConnectionState() const { return connection_state_; }
    int GetEncodeQualityStep() const { return 0; }
    bool ApplyViewerResolutionFeedback(double, double, bool&, int&, double&) { return false; }
    int GetResolutionAdaptationStep() const { return 0; }
    double GetResolutionScaleFactor() const { return 1.0; }
    void CleanupVideoEncoder() {}
    ResolutionAdaptationState* GetResolutionAdaptation() { return nullptr; }
    void ResetResolutionAdaptation() {}
    uint64_t ResetFrameDropCount() { return 0; }

private:
    class InternalDataChannelObserver : public webrtc::DataChannelObserver {
    public:
        InternalDataChannelObserver(WebRTCPeerImpl* owner,
                                    rtc::scoped_refptr<webrtc::DataChannelInterface> channel)
            : owner_(owner)
            , channel_(std::move(channel))
            , label_(channel_ ? channel_->label() : std::string()) {}

        void OnStateChange() override {
            if (owner_ && channel_) {
                owner_->HandleDataChannelStateChange(label_, channel_->state());
            }
        }

        void OnMessage(const webrtc::DataBuffer& buffer) override {
            if (owner_) {
                owner_->HandleDataChannelMessage(label_, buffer);
            }
        }

    private:
        WebRTCPeerImpl* owner_ = nullptr;
        rtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
        std::string label_;
    };

    // ── Callback interno de SDP criado ──────────────────────────────────

    void OnSDPCreated(webrtc::SessionDescriptionInterface* desc) {
        // Serializar o SDP.
        std::string sdp_str;
        desc->ToString(&sdp_str);

        // Setar como descrição local.
        auto observer = rtc::make_ref_counted<SetSDPObserver>(
            []() {},
            [](webrtc::RTCError) {});
        peer_connection_->SetLocalDescription(observer.get(), desc);

        // Notificar o listener.
        if (listener_) {
            SessionDescription sd;
            sd.type = (desc->GetType() == webrtc::SdpType::kOffer) ? "offer" : "answer";
            sd.sdp  = sdp_str;
            listener_->OnLocalDescription(sd);
        }
    }

    // ── PeerConnectionObserver ──────────────────────────────────────────

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}

    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> dc) override {
        AttachDataChannel(dc);
    }

    void OnIceGatheringChange(
        webrtc::PeerConnectionInterface::IceGatheringState state) override {
        if (!listener_) return;
        IceGatheringState our_state;
        switch (state) {
            case webrtc::PeerConnectionInterface::kIceGatheringNew:
                our_state = IceGatheringState::New; break;
            case webrtc::PeerConnectionInterface::kIceGatheringGathering:
                our_state = IceGatheringState::Gathering; break;
            case webrtc::PeerConnectionInterface::kIceGatheringComplete:
                our_state = IceGatheringState::Complete; break;
        }
        listener_->OnIceGatheringStateChanged(our_state);
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        if (!listener_) return;
        std::string sdp;
        candidate->ToString(&sdp);
        IceCandidate ic;
        ic.candidate       = sdp;
        ic.sdp_mid         = candidate->sdp_mid();
        ic.sdp_mline_index = candidate->sdp_mline_index();
        listener_->OnIceCandidate(ic);
    }

    void OnConnectionChange(
        webrtc::PeerConnectionInterface::PeerConnectionState state) override {
        switch (state) {
            case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
                connection_state_ = PeerConnectionState::New; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
                connection_state_ = PeerConnectionState::Connecting; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
                connection_state_ = PeerConnectionState::Connected; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
                connection_state_ = PeerConnectionState::Disconnected; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
                connection_state_ = PeerConnectionState::Failed; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
                connection_state_ = PeerConnectionState::Closed; break;
        }
        if (listener_)
            listener_->OnConnectionStateChanged(connection_state_);
    }

    void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        auto track = transceiver->receiver()->track();
        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
            video_track->AddOrUpdateSink(&video_sink_, rtc::VideoSinkWants());
        }
    }

    // Unused required overrides.
    void OnRenegotiationNeeded() override {}
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}
    void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}

    // ── DataChannelObserver ─────────────────────────────────────────────

    // ── VideoSink para receber frames decodificados (Viewer) ────────────

    void AttachDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> dc) {
        if (!dc) {
            return;
        }

        const std::string label = dc->label();
        if (auto it = data_channels_.find(label); it != data_channels_.end()) {
            it->second->UnregisterObserver();
        }

        auto observer = std::make_unique<InternalDataChannelObserver>(this, dc);
        dc->RegisterObserver(observer.get());
        data_channels_[label] = dc;
        data_channel_observers_[label] = std::move(observer);
        HandleDataChannelStateChange(label, dc->state());
    }

    void HandleDataChannelStateChange(
        const std::string& label,
        webrtc::DataChannelInterface::DataState state) {
        if (!listener_) {
            return;
        }

        if (state == webrtc::DataChannelInterface::kOpen) {
            listener_->OnDataChannelOpen(label);
        } else if (state == webrtc::DataChannelInterface::kClosed) {
            listener_->OnDataChannelClosed(label);
        }
    }

    void HandleDataChannelMessage(const std::string& label, const webrtc::DataBuffer& buffer) {
        if (!listener_) {
            return;
        }

        DataChannelMessage msg;
        msg.channel_label = label;
        msg.data.assign(buffer.data.data(), buffer.data.data() + buffer.data.size());
        msg.is_binary = buffer.binary;
        listener_->OnDataChannelMessage(msg);
    }

    class InternalVideoSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
    public:
        WebRTCPeerListener* listener = nullptr;

        void OnFrame(const webrtc::VideoFrame& frame) override {
            if (!listener) return;
            auto buffer = frame.video_frame_buffer()->ToI420();
            VideoFrame vf;
            vf.data_y      = buffer->DataY();
            vf.data_u      = buffer->DataU();
            vf.data_v      = buffer->DataV();
            vf.stride_y    = buffer->StrideY();
            vf.stride_u    = buffer->StrideU();
            vf.stride_v    = buffer->StrideV();
            vf.width       = buffer->width();
            vf.height      = buffer->height();
            vf.timestamp_us = frame.timestamp_us();
            // The libwebrtc path does not yet expose an app-specific metadata
            // channel for frame-qualified timings; keep the fields explicit so
            // callers can distinguish "not transported" from "not measured".
            vf.frame_id = 0;
            vf.capture_ready_timestamp_us = 0;
            vf.encode_ready_timestamp_us = 0;
            vf.send_started_timestamp_us = 0;
            listener->OnVideoFrame(vf);
        }
    };

    // ── Membros ─────────────────────────────────────────────────────────

    PeerConfig config_;
    WebRTCPeerListener* listener_ = nullptr;
    PeerConnectionState connection_state_ = PeerConnectionState::New;

    std::unique_ptr<rtc::Thread> network_thread_;
    std::unique_ptr<rtc::Thread> worker_thread_;
    std::unique_ptr<rtc::Thread> signaling_thread_;

    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface>        peer_connection_;
    rtc::scoped_refptr<FrameInjectorSource>                    video_source_;

    std::map<std::string, rtc::scoped_refptr<webrtc::DataChannelInterface>> data_channels_;
    std::map<std::string, std::unique_ptr<InternalDataChannelObserver>> data_channel_observers_;
    InternalVideoSink video_sink_;
};

} // namespace remotex::core

// ─────────────────────────────────────────────────────────────────────────────
// Stub quando libwebrtc NÃO está disponível
// ─────────────────────────────────────────────────────────────────────────────
#else // !REMOTEX_HAS_LIBWEBRTC
#include "webrtc_peer_stub.inc"
#endif // REMOTEX_HAS_LIBWEBRTC

// ─────────────────────────────────────────────────────────────────────────────
// Fachada pública (delega para WebRTCPeerImpl)
// ─────────────────────────────────────────────────────────────────────────────
namespace remotex::core {

WebRTCPeer::WebRTCPeer()
    : impl_(std::make_unique<WebRTCPeerImpl>()) {}

WebRTCPeer::~WebRTCPeer() = default;

void WebRTCPeer::SetListener(WebRTCPeerListener* listener)
{
    std::lock_guard lock(mutex_);
    listener_ = listener;
    impl_->SetListener(listener);
}

bool WebRTCPeer::Initialize(const PeerConfig& config)
{
    std::lock_guard lock(mutex_);
    config_ = config;
    return impl_->Initialize(config);
}

void WebRTCPeer::Shutdown()
{
    std::lock_guard lock(mutex_);
    impl_->Shutdown();
}

void WebRTCPeer::CreateOffer()
{
    std::lock_guard lock(mutex_);
    impl_->CreateOffer();
}

void WebRTCPeer::CreateAnswer()
{
    std::lock_guard lock(mutex_);
    impl_->CreateAnswer();
}

bool WebRTCPeer::SetRemoteDescription(const SessionDescription& desc)
{
    std::lock_guard lock(mutex_);
    return impl_->SetRemoteDescription(desc);
}

bool WebRTCPeer::AddIceCandidate(const IceCandidate& candidate)
{
    std::lock_guard lock(mutex_);
    return impl_->AddIceCandidate(candidate);
}

bool WebRTCPeer::AddVideoTrack(int width, int height, int fps)
{
    std::lock_guard lock(mutex_);
    return impl_->AddVideoTrack(width, height, fps);
}

void WebRTCPeer::UpdateVideoDimensions(int width, int height)
{
    std::lock_guard lock(mutex_);
    impl_->UpdateVideoDimensions(width, height);
}

bool WebRTCPeer::SendVideoFrame(const uint8_t* data, size_t size, int64_t timestamp_us)
{
    VideoFrameMetrics metrics{};
    metrics.capture_ready_timestamp_us = timestamp_us;
    metrics.encode_ready_timestamp_us = timestamp_us;
    metrics.send_started_timestamp_us = timestamp_us;
    return SendVideoFrame(data, size, timestamp_us, metrics);
}

bool WebRTCPeer::SendVideoFrame(const uint8_t* data,
                                size_t size,
                                int64_t timestamp_us,
                                const VideoFrameMetrics& metrics)
{
    std::lock_guard lock(mutex_);
    return impl_->SendVideoFrame(data, size, timestamp_us, metrics);
}

bool WebRTCPeer::CreateDataChannel(const std::string& label, bool ordered, bool reliable)
{
    std::lock_guard lock(mutex_);
    return impl_->CreateDataChannel(label, ordered, reliable);
}

bool WebRTCPeer::SendDataChannelMessage(const std::string& label,
                                         const uint8_t* data,
                                         size_t size,
                                         bool is_binary)
{
    std::lock_guard lock(mutex_);
    return impl_->SendDataChannelMessage(label, data, size, is_binary);
}

bool WebRTCPeer::SendDataChannelMessage(const std::string& label, const std::string& text)
{
    std::lock_guard lock(mutex_);
    return impl_->SendDataChannelMessage(label, text);
}

void WebRTCPeer::RestartIce()
{
    std::lock_guard lock(mutex_);
    impl_->RestartIce();
}

bool WebRTCPeer::UpdateIceServers(const std::vector<IceServer>& servers)
{
    std::lock_guard lock(mutex_);
    return impl_->UpdateIceServers(servers);
}

PeerConnectionState WebRTCPeer::GetConnectionState() const
{
    std::lock_guard lock(mutex_);
    return impl_->GetConnectionState();
}

int WebRTCPeer::GetEncodeQualityStep() const
{
    std::lock_guard lock(mutex_);
    return impl_->GetEncodeQualityStep();
}

bool WebRTCPeer::ApplyViewerResolutionFeedback(double avg_upload_ms,
                                               double dropped_fps,
                                               bool& downscaled,
                                               int& step,
                                               double& scale_factor)
{
    std::lock_guard lock(mutex_);
    return impl_->ApplyViewerResolutionFeedback(avg_upload_ms,
                                                dropped_fps,
                                                downscaled,
                                                step,
                                                scale_factor);
}

int WebRTCPeer::GetResolutionAdaptationStep() const
{
    std::lock_guard lock(mutex_);
    return impl_->GetResolutionAdaptationStep();
}

double WebRTCPeer::GetResolutionScaleFactor() const
{
    std::lock_guard lock(mutex_);
    return impl_->GetResolutionScaleFactor();
}

void WebRTCPeer::CleanupVideoEncoder()
{
    std::lock_guard lock(mutex_);
    impl_->CleanupVideoEncoder();
}

ResolutionAdaptationState* WebRTCPeer::GetResolutionAdaptation()
{
    std::lock_guard lock(mutex_);
    return impl_->GetResolutionAdaptation();
}

void WebRTCPeer::ResetResolutionAdaptation()
{
    std::lock_guard lock(mutex_);
    impl_->ResetResolutionAdaptation();
}

uint64_t WebRTCPeer::ResetFrameDropCount()
{
    std::lock_guard lock(mutex_);
    return impl_->ResetFrameDropCount();
}

bool WebRTCPeer::IsConnected() const
{
    return GetConnectionState() == PeerConnectionState::Connected;
}

} // namespace remotex::core

