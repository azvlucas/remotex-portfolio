#pragma once
// RemoteX — updater/host_updater.h
// Check, download, verify (Ed25519 + SHA-256) and apply updates to the host binary.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace remotex::core { class AuthClient; }

namespace remotex::updater {

struct HostUpdateConfig {
    std::string api_host;
    uint16_t    api_port   = 443;
    bool        use_tls    = true;
    std::string current_host_version;
    std::string host_service_name = "RemoteXHost";
    std::vector<uint8_t> ed25519_public_key;
};

struct UpdateCheckResult {
    bool        update_available = false;
    std::string latest_version;
    std::string download_url;
    std::string signature_hex;
    std::string sha256_hex;
    int64_t     file_size = 0;
};

using LogFn = std::function<void(const std::string&)>;

class HostUpdater {
public:
    void Configure(const HostUpdateConfig& config, remotex::core::AuthClient* auth, LogFn log_fn);

    // Check for host update, download, verify and apply.
    // Returns true if an update was applied and the host service was restarted.
    bool CheckAndApply();

    // Just check — does not download or apply.
    UpdateCheckResult CheckForUpdate();

    // Returns the version that was last applied, or empty if none.
    std::string LastAppliedVersion() const { return last_applied_version_; }

private:
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    UpdateCheckResult DoCheckForUpdate();
    bool DownloadBinary(const std::string& url, const std::filesystem::path& dest, int64_t expected_size);
    bool VerifyDownload(const std::filesystem::path& file, const std::string& signature_hex, const std::string& sha256_hex);
    bool ApplyHostUpdate(const std::filesystem::path& new_binary, const std::string& new_version);

    HttpResponse HttpGet(const std::string& path);
    bool HttpDownloadToFile(const std::string& path, const std::filesystem::path& dest, int64_t expected_size);

    void Log(const std::string& msg);

    HostUpdateConfig           config_;
    remotex::core::AuthClient* auth_ = nullptr;
    LogFn                      log_fn_;
    std::string                last_applied_version_;
};

// Shared helpers (also used by SelfUpdater).
std::vector<uint8_t> LoadEd25519PublicKeyFile(const std::filesystem::path& path);
bool StopNamedService(const std::wstring& service_name, DWORD timeout_ms);
bool StartNamedService(const std::wstring& service_name);
bool WaitForFileAvailability(const std::filesystem::path& path, DWORD timeout_ms);

} // namespace remotex::updater
