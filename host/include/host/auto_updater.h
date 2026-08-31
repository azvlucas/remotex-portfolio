#pragma once
// RemoteX — host/auto_updater.h
// Sistema de auto-update: verifica nova versão a cada 6h, baixa binário,
// verifica assinatura Ed25519, e reinicia o serviço Windows.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace remotex::core { class AuthClient; }

namespace remotex::host {

enum class AutoUpdaterRestartMode {
    None,
    ExternalHelper,
    ScmRecovery,
};

namespace detail {
std::vector<uint8_t> LoadEd25519PublicKeyFile(const std::filesystem::path& path);
bool VerifyFileEd25519Signature(const std::filesystem::path& file_path,
                                std::span<const uint8_t> public_key,
                                const std::string& signature_hex);
enum class ReleaseTrustResult {
    Trusted,
    MissingPublicKey,
    MissingSignature,
    InvalidSignature,
};
ReleaseTrustResult ValidateDownloadedRelease(const std::filesystem::path& file_path,
                                             std::span<const uint8_t> public_key,
                                             const std::string& signature_hex);
} // namespace detail

// Configuração do auto-updater.
struct AutoUpdaterConfig {
    std::string api_host;
    uint16_t    api_port   = 8081;
    bool        use_tls    = false;

    std::string current_version;           // Ex: "1.0.0"
    std::string service_name = "RemoteXHost";  // Nome do serviço Windows

    // Chave pública Ed25519 (32 bytes) para verificar assinatura do binário.
    std::vector<uint8_t> ed25519_public_key;

    int check_interval_sec = 1 * 3600;    // 1 hora
};

// Resultado de uma verificação de update.
struct UpdateCheckResult {
    bool        update_available = false;
    std::string latest_version;
    std::string download_url;       // URL para baixar o binário
    std::string signature_hex;      // Assinatura Ed25519 em hex (128 chars)
    std::string sha256_hex;         // SHA-256 hex do binário (64 chars, opcional)
    int64_t     file_size = 0;
};

// Listener para eventos do auto-updater.
class AutoUpdaterListener {
public:
    virtual ~AutoUpdaterListener() = default;

    virtual void OnUpdateCheckStarted() {}
    virtual void OnUpdateAvailable(const std::string& version) {}
    virtual void OnDownloadProgress(int64_t downloaded, int64_t total) {}
    virtual void OnUpdateWarning(const std::string& warning) {}
    virtual void OnUpdateReady(const std::string& version) {}
    virtual void OnUpdateApplied(const std::string& version) {}
    virtual void OnUpdateError(const std::string& error) {}

    // Called when the update has been handed off and the service must recycle.
    virtual void OnRestartRequired() {}
};

// Auto-updater do agente Host.
//
// Fluxo:
//   1. A cada 1h, faz GET /update/check?current_version=X.Y.Z
//   2. Se há nova versão, baixa o binário da URL retornada
//   3. Verifica assinatura Ed25519 do binário baixado
//   4. Renomeia o exe atual para .bak e copia o novo binário no lugar
//   5. Sinaliza OnRestartRequired — o watcher para o serviço e o SCM reinicia
class AutoUpdater {
public:
    AutoUpdater();
    ~AutoUpdater();

    AutoUpdater(const AutoUpdater&)            = delete;
    AutoUpdater& operator=(const AutoUpdater&) = delete;

    void SetListener(AutoUpdaterListener* listener);

    // Inicializa com configuração e auth client (para Bearer token).
    void Configure(const AutoUpdaterConfig& config, remotex::core::AuthClient* auth);

    // Inicia o loop de verificação em background.
    void Start();

    // Para o loop de verificação.
    void Stop();

    // Verifica agora (pode ser chamado manualmente).
    // Retorna true se um update foi encontrado e aplicado.
    bool CheckAndApplyNow();

    // Apenas verifica se há update disponível.
    UpdateCheckResult CheckForUpdate();

    bool IsRunning() const { return running_.load(); }

    // Returns true if the binary was swapped and the service must restart.
    bool NeedsRestart() const { return restart_needed_.load(); }
    AutoUpdaterRestartMode GetRestartMode() const { return restart_mode_.load(); }

private:
    // Loop de background.
    void WorkerLoop();

    // Etapas do update.
    UpdateCheckResult DoCheckForUpdate();
    bool DownloadBinary(const std::string& url, const std::string& dest_path, int64_t expected_size);
    bool VerifySignature(const std::string& file_path, const std::string& signature_hex);
    bool ApplyUpdate(const std::string& new_binary_path, const std::string& target_version);

    // HTTP GET via WinHTTP.
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };
    HttpResponse HttpGet(const std::string& url_or_path);
    bool HttpDownload(const std::string& url, const std::string& dest_path,
                      int64_t expected_size);

    AutoUpdaterConfig              config_;
    remotex::core::AuthClient*     auth_     = nullptr;
    AutoUpdaterListener*           listener_ = nullptr;

    std::thread                    thread_;
    std::atomic<bool>              running_{false};
    std::atomic<bool>              stop_requested_{false};
    std::atomic<bool>              restart_needed_{false};
    std::atomic<AutoUpdaterRestartMode> restart_mode_{AutoUpdaterRestartMode::None};
    mutable std::mutex             mutex_;
};

} // namespace remotex::host
