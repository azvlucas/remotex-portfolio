// RemoteX — updater/self_updater.cpp
// Self-update: download new updater binary, rename self to .bak, copy new, exit 100.

#include "updater/self_updater.h"
#include "updater/host_updater.h"  // For shared helpers.
#include "core/auth_client.h"
#include "core/crypto_utils.h"
#include "core/log_redaction.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace remotex::updater {

namespace fs = std::filesystem;

// ─── JSON helpers (same as host_updater) ─────────────────────────────────────

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

static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto byte_str = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return bytes;
}

// ─── SelfUpdater ─────────────────────────────────────────────────────────────

void SelfUpdater::Configure(const SelfUpdateConfig& config, remotex::core::AuthClient* auth, LogFn log_fn) {
    config_ = config;
    auth_   = auth;
    log_fn_ = std::move(log_fn);
}

void SelfUpdater::Log(const std::string& msg) {
    if (log_fn_) log_fn_(msg);
}

void SelfUpdater::CleanupPreviousBackup() {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    fs::path backup = fs::path(exe_path);
    backup += ".bak";

    std::error_code ec;
    if (fs::exists(backup)) {
        fs::remove(backup, ec);
        if (!ec) {
            Log("Cleaned up previous updater backup");
        }
    }
}

bool SelfUpdater::CheckAndApply() {
    Log("Checking for updater self-update...");

    std::string path = "/update/updater/check?current_version=" + config_.current_version;
    auto resp = HttpGet(path);
    if (resp.status_code != 200) {
        Log("Updater self-update check failed with HTTP status " + std::to_string(resp.status_code));
        return false;
    }

    bool update_available   = JsonGetBool(resp.body, "update_available");
    std::string new_version = JsonGetString(resp.body, "latest_version");
    std::string download_url = JsonGetString(resp.body, "download_url");
    std::string signature_hex = JsonGetString(resp.body, "signature_hex");
    std::string sha256_hex    = JsonGetString(resp.body, "sha256_hex");
    int64_t file_size         = JsonGetInt64(resp.body, "file_size");

    if (!update_available) {
        Log("Updater is up to date (version " + config_.current_version + ")");
        return false;
    }

    if (new_version.empty() || download_url.empty()) {
        Log("WARNING: Updater self-update returned incomplete data");
        return false;
    }

    Log("Updater self-update available: " + new_version);

    // Client-side guard: reject downgrades.
    if (!new_version.empty() && !config_.current_version.empty()) {
        auto parse = [](const std::string& v) -> std::vector<int> {
            std::vector<int> parts;
            std::string seg;
            for (size_t i = 0; i <= v.size(); ++i) {
                if (i == v.size() || v[i] == '.') {
                    if (!seg.empty()) {
                        try { parts.push_back(std::stoi(seg)); } catch (...) { return {}; }
                    }
                    seg.clear();
                } else {
                    seg += v[i];
                }
            }
            return parts;
        };
        auto cur = parse(config_.current_version);
        auto lat = parse(new_version);
        if (!cur.empty() && !lat.empty()) {
            bool is_newer = false;
            size_t max_len = (std::max)(cur.size(), lat.size());
            for (size_t i = 0; i < max_len; ++i) {
                int c = (i < cur.size()) ? cur[i] : 0;
                int l = (i < lat.size()) ? lat[i] : 0;
                if (l > c) { is_newer = true; break; }
                if (l < c) { break; }
            }
            if (!is_newer) {
                Log("Updater is up to date (version " + config_.current_version +
                    " >= server " + new_version + ")");
                return false;
            }
        }
    }

    // Download to temp.
    auto temp_dir = fs::temp_directory_path() / "remotex_updater_selfupdate";
    fs::create_directories(temp_dir);
    auto temp_binary = temp_dir / "remotex_updater_new.exe";

    Log("Downloading self-update (download_url=" +
        remotex::core::RedactIdentifier(download_url, "download_url") +
        " expected_size=" + std::to_string(file_size) + ")");
    if (!HttpDownloadToFile(download_url, temp_binary, file_size)) {
        Log("ERROR: Failed to download updater self-update");
        return false;
    }

    // Verify SHA-256.
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

    if (!sha256_hex.empty() && !downloaded_sha256.empty() && sha256_hex != downloaded_sha256) {
        Log("ERROR: Updater self-update SHA-256 mismatch");
        std::error_code ec;
        fs::remove(temp_binary, ec);
        return false;
    }

    // Ed25519 verification.
    bool sig_ok = false;
    if (!config_.ed25519_public_key.empty() && !signature_hex.empty()) {
        auto sig_bytes = HexToBytes(signature_hex);
        if (sig_bytes.size() == remotex::core::kEd25519SignatureSize) {
            std::ifstream bin_file(temp_binary, std::ios::binary | std::ios::ate);
            if (bin_file.is_open()) {
                auto sz = bin_file.tellg();
                bin_file.seekg(0);
                std::vector<uint8_t> data(static_cast<size_t>(sz));
                bin_file.read(reinterpret_cast<char*>(data.data()), sz);
                sig_ok = remotex::core::Ed25519Verify(config_.ed25519_public_key, data, sig_bytes);
            }
        }
    }

    if (!sig_ok) {
        if (!sha256_hex.empty() && sha256_hex == downloaded_sha256) {
            Log("WARNING: Ed25519 failed but SHA-256 matches; proceeding");
        } else {
            Log("ERROR: Updater self-update verification failed");
            std::error_code ec;
            fs::remove(temp_binary, ec);
            return false;
        }
    }

    // Self-update: rename current exe to .bak, copy new binary.
    wchar_t exe_path[MAX_PATH];
    DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        Log("ERROR: Could not determine updater executable path");
        return false;
    }

    fs::path current_exe(exe_path);
    fs::path backup_exe = current_exe;
    backup_exe += ".bak";

    std::error_code ec;

    // Remove old backup.
    fs::remove(backup_exe, ec);
    ec.clear();

    // Rename self to .bak (Windows allows renaming a running executable).
    fs::rename(current_exe, backup_exe, ec);
    if (ec) {
        Log("ERROR: Failed to rename updater to .bak: " + ec.message());
        return false;
    }

    // Copy new binary to the original path.
    fs::copy_file(temp_binary, current_exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Rollback.
        std::error_code rollback_ec;
        fs::rename(backup_exe, current_exe, rollback_ec);
        Log("ERROR: Failed to copy new updater binary: " + ec.message());
        return false;
    }

    // Clean up temp.
    fs::remove(temp_binary, ec);

    Log("Updater self-update " + new_version + " applied. Service will restart via SCM recovery (exit 100).");
    return true;
}

// ─── HTTP via WinHTTP ────────────────────────────────────────────────────────

// Timeouts: 15s DNS resolve, 15s connect, 30s send, 60s receive.
static constexpr int kResolveTimeoutMs = 15000;
static constexpr int kConnectTimeoutMs = 15000;
static constexpr int kSendTimeoutMs    = 30000;
static constexpr int kReceiveTimeoutMs = 60000;
// Download timeout: 5 minutes for large binary transfers.
static constexpr int kDownloadReceiveTimeoutMs = 300000;

SelfUpdater::HttpResponse SelfUpdater::HttpGet(const std::string& path) {
    auto attempt = [&](const std::string& token) -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX-Updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;

        WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs,
                           kSendTimeoutMs, kReceiveTimeoutMs);

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

bool SelfUpdater::HttpDownloadToFile(const std::string& url, const fs::path& dest, int64_t expected_size) {
    auto attempt = [&](const std::string& token) -> int {
        HINTERNET session = WinHttpOpen(L"RemoteX-Updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;

        WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs,
                           kSendTimeoutMs, kDownloadReceiveTimeoutMs);

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
                std::ofstream file(dest, std::ios::binary | std::ios::trunc);
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
                        }
                    }
                    file.close();
                    if (expected_size > 0 && total_read != expected_size) {
                        status_code = -1;
                    }
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

} // namespace remotex::updater
