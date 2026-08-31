#pragma once
// RemoteX — viewer/auto_updater.h
// Auto-update for the viewer app: check → download zip → verify → batch swap → relaunch.

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

namespace remotex::viewer {

enum class ViewerUpdateArtifactKind {
    None,
    PackageZip,
    ExecutableBinary,
};

struct ViewerUpdateConfig {
    std::string api_host;
    uint16_t    api_port   = 8081;
    bool        use_tls    = false;

    std::string current_version;
    std::vector<uint8_t> ed25519_public_key;

    int check_interval_sec = 1 * 3600;    // 1 hour
};

struct ViewerUpdateCheckResult {
    bool        update_available = false;
    std::string latest_version;
    std::string download_url;
    std::string signature_hex;
    std::string sha256_hex;
    int64_t     file_size = 0;
};

class ViewerAutoUpdaterListener {
public:
    virtual ~ViewerAutoUpdaterListener() = default;
    virtual void OnUpdateCheckStarted() {}
    virtual void OnUpdateAvailable(const std::string& version) {}
    virtual void OnDownloadProgress(int64_t downloaded, int64_t total) {}
    virtual void OnUpdateReady(const std::string& version) {}
    virtual void OnUpdateError(const std::string& error) {}
    virtual void OnUpdateWarning(const std::string& warning) {}
};

class ViewerAutoUpdater {
public:
    ViewerAutoUpdater();
    ~ViewerAutoUpdater();

    ViewerAutoUpdater(const ViewerAutoUpdater&) = delete;
    ViewerAutoUpdater& operator=(const ViewerAutoUpdater&) = delete;

    void SetListener(ViewerAutoUpdaterListener* listener);
    void Configure(const ViewerUpdateConfig& config, remotex::core::AuthClient* auth);

    void Start();
    void Stop();

    ViewerUpdateCheckResult CheckForUpdate();

    // Downloads, verifies, stages update. Returns true if ready to apply.
    bool DownloadAndStage();

    // Writes batch script to %TEMP%, launches it, returns true if viewer should exit.
    bool ApplyAndRelaunch();

    bool IsRunning() const { return running_.load(); }
    bool HasPendingUpdate() const { return update_staged_.load(); }
    std::string PendingVersion() const;

private:
    void WorkerLoop();
    ViewerUpdateCheckResult DoCheckForUpdate();

    struct HttpResponse { int status_code = 0; std::string body; };
    HttpResponse HttpGet(const std::string& path);
    bool HttpDownload(const std::string& url, const std::string& dest_path, int64_t expected_size);

    ViewerUpdateConfig          config_;
    remotex::core::AuthClient*  auth_     = nullptr;
    ViewerAutoUpdaterListener*  listener_ = nullptr;

    std::thread                 thread_;
    std::atomic<bool>           running_{false};
    std::atomic<bool>           stop_requested_{false};
    std::atomic<bool>           update_staged_{false};

    mutable std::mutex          mutex_;
    std::string                 pending_version_;
    std::string                 pending_signature_hex_;
    std::string                 pending_sha256_hex_;
    int64_t                     pending_file_size_ = 0;
    std::string                 pending_download_url_;
    std::filesystem::path       staging_dir_;
    std::filesystem::path       staged_artifact_path_;
    ViewerUpdateArtifactKind    staged_artifact_kind_ = ViewerUpdateArtifactKind::None;
};

} // namespace remotex::viewer
