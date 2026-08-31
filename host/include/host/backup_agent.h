#pragma once
// RemoteX — host/backup_agent.h
// Agente de backup agendado com suporte a backup incremental.
// Mantém manifesto local JSON (path → sha256 + size + modified).
// Compacta arquivos alterados em .tar.gz e faz upload via URL pré-assinada.

#include "core/auth_client.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace remotex::host {

// ─── Configuração recebida do backend ────────────────────────────────────────

struct BackupConfig {
    std::vector<std::string> folders;
    uint32_t                 interval_hours  = 2;
    uint32_t                 retention_count = 7;
    bool                     enabled         = true;
};

// ─── Entrada do manifesto local ──────────────────────────────────────────────

struct ManifestEntry {
    std::string sha256;
    uint64_t    size     = 0;
    std::string modified;   // ISO 8601
};

// ─── Resultado de um ciclo de backup ─────────────────────────────────────────

struct BackupCycleResult {
    bool        success       = false;
    uint32_t    files_new     = 0;
    uint32_t    files_changed = 0;
    uint32_t    files_removed = 0;
    uint64_t    archive_size  = 0;   // Tamanho do .tar.gz
    uint32_t    duration_sec  = 0;
    std::string error;
};

// ─── Listener (opcional) ─────────────────────────────────────────────────────

class BackupAgentListener {
public:
    virtual ~BackupAgentListener() = default;
    virtual void OnBackupCycleComplete(const BackupCycleResult& result) = 0;
};

// ─── BackupAgent ─────────────────────────────────────────────────────────────

class BackupAgent {
public:
    BackupAgent();
    ~BackupAgent();

    BackupAgent(const BackupAgent&)            = delete;
    BackupAgent& operator=(const BackupAgent&) = delete;

    // Configura o agente.
    // api_host/api_port: endpoint da API backend.
    // machine_id: ID da máquina registrada.
    // manifest_path: caminho do arquivo de manifesto local.
    // temp_dir: diretório para arquivos temporários (.tar.gz).
    struct Config {
        std::string               api_host;
        uint16_t                  api_port   = 8081;
        bool                      use_tls    = false;
        std::string               machine_id;
        std::filesystem::path     manifest_path;
        std::filesystem::path     temp_dir;
    };

    void Initialize(const Config& config, remotex::core::AuthClient* auth);
    void SetListener(BackupAgentListener* listener);

    // Inicia o scheduler de backup em background.
    void Start();

    // Para o scheduler.
    void Stop();

    bool IsRunning() const { return running_; }

    // Executa um ciclo de backup imediatamente (bloqueante).
    BackupCycleResult RunBackupCycle();

private:
    // ── Manifesto ─────────────────────────────────────────────────────────
    using Manifest = std::map<std::string, ManifestEntry>;

    Manifest LoadManifest();
    void     SaveManifest(const Manifest& manifest);

    // ── Scanning ──────────────────────────────────────────────────────────
    struct FileInfo {
        std::filesystem::path path;
        uint64_t              size;
        std::string           modified;  // ISO 8601
    };

    std::vector<FileInfo> ScanFolders(const std::vector<std::string>& folders);
    static std::string    FileSHA256(const std::filesystem::path& path);
    static std::string    TimeToISO8601(std::filesystem::file_time_type tp);

    // ── Compressão ────────────────────────────────────────────────────────
    // Cria um .tar.gz contendo os arquivos especificados.
    // Retorna o caminho do arquivo criado, ou vazio em caso de erro.
    std::filesystem::path CreateArchive(const std::vector<std::filesystem::path>& files);

    // ── HTTP ──────────────────────────────────────────────────────────────
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    // Busca a configuração de backup do backend.
    bool FetchConfig(BackupConfig& out);

    // Solicita URL pré-assinada para upload.
    bool RequestUploadURL(uint64_t size_bytes, uint32_t file_count,
                          std::string& out_url, std::string& out_storage_key);

    // Faz PUT do arquivo no URL pré-assinado.
    bool UploadFile(const std::string& url, const std::filesystem::path& file_path);

    // Solicita deleção do snapshot mais antigo.
    bool RequestDeleteOldest();

    // HTTP genérico via WinHTTP.
    HttpResponse HttpRequest(const std::string& method,
                             const std::string& path,
                             const std::string& json_body = "");

    // Upload HTTP PUT para URL absoluta.
    HttpResponse HttpPut(const std::string& url,
                         const std::filesystem::path& file_path);

    // ── Scheduler ─────────────────────────────────────────────────────────
    void SchedulerLoop();

    Config                       config_;
    remotex::core::AuthClient*   auth_     = nullptr;
    BackupAgentListener*         listener_ = nullptr;
    std::thread                  thread_;
    std::atomic<bool>            running_{false};
    std::atomic<bool>            should_stop_{false};
};

} // namespace remotex::host
