#pragma once
// RemoteX — core/webrtc_peer.h
// Wrapper genérico da libwebrtc PeerConnection.
// Usado tanto pelo Host (envia vídeo, recebe input) quanto pelo Viewer (recebe vídeo, envia input).
//
// Quando LIBWEBRTC_ROOT não está definido no CMake, este módulo compila
// mas as operações retornam erro (sem runtime WebRTC disponível).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace remotex::core {

class SignalingClient;  // Forward declaration
class ResolutionAdaptationState;

// ─── Tipos de dados ─────────────────────────────────────────────────────────

// Configuração de um ICE server (STUN ou TURN).
struct IceServer {
    std::vector<std::string> urls;       // e.g. "stun:stun.l.google.com:19302"
    std::string              username;   // Apenas para TURN
    std::string              credential; // Apenas para TURN
};

// Configuração da PeerConnection.
struct PeerConfig {
    std::vector<IceServer> ice_servers;
    std::string            session_id;
    bool                   is_offerer = false;  // true = Viewer cria offer; false = Host cria answer

    // Relay TCP (usado pelo stub sem libwebrtc).
    // Host e Viewer conectam ao relay, que faz o pareamento por session_id.
    std::string relay_host = "localhost";
    uint16_t    relay_port = 9100;
    uint32_t    relay_protocol_version = 1;
    uint64_t    relay_ticket_expires_unix = 0;
    uint32_t    relay_capabilities = 0;
    std::string relay_ticket;
};

// Estado da conexão P2P.
enum class PeerConnectionState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed
};

// Estado do ICE gathering.
enum class IceGatheringState {
    New,
    Gathering,
    Complete
};

// Candidato ICE gerado localmente.
struct IceCandidate {
    std::string candidate;
    std::string sdp_mid;
    int         sdp_mline_index = 0;
};

// Descritor de sessão (SDP).
struct SessionDescription {
    std::string type;  // "offer" ou "answer"
    std::string sdp;
};

// Frame de vídeo decodificado (recebido pelo Viewer).
struct VideoFrameMetrics {
    uint64_t frame_id = 0;
    int64_t capture_ready_timestamp_us = 0;
    int64_t encode_ready_timestamp_us = 0;
    int64_t send_started_timestamp_us = 0;
};

struct VideoFrame {
    const uint8_t* data_y    = nullptr;
    const uint8_t* data_u    = nullptr;
    const uint8_t* data_v    = nullptr;
    int            stride_y  = 0;
    int            stride_u  = 0;
    int            stride_v  = 0;
    int            width     = 0;
    int            height    = 0;
    int64_t        timestamp_us = 0;
    uint64_t       frame_id = 0;
    int64_t        capture_ready_timestamp_us = 0;
    int64_t        encode_ready_timestamp_us = 0;
    int64_t        send_started_timestamp_us = 0;
};

// Dados recebidos via Data Channel.
struct DataChannelMessage {
    std::string channel_label;
    std::vector<uint8_t> data;
    bool is_binary = false;
};

// ─── Callbacks ──────────────────────────────────────────────────────────────

// Interface de callbacks para eventos do WebRTCPeer.
class WebRTCPeerListener {
public:
    virtual ~WebRTCPeerListener() = default;

    // SDP local gerado (offer ou answer) — deve ser enviado ao peer via sinalização.
    virtual void OnLocalDescription(const SessionDescription& desc) = 0;

    // Candidato ICE local gerado — deve ser enviado ao peer via sinalização.
    virtual void OnIceCandidate(const IceCandidate& candidate) = 0;

    // Estado da conexão P2P mudou.
    virtual void OnConnectionStateChanged(PeerConnectionState state) = 0;

    // Estado do ICE gathering mudou.
    virtual void OnIceGatheringStateChanged(IceGatheringState state) = 0;

    // Frame de vídeo decodificado recebido (apenas Viewer).
    virtual void OnVideoFrame(const VideoFrame& frame) = 0;

    // Dados recebidos via Data Channel.
    virtual void OnDataChannelMessage(const DataChannelMessage& message) = 0;

    // Data Channel aberto.
    virtual void OnDataChannelOpen(const std::string& label) = 0;

    // Data Channel fechado.
    virtual void OnDataChannelClosed(const std::string& label) = 0;
};

// ─── WebRTCPeer ─────────────────────────────────────────────────────────────

// Implementação interna — opaca para quem usa o WebRTCPeer.
class WebRTCPeerImpl;

// Wrapper da libwebrtc PeerConnection.
//
// Fluxo Viewer (offerer):
//   peer.Initialize(config);          // is_offerer = true
//   peer.CreateOffer();               // → OnLocalDescription(offer)
//   // envia offer via sinalização
//   peer.SetRemoteDescription(answer);
//   peer.AddIceCandidate(candidate);
//
// Fluxo Host (answerer):
//   peer.Initialize(config);          // is_offerer = false
//   peer.AddVideoTrack(width, height);
//   peer.SetRemoteDescription(offer); // → cria answer automaticamente
//   // → OnLocalDescription(answer)
//   peer.AddIceCandidate(candidate);
//   peer.SendVideoFrame(frame_data, ...);
class WebRTCPeer {
public:
    WebRTCPeer();
    ~WebRTCPeer();

    WebRTCPeer(const WebRTCPeer&)            = delete;
    WebRTCPeer& operator=(const WebRTCPeer&) = delete;

    // Registra o listener de eventos.
    void SetListener(WebRTCPeerListener* listener);

    // Inicializa a PeerConnectionFactory e cria a PeerConnection.
    // Deve ser chamado uma vez antes de qualquer outra operação.
    bool Initialize(const PeerConfig& config);

    // Fecha a PeerConnection e libera recursos.
    void Shutdown();

    // ── Negociação SDP ──────────────────────────────────────────────────

    // Cria um SDP offer (Viewer). Resultado via OnLocalDescription.
    void CreateOffer();

    // Cria um SDP answer (Host). Resultado via OnLocalDescription.
    // Normalmente chamado automaticamente após SetRemoteDescription(offer).
    void CreateAnswer();

    // Define a descrição remota (offer do Viewer ou answer do Host).
    // Se for um offer e is_offerer==false, CreateAnswer() é chamado automaticamente.
    bool SetRemoteDescription(const SessionDescription& desc);

    // Adiciona um candidato ICE remoto.
    bool AddIceCandidate(const IceCandidate& candidate);

    // ── Mídia (Host) ────────────────────────────────────────────────────

    // Adiciona uma video track à PeerConnection.
    // O Host chama isto antes de CreateAnswer.
    bool AddVideoTrack(int width, int height, int fps = 30);

    // Atualiza as dimensões do vídeo (ex: após troca de monitor).
    void UpdateVideoDimensions(int width, int height);

    // Envia um frame bruto do capture path; o transporte faz encode quando disponível.
    bool SendVideoFrame(const uint8_t* data, size_t size, int64_t timestamp_us);
    bool SendVideoFrame(const uint8_t* data,
                        size_t size,
                        int64_t timestamp_us,
                        const VideoFrameMetrics& metrics);

    // ── Data Channels ───────────────────────────────────────────────────

    // Cria um Data Channel com o label especificado.
    // O offerer (Viewer) cria os channels; o answerer (Host) recebe via callback.
    bool CreateDataChannel(const std::string& label, bool ordered = true, bool reliable = true);

    // Envia dados por um Data Channel existente.
    bool SendDataChannelMessage(const std::string& label,
                                const uint8_t* data,
                                size_t size,
                                bool is_binary = true);

    bool SendDataChannelMessage(const std::string& label, const std::string& text);

    // ── ICE Restart ─────────────────────────────────────────────────────

    // Inicia um ICE restart (gera novo offer com ice-restart flag).
    void RestartIce();

    // Atualiza a lista de ICE servers (para ICE restart com novos TURN servers).
    bool UpdateIceServers(const std::vector<IceServer>& servers);

    // ── Estado ──────────────────────────────────────────────────────────

    PeerConnectionState GetConnectionState() const;
    int GetEncodeQualityStep() const;
    bool ApplyViewerResolutionFeedback(double avg_upload_ms,
                                       double dropped_fps,
                                       bool& downscaled,
                                       int& step,
                                       double& scale_factor);
    int GetResolutionAdaptationStep() const;
    double GetResolutionScaleFactor() const;
    void CleanupVideoEncoder();
    ResolutionAdaptationState* GetResolutionAdaptation();
    void ResetResolutionAdaptation();
    uint64_t ResetFrameDropCount();
    bool IsConnected() const;

private:
    std::unique_ptr<WebRTCPeerImpl> impl_;
    WebRTCPeerListener*             listener_ = nullptr;
    PeerConfig                      config_;
    mutable std::mutex              mutex_;
};

} // namespace remotex::core
