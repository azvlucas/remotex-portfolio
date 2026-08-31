#pragma once
// RemoteX — core/auth_client.h
// Cliente HTTP para autenticação (login, register, refresh).
// Armazena tokens de forma segura via Windows Credential Manager.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace remotex::core {

// Resultado de uma operação de autenticação.
struct AuthResult {
    bool        success = false;
    std::string user_id;
    std::string role;
    std::string access_token;
    std::string refresh_token;
    int64_t     expires_in = 0;     // segundos
    std::string error;              // mensagem de erro se !success
};

// Configuração do AuthClient.
struct AuthConfig {
    std::string api_host;           // Ex: "localhost"
    uint16_t    api_port  = 8080;
    bool        use_tls   = false;
    std::string credential_target = "RemoteX";  // Nome no Credential Manager
    std::vector<std::string> legacy_credential_targets;  // Targets antigos a migrar.
};

// Cliente de autenticação para a API REST do backend.
//
// Uso:
//   AuthClient auth;
//   auth.Configure(config);
//   auto result = auth.Login("user@example.com", "password");
//   if (result.success) { /* authenticated */ }
//   // Tokens são armazenados automaticamente no Credential Manager.
//   // Para futuras sessões:
//   if (auth.LoadStoredTokens()) { /* tokens loaded */ }
//   if (auth.IsAuthenticated()) { /* still valid or refreshed */ }
class AuthClient {
public:
    AuthClient()  = default;
    ~AuthClient() = default;

    AuthClient(const AuthClient&)            = delete;
    AuthClient& operator=(const AuthClient&) = delete;

    // Configura o endpoint da API.
    void Configure(const AuthConfig& config);

    // ── Operações de autenticação ───────────────────────────────────────

    // Registra um novo usuário.
    AuthResult Register(const std::string& email,
                        const std::string& password,
                        const std::string& name);

    // Login com email e senha.
    AuthResult Login(const std::string& email, const std::string& password);

    // Renova o access token usando o refresh token.
    AuthResult RefreshToken();

    // ── Estado ──────────────────────────────────────────────────────────

    // Retorna true se há um access token armazenado (não verifica expiração).
    bool IsAuthenticated() const;

    // Retorna o access token atual (para uso em headers de API).
    std::string GetAccessToken() const;

    // Retorna o user ID do último login.
    std::string GetUserID() const;

    // Retorna a role do usuário autenticado.
    std::string GetRole() const;

    // Retorna o refresh token atual.
    std::string GetRefreshToken() const;

    // Configura tokens diretamente (ex: carregados de credentials.json).
    void SetTokensDirect(const std::string& user_id,
                         const std::string& access_token,
                         const std::string& refresh_token);

    // Limpa os tokens (logout).
    void Logout();

    // ── Armazenamento seguro (Windows Credential Manager) ───────────────

    // Salva os tokens atuais no Credential Manager.
    bool SaveTokens() const;

    // Carrega tokens do Credential Manager.
    // Retorna true se encontrou tokens salvos.
    bool LoadStoredTokens();

    // Remove tokens do Credential Manager.
    bool ClearStoredTokens();

private:
    // Faz uma requisição HTTP POST e retorna o body da resposta.
    // Retorna {status_code, response_body}. status_code 0 = erro de rede.
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };
    HttpResponse HttpPost(const std::string& path, const std::string& json_body);

    // Parseia a resposta JSON de auth em AuthResult.
    AuthResult ParseAuthResponse(const HttpResponse& resp);

    AuthConfig  config_;
    std::string user_id_;
    std::string role_;
    std::string access_token_;
    std::string refresh_token_;
    mutable std::mutex mutex_;
};

} // namespace remotex::core
