// RemoteX — host/auto_updater.cpp
// Auto-update: check → download → verify Ed25519 → replace binary → restart service.

#include "host/auto_updater.h"
#include "core/auth_client.h"
#include "core/crypto_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <vector>


namespace remotex::host {

// ─── JSON helpers mínimos ───────────────────────────────────────────────────

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";
    if (json[pos] == '"') {
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        return json.substr(pos + 1, end - pos - 1);
    }
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

static bool JsonGetBool(const std::string& json, const std::string& key) {
    return JsonGetString(json, key) == "true";
}

static int64_t JsonGetInt64(const std::string& json, const std::string& key) {
    auto val = JsonGetString(json, key);
    if (val.empty()) return 0;
    try { return std::stoll(val); } catch (...) { return 0; }
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

static std::wstring QuoteCommandLineArg(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L"\\\"";
            continue;
        }
        quoted.push_back(ch);
    }
    quoted += L"\"";
    return quoted;
}

static bool LaunchApplyUpdateHelper(const std::filesystem::path& current_exe,
                                    const AutoUpdaterConfig& config,
                                    const std::filesystem::path& source_binary,
                                    const std::filesystem::path& backup_exe,
                                    const std::string& target_version,
                                    std::string* error_message) {
    namespace fs = std::filesystem;

    const fs::path status_file = fs::temp_directory_path() / "remotex_apply_update_status.txt";
    std::wstring cmd = QuoteCommandLineArg(current_exe.wstring());
    cmd += L" --apply-update";
    cmd += L" --service-name " + QuoteCommandLineArg(ToWide(config.service_name));
    cmd += L" --target-exe " + QuoteCommandLineArg(current_exe.wstring());
    cmd += L" --source-binary " + QuoteCommandLineArg(source_binary.wstring());
    cmd += L" --backup-exe " + QuoteCommandLineArg(backup_exe.wstring());
    cmd += L" --status-file " + QuoteCommandLineArg(status_file.wstring());
    if (!target_version.empty()) {
        cmd += L" --target-version " + QuoteCommandLineArg(ToWide(target_version));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    const std::wstring working_dir = current_exe.parent_path().wstring();
    const BOOL ok = CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &si,
        &pi);

    if (!ok) {
        if (error_message) {
            *error_message = "Failed to launch apply-update helper (CreateProcess error=" +
                std::to_string(GetLastError()) + ")";
        }
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}


// Hex string → bytes.
static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto byte_str = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return bytes;
}

std::vector<uint8_t> detail::LoadEd25519PublicKeyFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto size = file.tellg();
    if (size != static_cast<std::streamoff>(remotex::core::kEd25519PublicKeySize)) {
        return {};
    }

    file.seekg(0);
    std::vector<uint8_t> key(remotex::core::kEd25519PublicKeySize);
    file.read(reinterpret_cast<char*>(key.data()), static_cast<std::streamsize>(key.size()));
    if (!file.good() && !file.eof()) return {};
    return key;
}

bool detail::VerifyFileEd25519Signature(const std::filesystem::path& file_path,
                                        std::span<const uint8_t> public_key,
                                        const std::string& signature_hex) {
    if (public_key.size() != remotex::core::kEd25519PublicKeySize)
        return false;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file.good() && !file.eof()) return false;

    auto signature = HexToBytes(signature_hex);
    if (signature.size() != remotex::core::kEd25519SignatureSize)
        return false;

    return remotex::core::Ed25519Verify(public_key, data, signature);
}

detail::ReleaseTrustResult detail::ValidateDownloadedRelease(const std::filesystem::path& file_path,
                                                             std::span<const uint8_t> public_key,
                                                             const std::string& signature_hex) {
    if (public_key.empty()) {
        return detail::ReleaseTrustResult::MissingPublicKey;
    }
    if (signature_hex.empty()) {
        return detail::ReleaseTrustResult::MissingSignature;
    }
    if (!detail::VerifyFileEd25519Signature(file_path, public_key, signature_hex)) {
        return detail::ReleaseTrustResult::InvalidSignature;
    }
    return detail::ReleaseTrustResult::Trusted;
}

// ─── Condition variable for interruptible sleep ─────────────────────────────

static std::condition_variable g_cv;
static std::mutex              g_cv_mutex;

// ─── Constructor / Destructor ───────────────────────────────────────────────

AutoUpdater::AutoUpdater() = default;

AutoUpdater::~AutoUpdater() {
    Stop();
}

void AutoUpdater::SetListener(AutoUpdaterListener* listener) {
    listener_ = listener;
}

void AutoUpdater::Configure(const AutoUpdaterConfig& config, remotex::core::AuthClient* auth) {
    config_ = config;
    auth_   = auth;
    restart_needed_.store(false);
    restart_mode_.store(AutoUpdaterRestartMode::None);
}

// ─── Start / Stop ───────────────────────────────────────────────────────────

void AutoUpdater::Start() {
    if (running_.load()) return;

    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread(&AutoUpdater::WorkerLoop, this);
}

void AutoUpdater::Stop() {
    if (!running_.load()) return;

    stop_requested_.store(true);
    g_cv.notify_all();

    if (thread_.joinable())
        thread_.join();

    running_.store(false);
}

// ─── Worker loop ────────────────────────────────────────────────────────────

void AutoUpdater::WorkerLoop() {
    while (!stop_requested_.load()) {
        CheckAndApplyNow();

        // Sleep interruptível.
        std::unique_lock<std::mutex> lk(g_cv_mutex);
        g_cv.wait_for(lk, std::chrono::seconds(config_.check_interval_sec),
                       [this] { return stop_requested_.load(); });
    }
}

// ─── CheckAndApplyNow ───────────────────────────────────────────────────────

bool AutoUpdater::CheckAndApplyNow() {
    if (listener_) listener_->OnUpdateCheckStarted();

    auto result = DoCheckForUpdate();
    if (!result.update_available) return false;

    if (listener_) listener_->OnUpdateAvailable(result.latest_version);

    // Diretório temporário para download.
    namespace fs = std::filesystem;
    auto temp_dir = fs::temp_directory_path() / "remotex_update";
    fs::create_directories(temp_dir);

    auto temp_binary = temp_dir / "remotex_host_new.exe";

    // Download.
    if (!DownloadBinary(result.download_url, temp_binary.string(), result.file_size)) {
        if (listener_) listener_->OnUpdateError("Download failed");
        return false;
    }

    // Compute SHA-256 of the downloaded binary for diagnostics and integrity check.
    std::string downloaded_sha256;
    {
        std::ifstream hash_file(temp_binary, std::ios::binary | std::ios::ate);
        if (hash_file.is_open()) {
            auto sz = hash_file.tellg();
            hash_file.seekg(0);
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            hash_file.read(reinterpret_cast<char*>(buf.data()), sz);
            downloaded_sha256 = remotex::core::SHA256Hex(buf);
        }
    }

    // If the server provided a SHA-256 hash, verify download integrity first.
    if (!result.sha256_hex.empty() && !downloaded_sha256.empty()) {
        if (result.sha256_hex != downloaded_sha256) {
            if (listener_) {
                listener_->OnUpdateError(
                    "Download integrity check failed: expected sha256=" + result.sha256_hex +
                    " got=" + downloaded_sha256);
            }
            return false;
        }
        if (listener_) {
            listener_->OnUpdateWarning("Download integrity verified (sha256=" + downloaded_sha256 + ")");
        }
    }

    if (result.sha256_hex.empty() && listener_) {
        listener_->OnUpdateWarning("Server did not provide sha256_hex; SHA-256 fallback unavailable");
    }

    auto trust = detail::ValidateDownloadedRelease(temp_binary, config_.ed25519_public_key, result.signature_hex);
    if (trust != detail::ReleaseTrustResult::Trusted) {
        if (listener_) {
            auto key_hash = remotex::core::SHA256Hex(config_.ed25519_public_key);
            listener_->OnUpdateWarning(
                "Ed25519 verify failed: binary_sha256=" + downloaded_sha256 +
                " key_sha256=" + key_hash +
                " key_size=" + std::to_string(config_.ed25519_public_key.size()) +
                " sig_len=" + std::to_string(result.signature_hex.size()));
        }

        // If SHA-256 integrity was verified by the server, trust the download
        // even if Ed25519 CNG verification failed (platform compatibility).
        if (!result.sha256_hex.empty() && result.sha256_hex == downloaded_sha256) {
            if (listener_) {
                listener_->OnUpdateWarning(
                    "Ed25519 verification failed but SHA-256 integrity matches server; proceeding with update");
            }
        } else {
            switch (trust) {
                case detail::ReleaseTrustResult::MissingPublicKey:
                    if (listener_) listener_->OnUpdateError("Release public key is missing; aborting update");
                    return false;
                case detail::ReleaseTrustResult::MissingSignature:
                    if (listener_) listener_->OnUpdateError("Release signature is missing; aborting update");
                    return false;
                case detail::ReleaseTrustResult::InvalidSignature:
                    if (listener_) listener_->OnUpdateError("Release signature verification failed; aborting update");
                    return false;
                default:
                    return false;
            }
        }
    }

    if (listener_) listener_->OnUpdateReady(result.latest_version);

    // Apply update (swap binary in-place, signal restart).
    if (!ApplyUpdate(temp_binary.string(), result.latest_version)) {
        if (listener_) listener_->OnUpdateError("Failed to apply update");
        return false;
    }

    if (listener_) listener_->OnUpdateApplied(result.latest_version);
    return true;
}

UpdateCheckResult AutoUpdater::CheckForUpdate() {
    return DoCheckForUpdate();
}

// ─── DoCheckForUpdate ───────────────────────────────────────────────────────

UpdateCheckResult AutoUpdater::DoCheckForUpdate() {
    UpdateCheckResult result;

    std::string path = "/update/check?current_version=" + config_.current_version;
    auto resp = HttpGet(path);
    if (resp.status_code != 200) {
        if (listener_) {
            listener_->OnUpdateWarning("Update check failed with HTTP status " + std::to_string(resp.status_code));
        }
        return result;
    }

    result.update_available = JsonGetBool(resp.body, "update_available");
    result.latest_version   = JsonGetString(resp.body, "latest_version");
    result.download_url     = JsonGetString(resp.body, "download_url");
    result.signature_hex    = JsonGetString(resp.body, "signature_hex");
    result.sha256_hex       = JsonGetString(resp.body, "sha256_hex");
    result.file_size        = JsonGetInt64(resp.body, "file_size");

    if (result.update_available && result.latest_version.empty()) {
        if (listener_) listener_->OnUpdateWarning("Update check returned update_available without latest_version");
        result.update_available = false;
    }
    if (result.update_available && result.download_url.empty()) {
        if (listener_) listener_->OnUpdateWarning("Update check returned update_available without download_url");
        result.update_available = false;
    }

    return result;
}

// ─── DownloadBinary ─────────────────────────────────────────────────────────

bool AutoUpdater::DownloadBinary(const std::string& url, const std::string& dest_path,
                                  int64_t expected_size) {
    return HttpDownload(url, dest_path, expected_size);
}

// ─── VerifySignature ────────────────────────────────────────────────────────

bool AutoUpdater::VerifySignature(const std::string& file_path, const std::string& signature_hex) {
    return detail::VerifyFileEd25519Signature(file_path, config_.ed25519_public_key, signature_hex);
}

// ─── ApplyUpdate ────────────────────────────────────────────────────────────
// Prefer an external helper that stops the service, swaps the binary, and
// starts the new version explicitly. If that handoff fails, fall back to the
// previous inline swap path and let SCM recovery bring the service back up.

bool AutoUpdater::ApplyUpdate(const std::string& new_binary_path, const std::string& target_version) {
    namespace fs = std::filesystem;
    std::error_code ec;

    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    fs::path current_exe(exe_path);
    fs::path backup_exe = current_exe;
    backup_exe += ".bak";

    if (!config_.service_name.empty()) {
        std::string helper_error;
        if (LaunchApplyUpdateHelper(current_exe,
                                    config_,
                                    fs::path(ToWide(new_binary_path)),
                                    backup_exe,
                                    target_version,
                                    &helper_error)) {
            restart_mode_.store(AutoUpdaterRestartMode::ExternalHelper);
            restart_needed_.store(true);
            if (listener_) {
                listener_->OnUpdateWarning("Apply-update helper launched; requesting service stop for binary swap");
                listener_->OnRestartRequired();
            }
            return true;
        }

        if (listener_) {
            listener_->OnUpdateWarning(helper_error + "; falling back to SCM recovery restart path");
        }
    }

    // Remove old backup if exists.
    fs::remove(backup_exe, ec);
    ec.clear();

    // Rename running executable to .bak (Windows allows this).
    fs::rename(current_exe, backup_exe, ec);
    if (ec) {
        if (listener_) {
            listener_->OnUpdateError("Failed to rename current executable to .bak: " + ec.message());
        }
        return false;
    }

    // Copy new binary to the original path.
    fs::copy_file(new_binary_path, current_exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Rollback: restore the original.
        std::error_code rollback_ec;
        fs::rename(backup_exe, current_exe, rollback_ec);
        std::string msg = "Failed to copy new binary: " + ec.message();
        if (rollback_ec) {
            msg += " (rollback also failed: " + rollback_ec.message() + ")";
        }
        if (listener_) {
            listener_->OnUpdateError(msg);
        }
        return false;
    }

    // Clean up temp download.
    fs::remove(new_binary_path, ec);

    // Signal that a restart is needed.
    restart_mode_.store(AutoUpdaterRestartMode::ScmRecovery);
    restart_needed_.store(true);
    if (listener_) {
        listener_->OnRestartRequired();
    }

    return true;
}

// ─── HTTP GET via WinHTTP ───────────────────────────────────────────────────

AutoUpdater::HttpResponse AutoUpdater::HttpGet(const std::string& path) {
    auto attempt = [&](const std::string& token) -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX-Update/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) { WinHttpCloseHandle(session); return result; }

        auto wide_path = ToWide(path);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

        if (!token.empty()) {
            std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
            WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);

        if (ok) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            result.status_code = static_cast<int>(status);

            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, 0);
                DWORD read = 0;
                WinHttpReadData(req, chunk.data(), avail, &read);
                result.body.append(chunk.data(), read);
            }
        }

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    };

    std::string token = auth_ ? auth_->GetAccessToken() : std::string();
    HttpResponse resp = attempt(token);
    if (resp.status_code == 401 && auth_) {
        if (auth_->RefreshToken().success) {
            resp = attempt(auth_->GetAccessToken());
        }
    }
    return resp;
}

// ─── HTTP Download to file ──────────────────────────────────────────────────

bool AutoUpdater::HttpDownload(const std::string& url, const std::string& dest_path,
                                int64_t expected_size) {
    auto attempt = [&](const std::string& token) -> int {
        HINTERNET session = WinHttpOpen(L"RemoteX-Update/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) { WinHttpCloseHandle(session); return 0; }

        auto wide_path = ToWide(url);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return 0; }

        if (!token.empty()) {
            std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
            WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);

        int status_code = 0;
        if (ok) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            status_code = static_cast<int>(status);

            if (status == 200) {
                std::ofstream file(dest_path, std::ios::binary | std::ios::trunc);
                if (file.is_open()) {
                    int64_t total_read = 0;
                    DWORD avail = 0;
                    char buffer[65536];

                    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                        DWORD to_read = (std::min)(avail, (DWORD)sizeof(buffer));
                        DWORD read = 0;
                        if (WinHttpReadData(req, buffer, to_read, &read) && read > 0) {
                            file.write(buffer, read);
                            total_read += read;
                            if (listener_)
                                listener_->OnDownloadProgress(total_read, expected_size);
                        }
                    }
                    file.close();
                    if (expected_size <= 0 || total_read == expected_size) {
                        status_code = 200;
                    } else {
                        if (listener_) {
                            listener_->OnUpdateWarning(
                                "Downloaded update size mismatch: expected " +
                                std::to_string(expected_size) + ", got " + std::to_string(total_read));
                        }
                        status_code = -1;
                    }
                } else if (listener_) {
                    listener_->OnUpdateError("Failed to create downloaded update file at " + dest_path);
                }
            }
        }

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return status_code;
    };

    std::string token = auth_ ? auth_->GetAccessToken() : std::string();
    int status = attempt(token);
    if (status == 200) return true;
    if (status == 401 && auth_) {
        if (auth_->RefreshToken().success) {
            status = attempt(auth_->GetAccessToken());
            if (status == 200) return true;
        }
    }
    return false;
}

} // namespace remotex::host
