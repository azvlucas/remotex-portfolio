// RemoteX — updater/updater_service.cpp
// Windows Service integration for the Updater service.

#include "updater/updater_service.h"

#include <cstdio>

namespace remotex::updater {

namespace {

bool ConfigureInstalledService(SC_HANDLE svc, const std::wstring& command_line) {
    SERVICE_DESCRIPTIONW desc;
    desc.lpDescription = const_cast<LPWSTR>(
        L"RemoteX Update Service - manages automatic updates for the RemoteX Host agent.");
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc)) {
        fprintf(stderr, "[updater-service] ERROR: ChangeServiceConfig2(description) failed, error=%lu\n",
                GetLastError());
        return false;
    }

    if (!ChangeServiceConfigW(svc,
                              SERVICE_NO_CHANGE,
                              SERVICE_AUTO_START,
                              SERVICE_ERROR_NORMAL,
                              command_line.c_str(),
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              kUpdaterServiceDisplay)) {
        fprintf(stderr, "[updater-service] ERROR: ChangeServiceConfig failed, error=%lu\n", GetLastError());
        return false;
    }

    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_RESTART, 30000 },
    };
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa)) {
        fprintf(stderr, "[updater-service] WARNING: ChangeServiceConfig2(failure actions) failed, error=%lu\n",
                GetLastError());
    }

    SERVICE_FAILURE_ACTIONS_FLAG failure_flag{};
    failure_flag.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failure_flag)) {
        fprintf(stderr, "[updater-service] WARNING: ChangeServiceConfig2(failure actions flag) failed, error=%lu\n",
                GetLastError());
    }

    return true;
}

} // namespace

UpdaterEntryFn        UpdaterService::s_entry_fn;
OnStopRequestedFn     UpdaterService::s_on_stop_requested;
SERVICE_STATUS        UpdaterService::s_status{};
SERVICE_STATUS_HANDLE UpdaterService::s_status_handle = nullptr;
std::atomic<bool>     UpdaterService::s_stop_flag{false};

void UpdaterService::SetOnStopRequested(OnStopRequestedFn fn) {
    s_on_stop_requested = std::move(fn);
}

bool UpdaterService::Install(const std::wstring& exe_path) {
    std::wstring path = exe_path;
    if (path.empty()) {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        path = buf;
    }

    const std::wstring cmd = L"\"" + path + L"\" --service";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "[updater-service] ERROR: OpenSCManager failed (run as admin?), error=%lu\n",
                GetLastError());
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        kUpdaterServiceName,
        kUpdaterServiceDisplay,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmd.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!svc) {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_EXISTS) {
            fprintf(stderr, "[updater-service] ERROR: CreateService failed, error=%lu\n", err);
            CloseServiceHandle(scm);
            return false;
        }

        svc = OpenServiceW(scm, kUpdaterServiceName, SERVICE_ALL_ACCESS);
        if (!svc) {
            fprintf(stderr, "[updater-service] ERROR: OpenService failed for existing service, error=%lu\n",
                    GetLastError());
            CloseServiceHandle(scm);
            return false;
        }

        fprintf(stderr, "[updater-service] Service already installed. Refreshing configuration...\n");
    }

    const bool configured = ConfigureInstalledService(svc, cmd);
    if (configured) {
        fprintf(stderr, "[updater-service] Service installed/configured successfully.\n");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return configured;
}

bool UpdaterService::Uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        fprintf(stderr, "[updater-service] ERROR: OpenSCManager failed, error=%lu\n", GetLastError());
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, kUpdaterServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "[updater-service] ERROR: Service not found, error=%lu\n", GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        fprintf(stderr, "[updater-service] Stopping service...\n");
        for (int i = 0; i < 30; ++i) {
            QueryServiceStatus(svc, &status);
            if (status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
            Sleep(500);
        }
    }

    const BOOL ok = DeleteService(svc);
    if (ok) {
        fprintf(stderr, "[updater-service] Service uninstalled successfully.\n");
    } else {
        fprintf(stderr, "[updater-service] ERROR: DeleteService failed, error=%lu\n", GetLastError());
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok != FALSE;
}

bool UpdaterService::RunAsService(UpdaterEntryFn entry_fn) {
    s_entry_fn = std::move(entry_fn);
    s_stop_flag = false;

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kUpdaterServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "[updater-service] ERROR: StartServiceCtrlDispatcher failed, error=%lu\n",
                GetLastError());
        return false;
    }
    return true;
}

void WINAPI UpdaterService::ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    s_status_handle = RegisterServiceCtrlHandlerW(kUpdaterServiceName, ServiceCtrlHandler);
    if (!s_status_handle) {
        return;
    }

    s_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwCurrentState            = SERVICE_START_PENDING;
    s_status.dwControlsAccepted        = 0;
    s_status.dwWin32ExitCode           = 0;
    s_status.dwServiceSpecificExitCode = 0;
    s_status.dwCheckPoint              = 1;
    s_status.dwWaitHint                = 10000;
    SetServiceStatus(s_status_handle, &s_status);

    s_status.dwCurrentState     = SERVICE_RUNNING;
    s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    s_status.dwCheckPoint       = 0;
    s_status.dwWaitHint         = 0;
    SetServiceStatus(s_status_handle, &s_status);

    int exit_code = 0;
    if (s_entry_fn) {
        exit_code = s_entry_fn(s_stop_flag);
    }

    s_status.dwCurrentState = SERVICE_STOPPED;
    s_status.dwWin32ExitCode = (exit_code == 0) ? 0 : ERROR_SERVICE_SPECIFIC_ERROR;
    s_status.dwServiceSpecificExitCode = exit_code;
    SetServiceStatus(s_status_handle, &s_status);
}

void WINAPI UpdaterService::ServiceCtrlHandler(DWORD control) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            s_status.dwCurrentState = SERVICE_STOP_PENDING;
            s_status.dwCheckPoint   = 1;
            s_status.dwWaitHint     = 15000;
            SetServiceStatus(s_status_handle, &s_status);
            s_stop_flag = true;
            // Wake up the main loop so it can exit promptly.
            if (s_on_stop_requested) {
                s_on_stop_requested();
            }
            break;

        case kControlWakeup:
            // Custom control: trigger an immediate update check cycle.
            // The main loop will wake from sleep and run a check.
            if (s_on_stop_requested) {
                s_on_stop_requested();
            }
            break;

        default:
            break;
    }
}

} // namespace remotex::updater
