#pragma once
// RemoteX — updater/health_monitor.h
// Monitors the host service after an update and rolls back if it crashes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace remotex::updater {

using LogFn = std::function<void(const std::string&)>;

struct HealthMonitorConfig {
    std::wstring host_service_name = L"RemoteXHost";
    int          monitor_duration_sec = 120;    // Monitor for 2 minutes after update.
    int          check_interval_sec   = 10;     // Check every 10 seconds.
    int          max_crashes          = 2;      // Rollback after this many consecutive crashes.
};

class HealthMonitor {
public:
    void Configure(const HealthMonitorConfig& config, LogFn log_fn);

    // Monitor host after update. If it crashes repeatedly, restore backup and restart.
    // Returns true if the host remained healthy, false if rollback was performed.
    // stop_flag and wakeup_event allow the monitor to be interrupted for service shutdown.
    bool MonitorAfterUpdate(const std::filesystem::path& target_exe,
                            const std::filesystem::path& backup_exe,
                            std::atomic<bool>& stop_flag,
                            HANDLE wakeup_event);

private:
    void Log(const std::string& msg);
    void WriteRegistryResult(const std::string& result);

    HealthMonitorConfig config_;
    LogFn               log_fn_;
};

} // namespace remotex::updater
