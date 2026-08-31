#pragma once
// RemoteX — updater/updater_service.h
// Windows Service wrapper for the Updater service.

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace remotex::updater {

inline constexpr const wchar_t* kUpdaterServiceName    = L"RemoteXUpdater";
inline constexpr const wchar_t* kUpdaterServiceDisplay = L"RemoteX Update Service";

// Named event used to wake the updater from its sleep cycle (e.g., for force update).
inline constexpr const wchar_t* kWakeupEventName = L"Global\\RemoteXUpdaterWakeup";

using UpdaterEntryFn = std::function<int(std::atomic<bool>& stop_flag)>;

// Callback invoked from ServiceCtrlHandler when stop is requested.
// Must be set before RunAsService() to allow the handler to wake sleeping threads.
using OnStopRequestedFn = std::function<void()>;

class UpdaterService {
public:
    static bool Install(const std::wstring& exe_path = L"");
    static bool Uninstall();
    static bool RunAsService(UpdaterEntryFn entry_fn);

    // Set a callback that will be called from ServiceCtrlHandler on stop/shutdown.
    // This allows the main loop to be woken from interruptible sleeps.
    static void SetOnStopRequested(OnStopRequestedFn fn);

    // Custom service control code to trigger an immediate update check cycle.
    static constexpr DWORD kControlWakeup = 128;

private:
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static void WINAPI ServiceCtrlHandler(DWORD control);

    static UpdaterEntryFn        s_entry_fn;
    static OnStopRequestedFn     s_on_stop_requested;
    static SERVICE_STATUS        s_status;
    static SERVICE_STATUS_HANDLE s_status_handle;
    static std::atomic<bool>     s_stop_flag;
};

} // namespace remotex::updater
