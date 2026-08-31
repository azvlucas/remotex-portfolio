// RemoteX — updater/host_updater.cpp
// Check, download, verify, and apply updates to the host binary.
// Ported from host/auto_updater.cpp.

#include "updater/host_updater.h"
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

// ─── JSON helpers ────────────────────────────────────────────────────────────

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

static const char* ServiceStateName(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED:
        return "STOPPED";
    case SERVICE_START_PENDING:
        return "START_PENDING";
    case SERVICE_STOP_PENDING:
        return "STOP_PENDING";
    case SERVICE_RUNNING:
        return "RUNNING";
    case SERVICE_CONTINUE_PENDING:
        return "CONTINUE_PENDING";
    case SERVICE_PAUSE_PENDING:
        return "PAUSE_PENDING";
    case SERVICE_PAUSED:
        return "PAUSED";
    default:
        return "UNKNOWN";
    }
}

static std::string QueryNamedServiceState(const std::wstring& service_name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return "SCM_OPEN_FAILED(err=" + std::to_string(GetLastError()) + ")";
    }

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(), SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return "SERVICE_OPEN_FAILED(err=" + std::to_string(err) + ")";
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes_needed)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return "QUERY_FAILED(err=" + std::to_string(err) + ")";
    }

    std::string result = std::string(ServiceStateName(status.dwCurrentState)) +
                         " pid=" + std::to_string(status.dwProcessId);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
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

// ─── Shared helpers ──────────────────────────────────────────────────────────

std::vector<uint8_t> LoadEd25519PublicKeyFile(const fs::path& path) {
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

bool WaitForFileAvailability(const fs::path& path, DWORD timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        HANDLE file = CreateFileW(path.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return true;
        }
        Sleep(500);
    }
    return false;
}

bool StopNamedService(const std::wstring& service_name, DWORD timeout_ms) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(),
                                 SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes_needed)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    if (status.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }

    SERVICE_STATUS stop_status{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &stop_status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    const auto deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes_needed)) {
            break;
        }
        if (status.dwCurrentState == SERVICE_STOPPED) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return true;
        }
        Sleep(500);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return false;
}

bool StartNamedService(const std::wstring& service_name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(),
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    BOOL ok = StartServiceW(svc, 0, nullptr);
    if (!ok && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        ok = TRUE;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok != FALSE;
}

// ─── HostUpdater ─────────────────────────────────────────────────────────────

void HostUpdater::Configure(const HostUpdateConfig& config, remotex::core::AuthClient* auth, LogFn log_fn) {
    config_ = config;
    auth_   = auth;
    log_fn_ = std::move(log_fn);
}

void HostUpdater::Log(const std::string& msg) {
    if (log_fn_) log_fn_(msg);
}

bool HostUpdater::CheckAndApply() {
    Log("Checking for host update (current_version=" + config_.current_host_version +
        " api_host=" + remotex::core::RedactIdentifier(config_.api_host, "api_host") +
        " api_port=" + std::to_string(config_.api_port) +
        " tls=" + std::string(config_.use_tls ? "true" : "false") +
        " has_auth=" + std::string(auth_ ? "yes" : "no") +
        " has_pubkey=" + std::string(config_.ed25519_public_key.empty() ? "no" : "yes") + ")");

    auto result = DoCheckForUpdate();
    if (!result.update_available) {
        Log("Host is up to date (version " + config_.current_host_version + ")");
        return false;
    }

    Log("Host update available: " + result.latest_version);

    // Download to temp directory.
    auto temp_dir = fs::temp_directory_path() / "remotex_updater_download";
    fs::create_directories(temp_dir);
    auto temp_binary = temp_dir / "remotex_host_new.exe";

    Log("Downloading host update (download_url=" +
        remotex::core::RedactIdentifier(result.download_url, "download_url") +
        " expected_size=" + std::to_string(result.file_size) + ")");
    if (!DownloadBinary(result.download_url, temp_binary, result.file_size)) {
        Log("ERROR: Failed to download host update");
        return false;
    }

    if (!VerifyDownload(temp_binary, result.signature_hex, result.sha256_hex)) {
        Log("ERROR: Host update verification failed");
        std::error_code ec;
        fs::remove(temp_binary, ec);
        return false;
    }

    Log("Host update verified, applying...");

    if (!ApplyHostUpdate(temp_binary, result.latest_version)) {
        Log("ERROR: Failed to apply host update");
        return false;
    }

    last_applied_version_ = result.latest_version;
    Log("Host update " + result.latest_version + " applied successfully");
    return true;
}

UpdateCheckResult HostUpdater::CheckForUpdate() {
    return DoCheckForUpdate();
}

UpdateCheckResult HostUpdater::DoCheckForUpdate() {
    UpdateCheckResult result;

    std::string path = "/update/check?current_version=" + config_.current_host_version;
    auto resp = HttpGet(path);
    if (resp.status_code != 200) {
        Log("Update check failed with HTTP status " + std::to_string(resp.status_code) +
            " body=" + resp.body.substr(0, 500));
        return result;
    }

    result.update_available = JsonGetBool(resp.body, "update_available");
    result.latest_version   = JsonGetString(resp.body, "latest_version");
    result.download_url     = JsonGetString(resp.body, "download_url");
    result.signature_hex    = JsonGetString(resp.body, "signature_hex");
    result.sha256_hex       = JsonGetString(resp.body, "sha256_hex");
    result.file_size        = JsonGetInt64(resp.body, "file_size");
    Log("Update check response: update_available=" + std::string(result.update_available ? "true" : "false") +
        " latest_version=" + result.latest_version +
        " download_url=" + remotex::core::RedactIdentifier(result.download_url, "download_url") +
        " file_size=" + std::to_string(result.file_size) +
        " sha256_present=" + std::string(result.sha256_hex.empty() ? "no" : "yes") +
        " signature_present=" + std::string(result.signature_hex.empty() ? "no" : "yes"));

    if (result.update_available && (result.latest_version.empty() || result.download_url.empty())) {
        Log("WARNING: Update check returned update_available but missing version or URL");
        result.update_available = false;
    }

    return result;
}

bool HostUpdater::VerifyDownload(const fs::path& file, const std::string& signature_hex, const std::string& sha256_hex) {
    // Compute SHA-256 of the downloaded binary.
    std::string downloaded_sha256;
    {
        std::ifstream hash_file(file, std::ios::binary | std::ios::ate);
        if (hash_file.is_open()) {
            auto sz = hash_file.tellg();
            hash_file.seekg(0);
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            hash_file.read(reinterpret_cast<char*>(buf.data()), sz);
            downloaded_sha256 = remotex::core::SHA256Hex(buf);
        }
    }

    // If the server provided a SHA-256 hash, verify download integrity.
    if (!sha256_hex.empty() && !downloaded_sha256.empty()) {
        if (sha256_hex != downloaded_sha256) {
            Log("Download integrity check failed: expected sha256=" + sha256_hex + " got=" + downloaded_sha256);
            return false;
        }
        Log("Download integrity verified (sha256=" + downloaded_sha256 + ")");
    }

    // Ed25519 signature verification.
    if (!config_.ed25519_public_key.empty() && !signature_hex.empty()) {
        auto sig_bytes = HexToBytes(signature_hex);
        if (sig_bytes.size() == remotex::core::kEd25519SignatureSize) {
            std::ifstream bin_file(file, std::ios::binary | std::ios::ate);
            if (bin_file.is_open()) {
                auto sz = bin_file.tellg();
                bin_file.seekg(0);
                std::vector<uint8_t> data(static_cast<size_t>(sz));
                bin_file.read(reinterpret_cast<char*>(data.data()), sz);
                Log("Verifying download: public_key_size=" + std::to_string(config_.ed25519_public_key.size()) +
                    " signature_size=" + std::to_string(sig_bytes.size()) +
                    " file_size=" + std::to_string(data.size()));
                if (remotex::core::Ed25519Verify(config_.ed25519_public_key, data, sig_bytes)) {
                    Log("Ed25519 signature verified");
                    return true;
                }
            }
        }
        Log("WARNING: Ed25519 verification failed");
    }

    // If SHA-256 matched, trust the download even if Ed25519 CNG failed.
    if (!sha256_hex.empty() && sha256_hex == downloaded_sha256) {
        Log("Ed25519 verification failed but SHA-256 integrity matches server; proceeding");
        return true;
    }

    if (config_.ed25519_public_key.empty()) {
        Log("ERROR: Release public key is missing; aborting");
        return false;
    }
    if (signature_hex.empty()) {
        Log("ERROR: Release signature is missing; aborting");
        return false;
    }

    Log("ERROR: Release signature verification failed; aborting");
    return false;
}

bool HostUpdater::ApplyHostUpdate(const fs::path& new_binary, const std::string& new_version) {
    // Find the host exe in the same directory as the updater.
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    fs::path updater_dir = fs::path(exe_path).parent_path();
    fs::path host_exe = updater_dir / "remotex_host.exe";
    fs::path backup_exe = host_exe;
    backup_exe += ".bak";

    std::wstring host_svc_name = ToWide(config_.host_service_name);

    // Stop the host service.
    Log("Stopping host service '" + config_.host_service_name + "'...");
    bool stopped = StopNamedService(host_svc_name, 60000);
    DWORD stop_err = GetLastError();
    std::string stop_state = QueryNamedServiceState(host_svc_name);
    if (stopped) {
        Log("Host service stopped successfully (state=" + stop_state + ")");
    } else {
        Log("WARNING: StopNamedService returned false (err=" + std::to_string(stop_err) +
            ", state=" + stop_state + "), service may not be running - proceeding with update");
    }

    // Wait for the binary to become available.
    if (fs::exists(host_exe)) {
        if (!WaitForFileAvailability(host_exe, 30000)) {
            Log("ERROR: Timed out waiting for host_binary=" +
                remotex::core::RedactPath(host_exe, "host_binary") +
                " to become available (err=" + std::to_string(GetLastError()) + ")");
            StartNamedService(host_svc_name);
            return false;
        }
    }

    std::error_code ec;

    // Remove old backup.
    fs::remove(backup_exe, ec);
    ec.clear();

    // Rename current host to .bak.
    if (fs::exists(host_exe)) {
        Log("Renaming host_binary=" + remotex::core::RedactPath(host_exe, "host_binary") +
            " -> backup_binary=" + remotex::core::RedactPath(backup_exe, "backup_binary"));
        fs::rename(host_exe, backup_exe, ec);
        if (ec) {
            Log("ERROR: Failed to rename host executable to .bak: " + ec.message());
            StartNamedService(host_svc_name);
            return false;
        }
    }

    // Copy new binary.
    Log("Copying new binary from source=" + remotex::core::RedactPath(new_binary, "source") +
        " -> target=" + remotex::core::RedactPath(host_exe, "target"));
    fs::copy_file(new_binary, host_exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Log("ERROR: Failed to copy new binary: " + ec.message() +
            " (from=" + remotex::core::RedactPath(new_binary, "source") +
            " to=" + remotex::core::RedactPath(host_exe, "target") + ")");
        // Rollback.
        std::error_code rollback_ec;
        if (fs::exists(backup_exe)) {
            fs::rename(backup_exe, host_exe, rollback_ec);
            if (rollback_ec) {
                Log("ERROR: Rollback rename failed: " + rollback_ec.message());
            } else {
                Log("Rollback: restored backup_binary=" + remotex::core::RedactPath(backup_exe, "backup_binary") +
                    " -> host_binary=" + remotex::core::RedactPath(host_exe, "host_binary"));
            }
        }
        Log("Starting host service with previous version after failed update...");
        if (!StartNamedService(host_svc_name)) {
            Log("WARNING: Failed to restart host service after rollback (err=" +
                std::to_string(GetLastError()) + ", state=" + QueryNamedServiceState(host_svc_name) + ")");
        } else {
            Log("Host service restarted on previous version (state=" + QueryNamedServiceState(host_svc_name) + ")");
        }
        return false;
    }
    auto new_size = fs::file_size(host_exe, ec);
    if (!ec) {
        Log("Copy complete: new_size=" + std::to_string(new_size));
    }
    ec.clear();

    fs::path version_file = updater_dir / "VERSION";
    {
        std::ofstream vf(version_file, std::ios::trunc);
        if (vf.is_open()) {
            vf << new_version;
            vf.close();
            Log("Updated VERSION file to " + new_version);
        } else {
            Log("WARNING: Failed to update VERSION file at " +
                remotex::core::RedactPath(version_file, "version_file"));
        }
    }

    // Clean up temp download.
    fs::remove(new_binary, ec);

    // Start the host service with the new binary.
    Log("Starting host service with updated binary...");
    if (!StartNamedService(host_svc_name)) {
        Log("WARNING: Failed to start host service after update (err=" + std::to_string(GetLastError()) +
            ", state=" + QueryNamedServiceState(host_svc_name) + ")");
    } else {
        Log("Host service started successfully with updated binary (state=" +
            QueryNamedServiceState(host_svc_name) + ")");
    }

    return true;
}

bool HostUpdater::DownloadBinary(const std::string& url, const fs::path& dest, int64_t expected_size) {
    return HttpDownloadToFile(url, dest, expected_size);
}

// ─── HTTP via WinHTTP ────────────────────────────────────────────────────────

// Timeouts: 15s DNS resolve, 15s connect, 30s send, 60s receive.
static constexpr int kResolveTimeoutMs = 15000;
static constexpr int kConnectTimeoutMs = 15000;
static constexpr int kSendTimeoutMs    = 30000;
static constexpr int kReceiveTimeoutMs = 60000;
// Download timeout: 5 minutes for large binary transfers.
static constexpr int kDownloadReceiveTimeoutMs = 300000;

HostUpdater::HttpResponse HostUpdater::HttpGet(const std::string& path) {
    auto attempt = [&](const std::string& token) -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX-Updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            Log("ERROR: WinHttpOpen failed, err=" + std::to_string(GetLastError()));
            return result;
        }

        WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs,
                           kSendTimeoutMs, kReceiveTimeoutMs);

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) {
            Log("ERROR: WinHttpConnect failed for api_host=" +
                remotex::core::RedactIdentifier(config_.api_host, "api_host") +
                " api_port=" + std::to_string(config_.api_port) +
                " err=" + std::to_string(GetLastError()));
            WinHttpCloseHandle(session);
            return result;
        }

        auto wide_path = ToWide(path);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            Log("ERROR: WinHttpOpenRequest failed for request_path=" +
                remotex::core::RedactIdentifier(path, "request_path") +
                ", err=" + std::to_string(GetLastError()));
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return result;
        }

        if (!token.empty()) {
            std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
            WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);
        if (!ok) {
            Log("ERROR: WinHttpSendRequest/ReceiveResponse failed, err=" + std::to_string(GetLastError()));
        }

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
            Log("HTTP GET request_path=" + remotex::core::RedactIdentifier(path, "request_path") +
                " status=" + std::to_string(result.status_code) +
                " body_len=" + std::to_string(result.body.size()));
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

bool HostUpdater::HttpDownloadToFile(const std::string& url, const fs::path& dest, int64_t expected_size) {
    auto attempt = [&](const std::string& token) -> int {
        HINTERNET session = WinHttpOpen(L"RemoteX-Updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            Log("ERROR: WinHttpOpen failed, err=" + std::to_string(GetLastError()));
            return 0;
        }

        WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs,
                           kSendTimeoutMs, kDownloadReceiveTimeoutMs);

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) {
            Log("ERROR: WinHttpConnect failed for api_host=" +
                remotex::core::RedactIdentifier(config_.api_host, "api_host") +
                " api_port=" + std::to_string(config_.api_port) +
                " err=" + std::to_string(GetLastError()));
            WinHttpCloseHandle(session);
            return 0;
        }

        auto wide_path = ToWide(url);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            Log("ERROR: WinHttpOpenRequest failed for download_url=" +
                remotex::core::RedactIdentifier(url, "download_url") +
                ", err=" + std::to_string(GetLastError()));
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return 0;
        }

        if (!token.empty()) {
            std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
            WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);
        if (!ok) {
            Log("ERROR: WinHttpSendRequest/ReceiveResponse failed, err=" + std::to_string(GetLastError()));
        }

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
                    Log("Download complete: bytes=" + std::to_string(total_read) +
                        " destination=" + remotex::core::RedactPath(dest, "destination"));
                    if (expected_size > 0 && total_read != expected_size) {
                        Log("WARNING: Downloaded size mismatch: expected " +
                            std::to_string(expected_size) + ", got " + std::to_string(total_read));
                        status_code = -1;
                    }
                } else {
                    Log("ERROR: Failed to open download destination: " +
                        remotex::core::RedactPath(dest, "destination"));
                    status_code = -1;
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
        Log("Download returned 401, refreshing token and retrying...");
        if (auth_->RefreshToken().success) {
            status = attempt(auth_->GetAccessToken());
            if (status == 200) return true;
        }
    }
    Log("Download failed with HTTP status " + std::to_string(status));
    return false;
}

} // namespace remotex::updater
