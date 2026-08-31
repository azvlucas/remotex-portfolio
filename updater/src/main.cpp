// RemoteX — updater/main.cpp
// Entry point for the independent Updater service.
//
// Modes:
//   --install    → register the Windows Service and exit
//   --uninstall  → remove the Windows Service and exit
//   --service    → SCM entry point (UpdaterServiceMain)
//   (no args)    → console mode (debug)
//
// Service loop (every 1h):
//   1. Auth as machine (same credentials as host in CredManager)
//   2. Read connection settings from registry
//   3. Check self-update → if available: download, verify, rename self → .bak, copy new, exit 100
//   4. Check host update → if available: download, verify, stop host, swap binary, start host
//   5. Health monitor: after host update, monitor for 120s, rollback if host crashes repeatedly
//   6. Sleep 1h, repeat

#include "updater/updater_service.h"
#include "updater/host_updater.h"
#include "updater/self_updater.h"
#include "updater/health_monitor.h"

#include "core/auth_client.h"
#include "core/connection_settings.h"
#include "core/log_redaction.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

// ─── Version ─────────────────────────────────────────────────────────────────

static constexpr const char* kVersion = REMOTEX_UPDATER_VERSION;
static constexpr const char* kMachineCredentialTarget = "RemoteXHostMachine";
static constexpr int kCheckIntervalSec = 3600;  // 1 hour

// ─── Registry keys ───────────────────────────────────────────────────────────

static constexpr const wchar_t* kRegistryHost    = L"SOFTWARE\\RemoteX\\Host";
static constexpr const wchar_t* kRegistryLegacy  = L"SOFTWARE\\RemoteAccess\\Host";
static constexpr const wchar_t* kRegistryUpdater = L"SOFTWARE\\RemoteX\\Updater";

// ─── Logging ─────────────────────────────────────────────────────────────────

static FILE* g_log_file = nullptr;
static std::mutex g_log_mutex;

static fs::path GetExecutableDirectory() {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return fs::path(exe_path).parent_path();
}

static void OpenLogFile() {
    wchar_t program_data[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);

    fs::path log_dir;
    if (len > 0 && len < MAX_PATH) {
        log_dir = fs::path(program_data) / "RemoteX" / "logs";
    } else {
        log_dir = fs::path(L"C:\\ProgramData") / "RemoteX" / "logs";
    }
    if (log_dir.empty()) {
        log_dir = GetExecutableDirectory();
    }

    std::error_code ec;
    fs::create_directories(log_dir, ec);
    if (ec && log_dir != GetExecutableDirectory()) {
        log_dir = GetExecutableDirectory();
        ec.clear();
        fs::create_directories(log_dir, ec);
    }

    fs::path log_path = log_dir / "remotex_updater.log";

    if (fs::exists(log_path, ec) && fs::file_size(log_path, ec) > 5ull * 1024ull * 1024ull) {
        fs::path backup_path = log_dir / "remotex_updater.log.1";
        fs::remove(backup_path, ec);
        ec.clear();
        fs::rename(log_path, backup_path, ec);
    }

    g_log_file = _wfopen(log_path.c_str(), L"a");
    if (g_log_file) {
        setvbuf(g_log_file, nullptr, _IONBF, 0);
        fprintf(g_log_file, "[updater] Log file opened\n");
    } else {
        fprintf(stderr, "[updater] CRITICAL: Failed to open updater log file, error=%lu\n",
                GetLastError());
    }
}

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, fmt);

    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const std::string sanitized = remotex::core::SanitizeLogMessage(msg);

    if (g_log_file) {
        fprintf(g_log_file, "[%s] %s\n", timestamp, sanitized.c_str());
    }

    fprintf(stderr, "[%s] %s\n", timestamp, sanitized.c_str());
}

static void LogString(const std::string& msg) {
    Log("%s", msg.c_str());
}

// ─── Host version detection ──────────────────────────────────────────────────

static std::string GetHostVersion() {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        Log("WARNING: Could not determine host version (no VERSION file, no registry key)");
        return "unknown";
    }

    fs::path host_exe = fs::path(exe_path).parent_path() / "remotex_host.exe";
    if (!fs::exists(host_exe)) {
        Log("WARNING: Could not determine host version (no VERSION file, no registry key)");
        return "unknown";
    }

    // Read VERSION file if present.
    fs::path version_file = fs::path(exe_path).parent_path() / "VERSION";
    if (fs::exists(version_file)) {
        std::ifstream vf(version_file);
        std::string version;
        if (std::getline(vf, version) && !version.empty()) {
            // Trim whitespace.
            while (!version.empty() && (version.back() == '\r' || version.back() == '\n' || version.back() == ' '))
                version.pop_back();
            Log("Host version from VERSION file: %s", version.c_str());
            return version;
        }
    }

    // Fallback: read from registry.
    auto version = remotex::core::ReadRegistryStringValue(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\RemoteX\\Host", "Version");
    if (!version.empty()) {
        Log("Host version from registry: %s", version.c_str());
        return version;
    }

    Log("WARNING: Could not determine host version (no VERSION file, no registry key)");
    return "unknown";
}

// ─── Release public key ──────────────────────────────────────────────────────

static std::vector<uint8_t> LoadReleasePublicKey() {
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    fs::path key_path = fs::path(exe_path).parent_path() / "ed25519_public.bin";
    auto key = remotex::updater::LoadEd25519PublicKeyFile(key_path);
    if (key.empty()) {
        Log("WARNING: Release public key missing or invalid at %s",
            remotex::core::RedactPath(key_path, "release_key").c_str());
    }
    return key;
}

// ─── Write updater state to registry ─────────────────────────────────────────

static void WriteUpdaterRegistryState(const std::string& version,
                                      const std::string& host_version) {
    remotex::core::WriteRegistryStringValue(
        HKEY_LOCAL_MACHINE, kRegistryUpdater, "updater_version", version);
    if (!host_version.empty()) {
        remotex::core::WriteRegistryStringValue(
            HKEY_LOCAL_MACHINE, kRegistryUpdater, "last_update_version", host_version);

        // Write current UTC timestamp.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        uint64_t unix_seconds = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
        remotex::core::WriteRegistryStringValue(
            HKEY_LOCAL_MACHINE, kRegistryUpdater, "last_update_utc",
            std::to_string(unix_seconds));
    }
}

// ─── Interruptible sleep via Windows event ──────────────────────────────────
// A manual-reset event that can be signaled from:
//   - ServiceCtrlHandler (on stop/shutdown/wakeup)
//   - ConsoleHandler (Ctrl+C)
//   - Host process via named event (force update)

static HANDLE g_wakeup_event = nullptr;

static void CreateWakeupEvent() {
    // Create a named event so the host process can also signal it for force updates.
    g_wakeup_event = CreateEventW(nullptr, TRUE,  // manual-reset
                                  FALSE, // initial: non-signaled
                                  remotex::updater::kWakeupEventName);
    if (g_wakeup_event) {
        DWORD last_err = GetLastError();
        if (last_err == ERROR_ALREADY_EXISTS) {
            Log("Wakeup event opened (already existed): %ls", remotex::updater::kWakeupEventName);
        } else {
            Log("Wakeup event created: %ls", remotex::updater::kWakeupEventName);
        }
        return;
    }

    Log("WARNING: Failed to create named wakeup event (err=%lu), falling back to unnamed event",
        GetLastError());

    // Fallback to unnamed event (host won't be able to signal force-update directly).
    g_wakeup_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_wakeup_event) {
        Log("Fallback unnamed wakeup event created (host force-update via named event will NOT work)");
    } else {
        Log("ERROR: Failed to create any wakeup event (err=%lu)", GetLastError());
    }
}

// Wait for the specified duration, returning early if the event is signaled.
// Returns true if the event was signaled (stop or wakeup), false if timeout.
static bool InterruptibleWait(DWORD timeout_ms) {
    if (!g_wakeup_event) return false;
    DWORD result = WaitForSingleObject(g_wakeup_event, timeout_ms);
    // Reset the event for the next wait cycle.
    ResetEvent(g_wakeup_event);
    return (result == WAIT_OBJECT_0);
}

static void SignalWakeup() {
    if (g_wakeup_event) {
        SetEvent(g_wakeup_event);
    }
}

// ─── Main updater loop ──────────────────────────────────────────────────────

static int RunUpdater(bool is_service, std::atomic<bool>& stop_flag) {
    Log("RemoteX Updater v%s starting (service=%d)", kVersion, is_service ? 1 : 0);

    CreateWakeupEvent();

    // Register the stop callback so ServiceCtrlHandler can wake us.
    remotex::updater::UpdaterService::SetOnStopRequested([] {
        SignalWakeup();
    });

    // Clean up any previous self-update backup.
    remotex::updater::SelfUpdater self_updater;
    self_updater.Configure({}, nullptr, LogString);
    self_updater.CleanupPreviousBackup();

    // Write our version to registry.
    WriteUpdaterRegistryState(kVersion, "");

    while (!stop_flag.load()) {
        // 1. Load connection settings from the same registry as the host.
        bool used_legacy = false;
        auto conn = remotex::core::LoadConnectionSettingsFromRegistryWithFallback(
            HKEY_LOCAL_MACHINE, kRegistryHost, kRegistryLegacy, &used_legacy);

        Log("Connection: api_host=%s api_port=%u tls=%d",
            remotex::core::RedactIdentifier(conn.api_host, "api_host").c_str(),
            conn.api_port,
            conn.api_use_tls);

        // 2. Auth as machine (same CredManager target as host).
        remotex::core::AuthClient auth;
        remotex::core::AuthConfig auth_cfg;
        auth_cfg.api_host = conn.api_host;
        auth_cfg.api_port = conn.api_port;
        auth_cfg.use_tls  = conn.api_use_tls;
        auth_cfg.credential_target = kMachineCredentialTarget;
        auth.Configure(auth_cfg);

        // Try to load saved machine credentials.
        if (!auth.LoadStoredTokens()) {
            Log("WARNING: No machine credentials found, skipping update cycle");
        } else {
            auto public_key = LoadReleasePublicKey();
            std::string host_version = GetHostVersion();

            // 3. Check self-update first.
            if (!stop_flag.load()) {
                remotex::updater::SelfUpdateConfig self_cfg;
                self_cfg.api_host           = conn.api_host;
                self_cfg.api_port           = conn.api_port;
                self_cfg.use_tls            = conn.api_use_tls;
                self_cfg.current_version    = kVersion;
                self_cfg.ed25519_public_key = public_key;
                self_updater.Configure(self_cfg, &auth, LogString);

                if (self_updater.CheckAndApply()) {
                    Log("Self-update applied. Exiting with code 100 for SCM restart.");
                    if (g_wakeup_event) CloseHandle(g_wakeup_event);
                    return 100;
                }
            }

            // 4. Check host update.
            if (!stop_flag.load()) {
                remotex::updater::HostUpdater host_updater;
                remotex::updater::HostUpdateConfig host_cfg;
                host_cfg.api_host              = conn.api_host;
                host_cfg.api_port              = conn.api_port;
                host_cfg.use_tls               = conn.api_use_tls;
                host_cfg.current_host_version  = host_version;
                host_cfg.ed25519_public_key    = public_key;
                host_updater.Configure(host_cfg, &auth, LogString);

                if (host_updater.CheckAndApply()) {
                    // 5. Health monitor after host update.
                    wchar_t exe_path[MAX_PATH];
                    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
                    if (len > 0 && len < MAX_PATH) {
                        fs::path install_dir = fs::path(exe_path).parent_path();
                        fs::path host_exe = install_dir / "remotex_host.exe";
                        fs::path backup_exe = host_exe;
                        backup_exe += ".bak";

                        remotex::updater::HealthMonitor monitor;
                        remotex::updater::HealthMonitorConfig mon_cfg;
                        monitor.Configure(mon_cfg, LogString);

                        bool healthy = monitor.MonitorAfterUpdate(
                            host_exe, backup_exe, stop_flag, g_wakeup_event);
                        if (healthy) {
                            WriteUpdaterRegistryState(kVersion, host_updater.LastAppliedVersion());
                        }
                    }
                }
            }
        }

        if (stop_flag.load()) break;

        // 6. Sleep for check interval (interruptible via event).
        Log("Update cycle complete. Next check in %d seconds (wakeup event active=%s)",
            kCheckIntervalSec, g_wakeup_event ? "yes" : "no");
        bool woken = InterruptibleWait(kCheckIntervalSec * 1000);
        if (woken) {
            Log("Woke up early - force update or stop signal received");
        } else {
            Log("Scheduled update check cycle starting");
        }
    }

    Log("Updater shutting down");
    if (g_wakeup_event) CloseHandle(g_wakeup_event);
    return 0;
}

// ─── Console Ctrl handler ────────────────────────────────────────────────────

static std::atomic<bool>* g_stop_flag = nullptr;

static BOOL WINAPI ConsoleHandler(DWORD /*ctrl*/) {
    if (g_stop_flag) g_stop_flag->store(true);
    SignalWakeup();
    return TRUE;
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    OpenLogFile();
    Log("Process started (argc=%d, version=%s)", argc, kVersion);

    bool do_install   = false;
    bool do_uninstall = false;
    bool do_service   = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--install")        do_install = true;
        else if (arg == "--uninstall") do_uninstall = true;
        else if (arg == "--service")   do_service = true;
    }

    if (do_install) {
        return remotex::updater::UpdaterService::Install() ? 0 : 1;
    }

    if (do_uninstall) {
        return remotex::updater::UpdaterService::Uninstall() ? 0 : 1;
    }

    if (do_service) {
        auto entry_fn = [](std::atomic<bool>& stop_flag) -> int {
            g_stop_flag = &stop_flag;
            return RunUpdater(/*is_service=*/true, stop_flag);
        };
        return remotex::updater::UpdaterService::RunAsService(entry_fn) ? 0 : 1;
    }

    // Console mode (debug).
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::atomic<bool> console_stop{false};
    g_stop_flag = &console_stop;
    return RunUpdater(/*is_service=*/false, console_stop);
}
