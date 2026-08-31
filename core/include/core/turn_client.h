#pragma once
// RemoteX — core/turn_client.h
// Cliente HTTP para obter credenciais TURN e lista de servidores.
// Usado pelo Viewer antes de iniciar a PeerConnection.

#include "core/auth_client.h"
#include "core/webrtc_peer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace remotex::core {

// Credenciais temporárias TURN (formato REST API).
struct TurnCredentials {
    std::string              username;
    std::string              password;
    int                      ttl = 0;
    std::vector<std::string> uris;    // stun:host:port, turn:host:port?transport=udp, etc.
};

// Informação de um servidor TURN.
struct TurnServerInfo {
    std::string host;
    int         port    = 3478;
    bool        healthy = false;
};

// Configuração do TurnClient.
struct TurnClientConfig {
    std::string api_host;
    uint16_t    api_port = 8081;
    bool        use_tls  = false;
};

// Cliente HTTP para endpoints /turn/ do backend.
class TurnClient {
public:
    TurnClient() = default;
    ~TurnClient() = default;

    void Configure(const TurnClientConfig& config, AuthClient* auth);

    // Busca credenciais temporárias TURN.
    // Retorna true se obteve credenciais com sucesso.
    bool FetchCredentials(const std::string& user_id, TurnCredentials& out);

    // Busca lista de servidores TURN ativos.
    bool FetchServers(std::vector<TurnServerInfo>& out);

    // Converte TurnCredentials para uma lista de IceServer pronta para PeerConfig.
    static std::vector<IceServer> CredentialsToIceServers(const TurnCredentials& creds);

private:
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    HttpResponse HttpGet(const std::string& path);

    TurnClientConfig config_;
    AuthClient*      auth_ = nullptr;
};

} // namespace remotex::core
