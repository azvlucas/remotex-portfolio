#pragma once
// RemoteX — host/recording_uploader.h
// Upload de gravações MP4 para o backend via presigned URL (R2).
// Reutiliza padrão WinHTTP do BackupAgent.

#include "core/auth_client.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace remotex::host {

class RecordingUploaderTestPeer;

class RecordingUploader {
public:
    struct Config {
        std::string api_host;
        uint16_t    api_port   = 8081;
        bool        use_tls    = false;
        std::string machine_id;
        int         resolve_timeout_ms = 5000;
        int         connect_timeout_ms = 5000;
        int         send_timeout_ms    = 5000;
        int         receive_timeout_ms = 10000;
    };

    RecordingUploader();
    ~RecordingUploader();

    RecordingUploader(const RecordingUploader&)            = delete;
    RecordingUploader& operator=(const RecordingUploader&) = delete;

    void Initialize(const Config& config, remotex::core::AuthClient* auth);

    // Faz upload assíncrono do MP4 e deleta o arquivo local ao concluir.
    // session_id: ID da sessão no backend (pode ser vazio).
    // duration_sec: duração estimada da gravação em segundos.
    void UploadAsync(const std::filesystem::path& mp4_path,
                     const std::string& session_id,
                     int duration_sec);

private:
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    void UploadWorker(std::filesystem::path mp4_path,
                      std::string session_id,
                      int duration_sec);

    HttpResponse HttpRequest(const std::string& method,
                             const std::string& path,
                             const std::string& json_body = "");

    HttpResponse HttpPut(const std::string& url,
                         const std::filesystem::path& file_path);

    static std::string JsonGetString(const std::string& json, const std::string& key);

    friend class RecordingUploaderTestPeer;

    Config                       config_;
    remotex::core::AuthClient*   auth_     = nullptr;
    std::thread                  thread_;
    std::atomic<bool>            uploading_{false};
};

} // namespace remotex::host
