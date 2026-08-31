#pragma once
// RemoteX - host/desktop_context.h
// Helpers para inspecionar o desktop ativo da thread e o desktop de entrada do Windows.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace remotex::host {

struct DesktopContextSnapshot {
    std::string input_desktop_name;
    std::string thread_desktop_name;
    DWORD input_error = ERROR_SUCCESS;
    DWORD thread_error = ERROR_SUCCESS;
    bool input_desktop_available = false;
    bool thread_desktop_available = false;

    bool HasAnyDesktop() const {
        return input_desktop_available || thread_desktop_available;
    }

    bool HasCompleteContext() const {
        return input_desktop_available && thread_desktop_available;
    }

    bool InputMatchesThread() const {
        return input_desktop_available && thread_desktop_available &&
               input_desktop_name == thread_desktop_name;
    }
};

DesktopContextSnapshot CaptureDesktopContext();
bool SwitchCurrentThreadToInputDesktop(DWORD& error,
                                       DesktopContextSnapshot* snapshot_after_switch = nullptr);

} // namespace remotex::host
