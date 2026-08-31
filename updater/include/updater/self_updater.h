#pragma once
// RemoteX — updater/self_updater.h
// Self-update: the updater renames itself, copies the new binary, and exits
// with code 100 so that SCM recovery restarts the service with the new binary.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace remotex::core { class AuthClient; }

namespace remotex::updater {

struct SelfUpdateConfig {
    std::string api_host;
    uint16_t    api_port   = 443;
    bool        use_tls    = true;
    std::string current_version;
    std::vector<uint8_t> ed25519_public_key;
};

using LogFn = std::function<void(const std::string&)>;

class SelfUpdater {
public:
    void Configure(const SelfUpdateConfig& config, remotex::core::AuthClient* auth, LogFn log_fn);

    // Check for self-update. If available, downloads, verifies, renames self to .bak,
    // copies new binary, and returns true. Caller should exit with code 100.
    bool CheckAndApply();

    // Clean up .bak from previous self-update.
    void CleanupPreviousBackup();

private:
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    HttpResponse HttpGet(const std::string& path);
    bool HttpDownloadToFile(const std::string& path, const std::filesystem::path& dest, int64_t expected_size);

    void Log(const std::string& msg);

    SelfUpdateConfig           config_;
    remotex::core::AuthClient* auth_ = nullptr;
    LogFn                      log_fn_;
};

} // namespace remotex::updater
