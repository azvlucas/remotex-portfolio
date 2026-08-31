#include "core/windows_shell_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <exdisp.h>
#include <objbase.h>
#include <servprov.h>
#include <shlobj.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>

namespace remotex::core {
namespace {

class ComInitScope {
public:
    ComInitScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        initialized_ = SUCCEEDED(hr);
        already_initialized_ = (hr == RPC_E_CHANGED_MODE);
    }

    ~ComInitScope() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool ok() const {
        return initialized_ || already_initialized_;
    }

private:
    bool initialized_ = false;
    bool already_initialized_ = false;
};

class HandleScope {
public:
    explicit HandleScope(HANDLE handle) : handle_(handle) {}

    ~HandleScope() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    [[nodiscard]] HANDLE get() const {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

template <typename T>
struct ComReleaser {
    void operator()(T* value) const {
        if (value != nullptr) {
            value->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser<T>>;

std::optional<std::filesystem::path> PathFromKnownFolder(REFKNOWNFOLDERID folder_id,
                                                         HANDLE token = nullptr) {
    PWSTR wide_path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, token, &wide_path);
    if (FAILED(hr) || wide_path == nullptr || wide_path[0] == L'\0') {
        if (wide_path != nullptr) {
            CoTaskMemFree(wide_path);
        }
        return std::nullopt;
    }

    const std::filesystem::path path(wide_path);
    CoTaskMemFree(wide_path);
    return path;
}

DWORD CurrentProcessSessionId() {
    DWORD session_id = 0xFFFFFFFF;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        return session_id;
    }
    return 0xFFFFFFFF;
}

DWORD PreferredInteractiveSessionId() {
    const DWORD current_session_id = CurrentProcessSessionId();
    if (current_session_id != 0 && current_session_id != 0xFFFFFFFF) {
        return current_session_id;
    }
    return WTSGetActiveConsoleSessionId();
}

std::optional<std::filesystem::path> PathFromInteractiveUserKnownFolder(REFKNOWNFOLDERID folder_id) {
    const DWORD session_id = PreferredInteractiveSessionId();
    if (session_id == 0xFFFFFFFF) {
        return std::nullopt;
    }

    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session_id, &token) || token == nullptr) {
        return std::nullopt;
    }
    const HandleScope token_guard(token);

    return PathFromKnownFolder(folder_id, token_guard.get());
}

std::optional<std::filesystem::path> SystemProfileDirectory() {
    wchar_t windows_dir[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH || windows_dir[0] == L'\0') {
        return std::nullopt;
    }

    return std::filesystem::path(windows_dir) / L"System32" / L"config" / L"systemprofile";
}

std::wstring NormalizePathKey(const std::filesystem::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && !key.empty() && key.back() == L'\\') {
        key.pop_back();
    }
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return key;
}

bool PathHasPrefixCaseInsensitive(const std::filesystem::path& path,
                                  const std::filesystem::path& prefix) {
    const std::wstring path_key = NormalizePathKey(path);
    const std::wstring prefix_key = NormalizePathKey(prefix);
    if (path_key.empty() || prefix_key.empty() || path_key.size() < prefix_key.size()) {
        return false;
    }
    if (path_key.compare(0, prefix_key.size(), prefix_key) != 0) {
        return false;
    }
    return path_key.size() == prefix_key.size() || path_key[prefix_key.size()] == L'\\';
}

std::optional<std::filesystem::path> ActiveUserProfileDirectory() {
    if (auto user_profile = PathFromInteractiveUserKnownFolder(FOLDERID_Profile); user_profile.has_value()) {
        return user_profile;
    }
    return PathFromKnownFolder(FOLDERID_Profile);
}

std::filesystem::path RemapResolvedPathToActiveUserProfile(const std::filesystem::path& resolved_path) {
    const auto system_profile_path = SystemProfileDirectory();
    const auto user_profile_path = ActiveUserProfileDirectory();
    if (!system_profile_path.has_value() || !user_profile_path.has_value()) {
        return resolved_path;
    }
    return detail::RemapSystemProfilePathToUserProfile(
        resolved_path,
        *system_profile_path,
        *user_profile_path);
}

bool MatchesForegroundExplorerWindow(HWND explorer_window, HWND foreground_window) {
    if (explorer_window == nullptr || foreground_window == nullptr) {
        return false;
    }

    const HWND root_foreground = GetAncestor(foreground_window, GA_ROOT);
    return root_foreground == explorer_window;
}

std::optional<std::filesystem::path> ExtractExplorerWindowPath(IDispatch* dispatch) {
    if (dispatch == nullptr) {
        return std::nullopt;
    }

    IWebBrowserApp* browser = nullptr;
    if (FAILED(dispatch->QueryInterface(IID_IWebBrowserApp, reinterpret_cast<void**>(&browser))) ||
        browser == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IWebBrowserApp> browser_guard(browser);

    SHANDLE_PTR raw_hwnd = 0;
    if (FAILED(browser->get_HWND(&raw_hwnd))) {
        return std::nullopt;
    }

    const HWND foreground_window = GetForegroundWindow();
    const HWND browser_window = reinterpret_cast<HWND>(raw_hwnd);
    if (!MatchesForegroundExplorerWindow(browser_window, foreground_window)) {
        return std::nullopt;
    }

    IServiceProvider* service_provider = nullptr;
    if (FAILED(dispatch->QueryInterface(IID_IServiceProvider,
                                        reinterpret_cast<void**>(&service_provider))) ||
        service_provider == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IServiceProvider> service_provider_guard(service_provider);

    IShellBrowser* shell_browser = nullptr;
    if (FAILED(service_provider->QueryService(SID_STopLevelBrowser,
                                              IID_IShellBrowser,
                                              reinterpret_cast<void**>(&shell_browser))) ||
        shell_browser == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IShellBrowser> shell_browser_guard(shell_browser);

    IShellView* shell_view = nullptr;
    if (FAILED(shell_browser->QueryActiveShellView(&shell_view)) || shell_view == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IShellView> shell_view_guard(shell_view);

    IFolderView* folder_view = nullptr;
    if (FAILED(shell_view->QueryInterface(IID_IFolderView,
                                          reinterpret_cast<void**>(&folder_view))) ||
        folder_view == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IFolderView> folder_view_guard(folder_view);

    IPersistFolder2* persist_folder = nullptr;
    if (FAILED(folder_view->GetFolder(IID_IPersistFolder2,
                                      reinterpret_cast<void**>(&persist_folder))) ||
        persist_folder == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IPersistFolder2> persist_folder_guard(persist_folder);

    PIDLIST_ABSOLUTE item_id_list = nullptr;
    if (FAILED(persist_folder->GetCurFolder(&item_id_list)) || item_id_list == nullptr) {
        return std::nullopt;
    }

    wchar_t path_buffer[MAX_PATH] = {};
    const bool converted = SHGetPathFromIDListW(item_id_list, path_buffer) == TRUE;
    CoTaskMemFree(item_id_list);
    if (!converted || path_buffer[0] == L'\0') {
        return std::nullopt;
    }

    return RemapResolvedPathToActiveUserProfile(std::filesystem::path(path_buffer));
}

} // namespace

namespace detail {

std::filesystem::path RemapSystemProfilePathToUserProfile(
    const std::filesystem::path& resolved_path,
    const std::filesystem::path& system_profile_path,
    const std::filesystem::path& user_profile_path) {
    if (resolved_path.empty() || system_profile_path.empty() || user_profile_path.empty()) {
        return resolved_path;
    }

    if (!PathHasPrefixCaseInsensitive(resolved_path, system_profile_path)) {
        return resolved_path;
    }

    const std::wstring resolved_key = NormalizePathKey(resolved_path);
    const std::wstring system_profile_key = NormalizePathKey(system_profile_path);
    if (resolved_key.size() <= system_profile_key.size()) {
        return user_profile_path;
    }

    std::wstring suffix = resolved_key.substr(system_profile_key.size());
    while (!suffix.empty() && suffix.front() == L'\\') {
        suffix.erase(suffix.begin());
    }
    if (suffix.empty()) {
        return user_profile_path;
    }

    return user_profile_path / std::filesystem::path(suffix);
}

} // namespace detail

std::optional<std::filesystem::path> DetectActiveExplorerDirectory() {
    ComInitScope com_scope;
    if (!com_scope.ok()) {
        return std::nullopt;
    }

    IShellWindows* shell_windows = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_ShellWindows,
                                        nullptr,
                                        CLSCTX_ALL,
                                        IID_IShellWindows,
                                        reinterpret_cast<void**>(&shell_windows));
    if (FAILED(hr) || shell_windows == nullptr) {
        return std::nullopt;
    }
    const ComPtr<IShellWindows> shell_windows_guard(shell_windows);

    long count = 0;
    if (FAILED(shell_windows->get_Count(&count))) {
        return std::nullopt;
    }

    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        IDispatch* dispatch = nullptr;
        const HRESULT item_hr = shell_windows->Item(index, &dispatch);
        VariantClear(&index);
        if (FAILED(item_hr) || dispatch == nullptr) {
            continue;
        }
        const ComPtr<IDispatch> dispatch_guard(dispatch);

        if (auto path = ExtractExplorerWindowPath(dispatch); path.has_value()) {
            return path;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> GetDesktopDirectory() {
    if (auto desktop_path = PathFromInteractiveUserKnownFolder(FOLDERID_Desktop); desktop_path.has_value()) {
        return desktop_path;
    }

    if (auto desktop_path = PathFromKnownFolder(FOLDERID_Desktop); desktop_path.has_value()) {
        return RemapResolvedPathToActiveUserProfile(*desktop_path);
    }

    return std::nullopt;
}

ClipboardPasteTarget ResolveExplorerDirectoryOrDesktop() {
    ClipboardPasteTarget target;

    if (auto explorer_path = DetectActiveExplorerDirectory(); explorer_path.has_value()) {
        target.path = *explorer_path;
        target.resolved_from_explorer = true;
        return target;
    }

    if (auto desktop_path = GetDesktopDirectory(); desktop_path.has_value()) {
        target.path = *desktop_path;
        target.used_desktop_fallback = true;
    }

    return target;
}

} // namespace remotex::core
