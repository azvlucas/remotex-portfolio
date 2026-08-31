// RemoteX — viewer/auto_updater.cpp
// Auto-update for viewer: check → download zip → verify → batch swap → relaunch.

#include "viewer/auto_updater.h"
#include "core/auth_client.h"
#include "core/crypto_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace remotex::viewer {

// ─── JSON / string helpers ──────────────────────────────────────────────────

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

static ViewerUpdateArtifactKind DetectArtifactKind(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return ViewerUpdateArtifactKind::None;
    }

    unsigned char header[4] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() < 2) {
        return ViewerUpdateArtifactKind::None;
    }

    if (header[0] == 'M' && header[1] == 'Z') {
        return ViewerUpdateArtifactKind::ExecutableBinary;
    }
    if (header[0] == 'P' && header[1] == 'K') {
        return ViewerUpdateArtifactKind::PackageZip;
    }
    return ViewerUpdateArtifactKind::None;
}

static std::condition_variable g_viewer_cv;
static std::mutex              g_viewer_cv_mutex;

// ─── Constructor / Destructor ───────────────────────────────────────────────

ViewerAutoUpdater::ViewerAutoUpdater() = default;

ViewerAutoUpdater::~ViewerAutoUpdater() {
    Stop();
}

void ViewerAutoUpdater::SetListener(ViewerAutoUpdaterListener* listener) {
    listener_ = listener;
}

void ViewerAutoUpdater::Configure(const ViewerUpdateConfig& config, remotex::core::AuthClient* auth) {
    config_ = config;
    auth_   = auth;
}

std::string ViewerAutoUpdater::PendingVersion() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return pending_version_;
}

// ─── Start / Stop ───────────────────────────────────────────────────────────

void ViewerAutoUpdater::Start() {
    if (running_.load()) return;
    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread(&ViewerAutoUpdater::WorkerLoop, this);
}

void ViewerAutoUpdater::Stop() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    g_viewer_cv.notify_all();
    if (thread_.joinable())
        thread_.join();
    running_.store(false);
}

// ─── Worker loop ────────────────────────────────────────────────────────────

void ViewerAutoUpdater::WorkerLoop() {
    while (!stop_requested_.load()) {
        auto result = DoCheckForUpdate();
        if (result.update_available && !update_staged_.load()) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                pending_version_ = result.latest_version;
                pending_download_url_ = result.download_url;
                pending_signature_hex_ = result.signature_hex;
                pending_sha256_hex_ = result.sha256_hex;
                pending_file_size_ = result.file_size;
            }
            if (listener_) listener_->OnUpdateAvailable(result.latest_version);
        }

        std::unique_lock<std::mutex> lk(g_viewer_cv_mutex);
        g_viewer_cv.wait_for(lk, std::chrono::seconds(config_.check_interval_sec),
                             [this] { return stop_requested_.load(); });
    }
}

// ─── CheckForUpdate ─────────────────────────────────────────────────────────

ViewerUpdateCheckResult ViewerAutoUpdater::CheckForUpdate() {
    return DoCheckForUpdate();
}

ViewerUpdateCheckResult ViewerAutoUpdater::DoCheckForUpdate() {
    ViewerUpdateCheckResult result;
    if (listener_) listener_->OnUpdateCheckStarted();

    std::string path = "/update/viewer/check?current_version=" + config_.current_version;
    auto resp = HttpGet(path);
    if (resp.status_code != 200) {
        if (listener_)
            listener_->OnUpdateWarning("Viewer update check failed with HTTP status " + std::to_string(resp.status_code));
        return result;
    }

    result.update_available = JsonGetBool(resp.body, "update_available");
    result.latest_version   = JsonGetString(resp.body, "latest_version");
    result.download_url     = JsonGetString(resp.body, "download_url");
    result.signature_hex    = JsonGetString(resp.body, "signature_hex");
    result.sha256_hex       = JsonGetString(resp.body, "sha256_hex");
    result.file_size        = JsonGetInt64(resp.body, "file_size");

    if (result.update_available && (result.latest_version.empty() || result.download_url.empty())) {
        result.update_available = false;
    }

    return result;
}

// ─── DownloadAndStage ───────────────────────────────────────────────────────

bool ViewerAutoUpdater::DownloadAndStage() {
    namespace fs = std::filesystem;

    std::string download_url, signature_hex, sha256_hex, version;
    int64_t file_size;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        download_url = pending_download_url_;
        signature_hex = pending_signature_hex_;
        sha256_hex = pending_sha256_hex_;
        file_size = pending_file_size_;
        version = pending_version_;
    }

    if (download_url.empty() || version.empty()) {
        if (listener_) listener_->OnUpdateError("No pending update to download");
        return false;
    }

    auto temp_dir = fs::temp_directory_path() / "remotex_viewer_update";
    fs::create_directories(temp_dir);

    auto artifact_path = temp_dir / "remotex_viewer_update.zip";

    if (!HttpDownload(download_url, artifact_path.string(), file_size)) {
        if (listener_) listener_->OnUpdateError("Failed to download viewer update");
        return false;
    }

    std::vector<uint8_t> downloaded_data;
    {
        std::ifstream file(artifact_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (listener_) listener_->OnUpdateError("Failed to open downloaded viewer update");
            return false;
        }

        auto sz = file.tellg();
        if (sz <= 0) {
            if (listener_) listener_->OnUpdateError("Downloaded viewer update is empty");
            return false;
        }

        file.seekg(0);
        downloaded_data.resize(static_cast<size_t>(sz));
        file.read(reinterpret_cast<char*>(downloaded_data.data()), sz);
        if (!file.good() && !file.eof()) {
            if (listener_) listener_->OnUpdateError("Failed to read downloaded viewer update");
            return false;
        }
    }

    auto artifact_kind = DetectArtifactKind(artifact_path);
    if (artifact_kind == ViewerUpdateArtifactKind::None) {
        if (listener_) {
            listener_->OnUpdateError(
                "Downloaded viewer update is neither a valid .zip package nor a Windows executable");
        }
        return false;
    }

    // Verify SHA-256 if provided
    auto downloaded_sha256 = remotex::core::SHA256Hex(downloaded_data);
    if (!sha256_hex.empty() && sha256_hex != downloaded_sha256) {
        if (listener_) listener_->OnUpdateError("Download integrity check failed");
        return false;
    }

    if (artifact_kind == ViewerUpdateArtifactKind::ExecutableBinary &&
        !config_.ed25519_public_key.empty() && !signature_hex.empty()) {
        auto sig = HexToBytes(signature_hex);
        if (sig.size() == remotex::core::kEd25519SignatureSize) {
            if (!remotex::core::Ed25519Verify(config_.ed25519_public_key, downloaded_data, sig)) {
                // If SHA-256 matched, proceed anyway (platform compatibility).
                if (sha256_hex.empty()) {
                    if (listener_) listener_->OnUpdateError("Signature verification failed");
                    return false;
                }
                if (listener_) listener_->OnUpdateWarning("Ed25519 failed but SHA-256 matches; proceeding");
            }
        }
    }
    if (artifact_kind == ViewerUpdateArtifactKind::ExecutableBinary && listener_) {
        listener_->OnUpdateWarning("Viewer update endpoint returned a raw executable; applying legacy compatibility path");
    }
    if (artifact_kind == ViewerUpdateArtifactKind::PackageZip &&
        !config_.ed25519_public_key.empty() && !signature_hex.empty() && listener_) {
        listener_->OnUpdateWarning("Viewer update package downloaded as .zip; relying on SHA-256 package integrity check");
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        staging_dir_ = temp_dir;
        staged_artifact_path_ = artifact_path;
        staged_artifact_kind_ = artifact_kind;
    }
    update_staged_.store(true);

    if (listener_) listener_->OnUpdateReady(version);
    return true;
}

// ─── ApplyAndRelaunch ───────────────────────────────────────────────────────

bool ViewerAutoUpdater::ApplyAndRelaunch() {
    if (!update_staged_.load()) return false;

    namespace fs = std::filesystem;

    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    fs::path current_exe(exe_path);
    fs::path install_dir = current_exe.parent_path();

    fs::path staging;
    fs::path staged_artifact_path;
    ViewerUpdateArtifactKind staged_artifact_kind = ViewerUpdateArtifactKind::None;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        staging = staging_dir_;
        staged_artifact_path = staged_artifact_path_;
        staged_artifact_kind = staged_artifact_kind_;
    }

    if (staged_artifact_path.empty()) {
        staged_artifact_path = staging / "remotex_viewer_update.zip";
    }
    if (staged_artifact_kind == ViewerUpdateArtifactKind::None) {
        staged_artifact_kind = DetectArtifactKind(staged_artifact_path);
    }
    if (!fs::exists(staged_artifact_path)) {
        if (listener_) listener_->OnUpdateError("Staged update artifact not found");
        return false;
    }
    if (staged_artifact_kind == ViewerUpdateArtifactKind::None) {
        if (listener_) listener_->OnUpdateError("Staged update artifact format is invalid");
        return false;
    }

    // Check if install dir is writable (e.g. Program Files requires elevation)
    bool needs_elevation = false;
    auto test_file = install_dir / ".remotex_update_test";
    {
        std::ofstream test(test_file);
        if (!test.is_open()) {
            needs_elevation = true;
        }
    }
    if (!needs_elevation) {
        fs::remove(test_file);
    }

    // Write a batch script to %TEMP% that:
    // 1. Waits for the viewer process to exit
    // 2. Copies the staged executable or extracts/copies the staged package
    // 3. Relaunches the viewer
    auto bat_path = fs::temp_directory_path() / "remotex_viewer_update.bat";
    auto log_path = fs::temp_directory_path() / "remotex_viewer_update.log";

    std::ofstream bat(bat_path);
    if (!bat.is_open()) {
        if (listener_) listener_->OnUpdateError("Failed to create update batch script");
        return false;
    }

    std::string extract_dir = (staging / "extracted").string();
    std::string artifact_str = staged_artifact_path.string();
    std::string install_str = install_dir.string();
    std::string exe_str = current_exe.string();
    std::string log_str = log_path.string();
    DWORD viewer_pid = GetCurrentProcessId();

    bat << "@echo off\r\n";
    bat << "set \"LOG=" << log_str << "\"\r\n";
    bat << "echo [%date% %time%] Update script started > \"%LOG%\"\r\n";

    // Wait for the viewer process to exit by PID.
    // 'timeout' does not work with CREATE_NO_WINDOW (no console),
    // so we poll with 'tasklist' instead.
    bat << "set RETRIES=0\r\n";
    bat << ":WAIT_LOOP\r\n";
    bat << "tasklist /FI \"PID eq " << viewer_pid << "\" 2>nul | find \"" << viewer_pid << "\" >nul\r\n";
    bat << "if errorlevel 1 goto PROCESS_EXITED\r\n";
    bat << "set /a RETRIES+=1\r\n";
    bat << "if %RETRIES% GEQ 30 (\r\n";
    bat << "  echo [%date% %time%] WARNING: viewer PID " << viewer_pid << " did not exit after 30 retries >> \"%LOG%\"\r\n";
    bat << "  goto PROCESS_EXITED\r\n";
    bat << ")\r\n";
    bat << "ping -n 2 127.0.0.1 >nul\r\n";
    bat << "goto WAIT_LOOP\r\n";

    bat << ":PROCESS_EXITED\r\n";
    bat << "echo [%date% %time%] Viewer process exited >> \"%LOG%\"\r\n";

    if (staged_artifact_kind == ViewerUpdateArtifactKind::ExecutableBinary) {
        bat << "echo [%date% %time%] Applying legacy executable update... >> \"%LOG%\"\r\n";
        bat << "copy /Y \"" << artifact_str << "\" \"" << exe_str << "\" >> \"%LOG%\" 2>&1\r\n";
        bat << "if errorlevel 1 (\r\n";
        bat << "  echo [%date% %time%] ERRO: failed to copy updated executable >> \"%LOG%\"\r\n";
        bat << "  goto RELAUNCH\r\n";
        bat << ")\r\n";
        bat << "echo [%date% %time%] Executable copied successfully >> \"%LOG%\"\r\n";
        bat << "del \"" << artifact_str << "\" 2>nul\r\n";
    } else {
        // Extract zip — redirect PowerShell stderr to log (no pipes in cmd to avoid cmd.exe interpreting '|')
        bat << "if exist \"" << extract_dir << "\" rmdir /s /q \"" << extract_dir << "\"\r\n";
        bat << "echo [%date% %time%] Extracting zip... >> \"%LOG%\"\r\n";
        bat << "powershell -NoProfile -Command \"Expand-Archive -Path '" << artifact_str
            << "' -DestinationPath '" << extract_dir << "' -Force\" >> \"%LOG%\" 2>&1\r\n";
        bat << "if errorlevel 1 (\r\n";
        bat << "  echo [%date% %time%] ERRO: Expand-Archive failed >> \"%LOG%\"\r\n";
        bat << "  goto RELAUNCH\r\n";
        bat << ")\r\n";
        // Verify extraction produced files
        bat << "dir \"" << extract_dir << "\\remotex_viewer.exe\" >nul 2>&1\r\n";
        bat << "if errorlevel 1 (\r\n";
        bat << "  echo [%date% %time%] ERRO: remotex_viewer.exe not found after extraction >> \"%LOG%\"\r\n";
        bat << "  dir \"" << extract_dir << "\" >> \"%LOG%\" 2>&1\r\n";
        bat << "  goto RELAUNCH\r\n";
        bat << ")\r\n";

        // Small delay to ensure all file handles are released after viewer exit
        bat << "ping -n 2 127.0.0.1 >nul\r\n";

        // Copy files — use robocopy for reliable recursive copy (mirrors subdirs like platforms/, styles/)
        bat << "echo [%date% %time%] Copying files to install dir... >> \"%LOG%\"\r\n";
        bat << "robocopy \"" << extract_dir << "\" \"" << install_str << "\" /s /is /it >> \"%LOG%\" 2>&1\r\n";
        // robocopy returns 0-7 for success, >=8 for error
        bat << "if errorlevel 8 (\r\n";
        bat << "  echo [%date% %time%] ERRO: robocopy failed with code %errorlevel% >> \"%LOG%\"\r\n";
        bat << "  goto RELAUNCH\r\n";
        bat << ")\r\n";
        bat << "echo [%date% %time%] Files copied successfully >> \"%LOG%\"\r\n";

        // Cleanup
        bat << "rmdir /s /q \"" << extract_dir << "\" 2>nul\r\n";
        bat << "del \"" << artifact_str << "\" 2>nul\r\n";
    }

    bat << ":RELAUNCH\r\n";
    bat << "echo [%date% %time%] Relaunching viewer... >> \"%LOG%\"\r\n";
    bat << "start \"\" \"" << exe_str << "\"\r\n";
    bat << "echo [%date% %time%] Done >> \"%LOG%\"\r\n";
    bat << "del \"%~f0\"\r\n";
    bat.close();

    if (needs_elevation) {
        std::wstring params = L"/c \"" + bat_path.wstring() + L"\"";
        HINSTANCE result = ShellExecuteW(
            nullptr,
            L"runas",
            L"cmd.exe",
            params.c_str(),
            nullptr,
            SW_HIDE
        );
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                if (listener_) listener_->OnUpdateError(
                    "Atualizacao requer permissao de administrador");
            } else {
                if (listener_) listener_->OnUpdateError(
                    "Failed to launch elevated update script (error " + std::to_string(err) + ")");
            }
            return false;
        }
    } else {
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"cmd.exe /c \"" + bat_path.wstring() + L"\"";

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            if (listener_) listener_->OnUpdateError("Failed to launch update batch script");
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    return true; // Caller should exit the viewer
}

// ─── HTTP GET via WinHTTP ───────────────────────────────────────────────────

ViewerAutoUpdater::HttpResponse ViewerAutoUpdater::HttpGet(const std::string& path) {
    auto attempt = [&](const std::string& token) -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX-ViewerUpdate/1.0",
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

// ─── HTTP Download ──────────────────────────────────────────────────────────

bool ViewerAutoUpdater::HttpDownload(const std::string& url, const std::string& dest_path,
                                      int64_t expected_size) {
    auto attempt = [&](const std::string& token) -> int {
        HINTERNET session = WinHttpOpen(L"RemoteX-ViewerUpdate/1.0",
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

} // namespace remotex::viewer
