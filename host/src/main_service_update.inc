// ─── Updater service migration ───────────────────────────────────────────────
// v0.2.0 transitional: if remotex_updater.exe exists alongside the host and the
// RemoteXUpdater service is not installed, install and start it.

// Forward declaration (defined later in this file).
static bool StartNamedService(const std::wstring& service_name);

static bool IsUpdaterServiceInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, L"RemoteXUpdater", SERVICE_QUERY_STATUS);
    bool installed = (svc != nullptr);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return installed;
}

static bool IsUpdaterServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, L"RemoteXUpdater", SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    bool running = false;
    if (QueryServiceStatus(svc, &status)) {
        running = (status.dwCurrentState == SERVICE_RUNNING ||
                   status.dwCurrentState == SERVICE_START_PENDING);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

static void TryInstallUpdaterService() {
    fs::path exe_path = GetCurrentExecutablePath();
    if (exe_path.empty()) return;

    fs::path updater_exe = exe_path.parent_path() / "remotex_updater.exe";
    if (!fs::exists(updater_exe)) {
        return;
    }

    if (IsUpdaterServiceInstalled()) {
        // Already installed — ensure it's running.
        if (!IsUpdaterServiceRunning()) {
            Log("RemoteXUpdater service installed but not running, starting...");
            StartNamedService(L"RemoteXUpdater");
        }
        return;
    }

    Log("Detected remotex_updater.exe, installing RemoteXUpdater service...");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmd = L"\"" + updater_exe.wstring() + L"\" --install";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        Log("Starting RemoteXUpdater service...");
        StartNamedService(L"RemoteXUpdater");
    } else {
        Log("WARNING: Failed to install RemoteXUpdater (error=%lu)", GetLastError());
    }
}

static bool WaitForFileAvailability(const fs::path& path, DWORD timeout_ms) {
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

static bool StopNamedService(const std::wstring& service_name, DWORD timeout_ms) {
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

static bool StartNamedService(const std::wstring& service_name) {
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

static int RunApplyUpdateHelper(const std::wstring& service_name,
                                const fs::path& target_exe,
                                const fs::path& source_binary,
                                const fs::path& backup_exe,
                                const std::string& target_version,
                                const fs::path& status_file) {
    std::error_code ec;

    WriteUpdateHelperStatus(status_file,
                            "started",
                            0,
                            "start",
                            "Helper launched",
                            target_exe,
                            source_binary,
                            target_version);

    if (!StopNamedService(service_name, 60000)) {
        WriteUpdateHelperStatus(status_file,
                                "failed",
                                2,
                                "stop_service",
                                "Failed to stop service before applying update",
                                target_exe,
                                source_binary,
                                target_version);
        return 2;
    }
    if (!WaitForFileAvailability(target_exe, 60000)) {
        WriteUpdateHelperStatus(status_file,
                                "failed",
                                3,
                                "wait_target",
                                "Timed out waiting for target executable to become writable",
                                target_exe,
                                source_binary,
                                target_version);
        return 3;
    }

    fs::remove(backup_exe, ec);
    ec.clear();

    fs::rename(target_exe, backup_exe, ec);
    if (ec) {
        WriteUpdateHelperStatus(status_file,
                                "failed",
                                4,
                                "backup_current",
                                "Failed to rename current executable to backup: " + ec.message(),
                                target_exe,
                                source_binary,
                                target_version);
        return 4;
    }

    fs::copy_file(source_binary, target_exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::error_code rollback_ec;
        fs::rename(backup_exe, target_exe, rollback_ec);
        std::string message = "Failed to copy new executable: " + ec.message();
        if (rollback_ec) {
            message += " (rollback failed: " + rollback_ec.message() + ")";
        }
        WriteUpdateHelperStatus(status_file,
                                "failed",
                                5,
                                "copy_new_binary",
                                message,
                                target_exe,
                                source_binary,
                                target_version);
        return 5;
    }

    fs::remove(source_binary, ec);
    if (!StartNamedService(service_name)) {
        WriteUpdateHelperStatus(status_file,
                                "failed",
                                6,
                                "start_service",
                                "New binary copied, but failed to start service",
                                target_exe,
                                source_binary,
                                target_version);
        return 6;
    }

    WriteUpdateHelperStatus(status_file,
                            "success",
                            0,
                            "complete",
                            "Update applied and service restarted",
                            target_exe,
                            source_binary,
                            target_version);
    return 0;
}

static bool DesktopWorkerHealthy(DWORD expected_session, DWORD expected_pid,
                                 std::chrono::steady_clock::time_point started_at) {
    const uint64_t heartbeat_utc = LoadRegQword(kRegWorkerHeartbeatUtc);
    const DWORD session_id = LoadRegDword(kRegWorkerSessionId);
    const DWORD pid = LoadRegDword(kRegWorkerPid);
    const uint64_t now_utc = GetUnixTimeSeconds();

    if (heartbeat_utc == 0 || pid != expected_pid || session_id != expected_session) {
        return (std::chrono::steady_clock::now() - started_at) < std::chrono::seconds(20);
    }

    return now_utc >= heartbeat_utc && (now_utc - heartbeat_utc) <= 15;
}

static bool DesktopWorkerSignalingHealthy(std::chrono::steady_clock::time_point started_at) {
    using namespace std::chrono_literals;

    if ((std::chrono::steady_clock::now() - started_at) < 45s) {
        return true;
    }

    const uint64_t signaling_utc = LoadRegQword(kRegWorkerSignalingUtc);
    const DWORD signaling_up = LoadRegDword(kRegWorkerSignalingUp);
    const uint64_t now_utc = GetUnixTimeSeconds();

    if (signaling_utc == 0) {
        return false;
    }

    const uint64_t age_sec = now_utc >= signaling_utc ? (now_utc - signaling_utc) : 0;

    if (signaling_up != 0) {
        return age_sec <= 120;
    }

    return age_sec <= 180;
}

struct DesktopWorkerRegistryState {
    uint64_t heartbeat_utc = 0;
    DWORD session_id = 0;
    DWORD pid = 0;
    uint64_t signaling_utc = 0;
    DWORD signaling_up = 0;
};

static DesktopWorkerRegistryState LoadDesktopWorkerRegistryState() {
    DesktopWorkerRegistryState state;
    state.heartbeat_utc = LoadRegQword(kRegWorkerHeartbeatUtc);
    state.session_id = LoadRegDword(kRegWorkerSessionId);
    state.pid = LoadRegDword(kRegWorkerPid);
    state.signaling_utc = LoadRegQword(kRegWorkerSignalingUtc);
    state.signaling_up = LoadRegDword(kRegWorkerSignalingUp);
    return state;
}

// ─── RunHost: lógica principal extraída para uso em console e serviço ───────

// is_service: true quando rodando como Windows Service (Session 0 watcher).
// is_desktop: true quando rodando como processo filho na sessão interativa (--desktop).
// stop_flag: referência ao flag de shutdown (console ou serviço).
// cli_email/cli_password: credenciais passadas via --email/--password.
