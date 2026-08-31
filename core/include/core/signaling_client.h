#pragma once
// RemoteX — core/signaling_client.h
// Cliente WebSocket para o servidor de sinalização.
// Usa WinHTTP WebSocket API (nativo Windows, sem dependências extras).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace remotex::core {

// Tipos de mensagem do protocolo de sinalização (espelha server/internal/signaling/messages.go).
namespace signal_msg {
    inline constexpr const char* SessionJoin  = "session_join";
    inline constexpr const char* SessionLeave = "session_leave";
    inline constexpr const char* Offer        = "offer";
    inline constexpr const char* Answer       = "answer";
    inline constexpr const char* ICECandidate = "ice_candidate";
    inline constexpr const char* Error        = "error";
    inline constexpr const char* PeerJoined   = "peer_joined";
    inline constexpr const char* PeerLeft     = "peer_left";
    inline constexpr const char* Chat         = "chat";
    inline constexpr const char* ChatJoin     = "chat_join";
    inline constexpr const char* ChatLeave    = "chat_leave";
}

// Roles dentro de uma sessão.
namespace signal_role {
    inline constexpr const char* Host   = "host";
    inline constexpr const char* Viewer = "viewer";
}

// Eventos que o SignalingClient notifica à aplicação.
struct SignalingEvent {
    std::string type;        // Tipo da mensagem (ver signal_msg)
    std::string session_id;
    std::string role;
    std::string machine_id;  // Preenchido para mensagens de chat
    std::string payload;     // JSON string do campo payload
    std::string error;       // Preenchido apenas para type == "error"
};

// Interface de callback para eventos de sinalização.
class SignalingListener {
public:
    virtual ~SignalingListener() = default;

    virtual void OnConnected() = 0;
    virtual void OnDisconnected() = 0;
    virtual void OnSignalingMessage(const SignalingEvent& event) = 0;
    virtual void OnError(const std::string& error) = 0;
    virtual void OnSocketActivity() {}
};

// Estado da conexão WebSocket.
enum class SignalingState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
};

// Configuração do SignalingClient.
struct SignalingConfig {
    std::string host;                       // Ex: "localhost"
    uint16_t    port          = 8080;
    std::string path          = "/ws";
    std::function<std::string(bool)> token_provider; // Funcao provedora com opcao force_refresh
    std::string client_id;                  // Identificador deste client
    bool        use_tls       = false;
    uint32_t    max_reconnect_attempts = 0; // 0 = infinito
    uint32_t    initial_backoff_ms     = 500;
    uint32_t    max_backoff_ms         = 30000;
};

// Cliente WebSocket para comunicação com o servidor de sinalização.
//
// Uso típico:
//   SignalingClient client;
//   client.SetListener(&myListener);
//   client.Connect(config);
//   client.JoinSession("session-123", "viewer");
//   // ... recebe OnSignalingMessage com PeerJoined ...
//   client.SendOffer(sdp_json);
//   // ... recebe OnSignalingMessage com Answer ...
//   client.Disconnect();
class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    SignalingClient(const SignalingClient&)            = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Registra o listener de eventos (deve ser chamado antes de Connect).
    void SetListener(SignalingListener* listener);

    // Conecta ao servidor de sinalização. Retorna imediatamente;
    // a conexão é feita em background. OnConnected() é chamado quando pronto.
    bool Connect(const SignalingConfig& config);

    // Desconecta e para a thread de reconexão.
    void Disconnect();

    // Entra em uma sessão de sinalização.
    void JoinSession(const std::string& session_id, const std::string& role);

    // Sai da sessão atual.
    void LeaveSession();

    // Envia SDP offer (JSON string com campo "sdp").
    void SendOffer(const std::string& session_id,
                   const std::string& sdp,
                   const std::string& access_mode = "",
                   const std::string& backend_session_id = "");

    // Envia SDP answer (JSON string com campo "sdp").
    void SendAnswer(const std::string& session_id, const std::string& sdp);

    // Envia ICE candidate.
    void SendICECandidate(const std::string& session_id,
                          const std::string& candidate,
                          const std::string& sdp_mid,
                          int sdp_mline_index);

    // Envia um erro explicito para o peer da sessao.
    void SendError(const std::string& session_id, const std::string& error);

    // Inscreve-se para receber mensagens de chat de uma máquina.
    void ChatJoin(const std::string& machine_id);

    // Remove inscrição de chat de uma máquina.
    void ChatLeave(const std::string& machine_id);

    // Envia uma mensagem de chat para/de uma máquina.
    void SendChatMessage(const std::string& machine_id,
                         const std::string& from,
                         const std::string& text);

    // Estado atual da conexão.
    SignalingState GetState() const;

private:
    // Envia uma mensagem JSON genérica pelo WebSocket.
    void SendMessage(const std::string& json_str);

    // Monta e envia uma mensagem com type, session_id, role e payload opcionais.
    void SendSignalingMessage(const std::string& type,
                              const std::string& session_id = "",
                              const std::string& role = "",
                              const std::string& payload_json = "");

    // Thread principal de conexão e leitura.
    void ConnectionThread();

    // Tenta estabelecer a conexão WebSocket. Retorna true se sucesso.
    bool EstablishConnection();

    // Loop de leitura de mensagens. Retorna quando a conexão cai.
    void ReadLoop();

    // Thread de keepalive: envia ping a cada 30s para evitar timeout do proxy.
    void PingThread();

    // Fecha os handles WinHTTP atuais.
    void CloseHandles();

    // Parseia uma mensagem JSON recebida e despacha para o listener.
    void HandleMessage(const std::string& json_str);

    // Lógica de backoff exponencial.
    uint32_t CalculateBackoff(uint32_t attempt) const;

    SignalingConfig         config_;
    SignalingListener*      listener_ = nullptr;
    std::atomic<SignalingState> state_{SignalingState::Disconnected};
    std::string             last_error_;  // Último erro de conexão (diagnóstico)

    // WinHTTP handles.
    HINTERNET               session_handle_  = nullptr;
    HINTERNET               connection_      = nullptr;
    HINTERNET               request_         = nullptr;
    HINTERNET               websocket_       = nullptr;

    // Thread de conexão.
    std::thread             conn_thread_;
    std::atomic<bool>       should_stop_{false};
    std::mutex              send_mutex_;         // Protege envios simultâneos
    std::mutex              connect_mutex_;      // Protege EstablishConnection/CloseHandles

    // Thread de ping/keepalive.
    std::thread             ping_thread_;
    std::condition_variable ping_cv_;
    std::mutex              ping_mutex_;
    std::atomic<bool>       native_ping_enabled_{true};
};

} // namespace remotex::core
