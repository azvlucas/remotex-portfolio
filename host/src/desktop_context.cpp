// RemoteX - host/desktop_context.cpp
// Inspeciona nomes de desktop para diferenciar mudancas de desktop seguro
// de falhas genericas de captura.

#include "host/desktop_context.h"

#include <string>

namespace {

bool QueryDesktopName(HDESK desktop, std::string& name, DWORD& error) {
    name.clear();
    error = ERROR_SUCCESS;
    if (!desktop) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }

    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &needed)) {
        const DWORD probe_error = GetLastError();
        if (probe_error != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
            error = probe_error;
            return false;
        }
    }

    if (needed == 0) {
        error = ERROR_GEN_FAILURE;
        return false;
    }

    std::wstring wide_name(needed / sizeof(wchar_t), L'\0');
    if (!GetUserObjectInformationW(desktop,
                                   UOI_NAME,
                                   wide_name.data(),
                                   needed,
                                   &needed)) {
        error = GetLastError();
        return false;
    }

    const size_t terminator = wide_name.find(L'\0');
    if (terminator != std::wstring::npos) {
        wide_name.resize(terminator);
    }

    const int utf8_size = WideCharToMultiByte(CP_UTF8,
                                              0,
                                              wide_name.c_str(),
                                              static_cast<int>(wide_name.size()),
                                              nullptr,
                                              0,
                                              nullptr,
                                              nullptr);
    if (utf8_size <= 0) {
        error = GetLastError();
        return false;
    }

    name.resize(static_cast<size_t>(utf8_size));
    const int converted = WideCharToMultiByte(CP_UTF8,
                                              0,
                                              wide_name.c_str(),
                                              static_cast<int>(wide_name.size()),
                                              name.data(),
                                              utf8_size,
                                              nullptr,
                                              nullptr);
    if (converted != utf8_size) {
        error = GetLastError();
        name.clear();
        return false;
    }

    return true;
}

} // namespace

namespace remotex::host {

DesktopContextSnapshot CaptureDesktopContext() {
    DesktopContextSnapshot snapshot;

    std::string thread_name;
    if (QueryDesktopName(GetThreadDesktop(GetCurrentThreadId()),
                         thread_name,
                         snapshot.thread_error)) {
        snapshot.thread_desktop_name = std::move(thread_name);
        snapshot.thread_desktop_available = true;
        snapshot.thread_error = ERROR_SUCCESS;
    }

    HDESK input_desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!input_desktop) {
        snapshot.input_error = GetLastError();
        return snapshot;
    }

    std::string input_name;
    if (QueryDesktopName(input_desktop, input_name, snapshot.input_error)) {
        snapshot.input_desktop_name = std::move(input_name);
        snapshot.input_desktop_available = true;
        snapshot.input_error = ERROR_SUCCESS;
    }
    CloseDesktop(input_desktop);
    return snapshot;
}

bool SwitchCurrentThreadToInputDesktop(DWORD& error,
                                       DesktopContextSnapshot* snapshot_after_switch) {
    error = ERROR_SUCCESS;

    const DesktopContextSnapshot current = CaptureDesktopContext();
    if (current.HasCompleteContext() && current.InputMatchesThread()) {
        if (snapshot_after_switch != nullptr) {
            *snapshot_after_switch = current;
        }
        return true;
    }

    constexpr ACCESS_MASK kDesktopAccess =
        DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP;
    HDESK input_desktop = OpenInputDesktop(0, FALSE, kDesktopAccess);
    if (!input_desktop) {
        error = GetLastError();
        return false;
    }

    const BOOL switched = SetThreadDesktop(input_desktop);
    error = switched ? ERROR_SUCCESS : GetLastError();
    CloseDesktop(input_desktop);
    if (!switched) {
        return false;
    }

    if (snapshot_after_switch != nullptr) {
        *snapshot_after_switch = CaptureDesktopContext();
    }
    return true;
}

} // namespace remotex::host
