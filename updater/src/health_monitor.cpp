// RemoteX — updater/health_monitor.cpp
// Post-update health check: monitors host service and rolls back if it crashes.

#include "updater/health_monitor.h"
#include "updater/host_updater.h"  // For StopNamedService, StartNamedService.
#include "core/connection_settings.h"  // For WriteRegistryStringValue.

#include <chrono>
#include <filesystem>
#include <system_error>

namespace remotex::updater {

namespace fs = std::filesystem;

static constexpr const wchar_t* kUpdaterRegistryKey = L"SOFTWARE\\RemoteX\\Updater";

static const char* ServiceStateName(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED:
        return "STOPPED";
    case SERVICE_RUNNING:
        return "RUNNING";
    case SERVICE_START_PENDING:
        return "START_PENDING";
    case SERVICE_STOP_PENDING:
        return "STOP_PENDING";
    default:
        return "UNKNOWN";
    }
}

void HealthMonitor::Configure(const HealthMonitorConfig& config, LogFn log_fn) {
    config_ = config;
    log_fn_ = std::move(log_fn);
}

void HealthMonitor::Log(const std::string& msg) {
    if (log_fn_) log_fn_(msg);
}

void HealthMonitor::WriteRegistryResult(const std::string& result) {
    remotex::core::WriteRegistryStringValue(
        HKEY_LOCAL_MACHINE, kUpdaterRegistryKey, "last_update_result", result);
}

bool HealthMonitor::MonitorAfterUpdate(const fs::path& target_exe,
                                       const fs::path& backup_exe,
                                       std::atomic<bool>& stop_flag,
                                       HANDLE wakeup_event) {
    if (!fs::exists(backup_exe)) {
        Log("No backup binary found, skipping health monitoring");
        return true;
    }

    Log("Monitoring host service for " + std::to_string(config_.monitor_duration_sec) +
        "s after update...");

    int consecutive_crashes = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(config_.monitor_duration_sec);

    while (std::chrono::steady_clock::now() < deadline) {
        // Interruptible sleep: use the wakeup event if available, else Sleep.
        if (wakeup_event) {
            WaitForSingleObject(wakeup_event, config_.check_interval_sec * 1000);
        } else {
            Sleep(config_.check_interval_sec * 1000);
        }

        // If service shutdown was requested, abort monitoring but don't rollback
        // (the update may be fine, we just can't verify right now).
        if (stop_flag.load()) {
            Log("Service shutdown requested during health monitoring, aborting monitor");
            return true;
        }

        // Query service status.
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            Log("Health check: failed to open SCM (err=" + std::to_string(GetLastError()) + ")");
            continue;
        }

        SC_HANDLE svc = OpenServiceW(scm, config_.host_service_name.c_str(),
                                     SERVICE_QUERY_STATUS);
        if (!svc) {
            Log("Health check: failed to open host service (err=" + std::to_string(GetLastError()) + ")");
            CloseServiceHandle(scm);
            continue;
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed = 0;
        bool query_ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                             reinterpret_cast<BYTE*>(&status),
                                             sizeof(status), &bytes_needed) != FALSE;
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);

        if (!query_ok) {
            Log("Health check: QueryServiceStatusEx failed (err=" + std::to_string(GetLastError()) + ")");
            continue;
        }

        Log("Health check: host service state=" + std::string(ServiceStateName(status.dwCurrentState)) +
            " pid=" + std::to_string(status.dwProcessId) +
            " crashes=" + std::to_string(consecutive_crashes));

        if (status.dwCurrentState == SERVICE_STOPPED) {
            consecutive_crashes++;
            Log("Host service stopped unexpectedly (crash " +
                std::to_string(consecutive_crashes) + "/" +
                std::to_string(config_.max_crashes) + ")");

            if (consecutive_crashes >= config_.max_crashes) {
                // Rollback.
                Log("Maximum crashes reached, rolling back to previous version...");
                {
                    std::error_code target_ec;
                    auto target_size = fs::file_size(target_exe, target_ec);
                    std::string target_size_text = target_ec ? "unknown(" + target_ec.message() + ")" :
                                                               std::to_string(target_size);

                    std::error_code backup_ec;
                    auto backup_size = fs::file_size(backup_exe, backup_ec);
                    std::string backup_size_text = backup_ec ? "unknown(" + backup_ec.message() + ")" :
                                                               std::to_string(backup_size);

                    Log("Rollback: removing target (" + target_size_text +
                        " bytes), restoring backup (" + backup_size_text + " bytes)");
                }

                StopNamedService(config_.host_service_name, 10000);

                std::error_code ec;
                fs::remove(target_exe, ec);
                ec.clear();
                fs::rename(backup_exe, target_exe, ec);
                if (ec) {
                    Log("ERROR: Rollback failed: " + ec.message());
                    WriteRegistryResult("rollback_failed");
                    return false;
                }

                Log("Rollback: binary restored, VERSION file may be stale (will self-correct on next cycle)");

                if (!StartNamedService(config_.host_service_name)) {
                    Log("ERROR: Failed to start host after rollback");
                    WriteRegistryResult("rollback_start_failed");
                    return false;
                }

                Log("Rollback completed, host running with previous version");
                WriteRegistryResult("rollback");
                return false;
            }

            // Try to restart the host to give it another chance.
            StartNamedService(config_.host_service_name);
        } else if (status.dwCurrentState == SERVICE_RUNNING) {
            // Reset crash counter on success.
            consecutive_crashes = 0;
        }
    }

    // Monitoring period completed successfully.
    Log("Host service healthy after update, cleaning up backup");

    std::error_code ec;
    fs::remove(backup_exe, ec);

    WriteRegistryResult("success");
    return true;
}

} // namespace remotex::updater
