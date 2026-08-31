static std::string TrimTrailingWhitespace(std::string value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

static std::string FormatWindowsErrorMessage(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageA(flags,
                                      nullptr,
                                      error,
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPSTR>(&buffer),
                                      0,
                                      nullptr);
    if (size == 0 || !buffer) {
        return {};
    }

    std::string message(buffer, size);
    LocalFree(buffer);
    return TrimTrailingWhitespace(std::move(message));
}

static std::string DescribeWinHttpError(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT:
            return "timeout";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "name not resolved";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "cannot connect";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "connection error";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return "TLS secure failure";
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
            return "client certificate required";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
            return "invalid server response";
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            return "operation cancelled";
        case ERROR_WINHTTP_AUTODETECTION_FAILED:
            return "proxy autodetection failed";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
            return "unrecognized URL scheme";
        default:
            break;
    }

    const std::string message = FormatWindowsErrorMessage(error);
    if (!message.empty()) {
        return message;
    }
    return "WinHTTP error " + std::to_string(error);
}

static std::string DescribeSecureFailureFlags(DWORD flags) {
    std::vector<std::string> issues;
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED) != 0) {
        issues.emplace_back("cert revocation check failed");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT) != 0) {
        issues.emplace_back("invalid cert");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED) != 0) {
        issues.emplace_back("cert revoked");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA) != 0) {
        issues.emplace_back("invalid CA");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID) != 0) {
        issues.emplace_back("cert CN invalid");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID) != 0) {
        issues.emplace_back("cert date invalid");
    }
    if ((flags & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR) != 0) {
        issues.emplace_back("security channel error");
    }

    if (issues.empty()) {
        return {};
    }

    std::string joined;
    for (size_t i = 0; i < issues.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += issues[i];
    }
    return joined;
}

static bool IsRevocationOfflineFailure(DWORD winhttp_error, DWORD secure_failure_flags) {
    if (winhttp_error == ERROR_WINHTTP_SECURE_CERT_REV_FAILED) {
        return true;
    }
    return winhttp_error == ERROR_WINHTTP_SECURE_FAILURE &&
           (secure_failure_flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED) != 0;
}

static void CALLBACK HostWinHttpStatusCallback(HINTERNET,
                                               DWORD_PTR context,
                                               DWORD status,
                                               LPVOID info,
                                               DWORD info_len) {
    if (status != WINHTTP_CALLBACK_STATUS_SECURE_FAILURE ||
        context == 0 ||
        info == nullptr ||
        info_len < sizeof(DWORD)) {
        return;
    }

    *reinterpret_cast<DWORD*>(context) = *static_cast<DWORD*>(info);
}

static SimpleHttpResponse HttpPostJson(const HostConnectionSettings& settings,
                                       const std::string& path,
                                       const std::string& body) {
    SimpleHttpResponse result;

    HINTERNET session = WinHttpOpen(L"RemoteX-Host/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error_stage = "WinHttpOpen";
        result.winhttp_error = GetLastError();
        result.error_detail = DescribeWinHttpError(result.winhttp_error);
        return result;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 15000);

    HINTERNET conn = WinHttpConnect(session, ToWide(settings.api_host).c_str(), settings.api_port, 0);
    if (!conn) {
        result.error_stage = "WinHttpConnect";
        result.winhttp_error = GetLastError();
        result.error_detail = DescribeWinHttpError(result.winhttp_error);
        WinHttpCloseHandle(session);
        return result;
    }

    auto perform_request = [&](bool ignore_revocation_offline) -> SimpleHttpResponse {
        SimpleHttpResponse attempt;
        DWORD secure_failure_flags = 0;

        const DWORD flags = settings.api_use_tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"POST", ToWide(path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            attempt.error_stage = "WinHttpOpenRequest";
            attempt.winhttp_error = GetLastError();
            attempt.error_detail = DescribeWinHttpError(attempt.winhttp_error);
            return attempt;
        }

        WinHttpSetStatusCallback(req,
                                 HostWinHttpStatusCallback,
                                 WINHTTP_CALLBACK_FLAG_SECURE_FAILURE,
                                 0);

        if (ignore_revocation_offline) {
            DWORD option = TRUE;
            WinHttpSetOption(req,
                             WINHTTP_OPTION_IGNORE_CERT_REVOCATION_OFFLINE,
                             &option,
                             sizeof(option));
        }

        WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     const_cast<char*>(body.data()),
                                     static_cast<DWORD>(body.size()),
                                     static_cast<DWORD>(body.size()),
                                     reinterpret_cast<DWORD_PTR>(&secure_failure_flags));
        if (ok) {
            ok = WinHttpReceiveResponse(req, nullptr);
            if (!ok) {
                attempt.error_stage = "WinHttpReceiveResponse";
                attempt.winhttp_error = GetLastError();
                attempt.error_detail = DescribeWinHttpError(attempt.winhttp_error);
            }
        } else {
            attempt.error_stage = "WinHttpSendRequest";
            attempt.winhttp_error = GetLastError();
            attempt.error_detail = DescribeWinHttpError(attempt.winhttp_error);
        }

        attempt.secure_failure_flags = secure_failure_flags;
        if ((attempt.winhttp_error == ERROR_WINHTTP_SECURE_FAILURE ||
             attempt.winhttp_error == ERROR_WINHTTP_SECURE_CERT_REV_FAILED) &&
            secure_failure_flags != 0) {
            const std::string flags_desc = DescribeSecureFailureFlags(secure_failure_flags);
            if (!flags_desc.empty()) {
                attempt.error_detail += " [" + flags_desc + "]";
            }
        }

        if (ok) {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            attempt.status_code = static_cast<int>(status);

            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, 0);
                DWORD read = 0;
                WinHttpReadData(req, chunk.data(), avail, &read);
                attempt.body.append(chunk.data(), read);
            }
            attempt.used_revocation_offline_fallback = ignore_revocation_offline;
        } else if (ignore_revocation_offline) {
            attempt.used_revocation_offline_fallback = true;
        }

        WinHttpCloseHandle(req);
        return attempt;
    };

    result = perform_request(false);
    if (settings.api_use_tls &&
        IsRevocationOfflineFailure(result.winhttp_error, result.secure_failure_flags)) {
        // Keep this fallback narrowly scoped to revocation-offline failures.
        result = perform_request(true);
    }

    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

static EnrollmentResponse EnrollWithBootstrap(const BootstrapFile& bootstrap,
                                              const std::string& machine_name,
                                              const std::string& os_info) {
    EnrollmentResponse result;

    const std::string body =
        "{\"enrollment_token\":\"" + EscapeJson(bootstrap.enrollment_token) +
        "\",\"machine_name\":\"" + EscapeJson(machine_name) +
        "\",\"os_info\":\"" + EscapeJson(os_info) + "\"}";

    const auto response = HttpPostJson(bootstrap.settings, "/host/enroll", body);
    result.status_code = response.status_code;
    result.used_revocation_offline_fallback = response.used_revocation_offline_fallback;
    if (response.status_code != 201) {
        result.error = JsonGetString(response.body, "error");
        if (result.error.empty()) {
            result.error = response.status_code == 0
                ? ("network error during " + response.error_stage +
                   " (" + response.error_detail + ") [host=" + bootstrap.settings.api_host +
                   ":" + std::to_string(bootstrap.settings.api_port) +
                   ", tls=" + (bootstrap.settings.api_use_tls ? "true" : "false") + "]")
                : ("HTTP " + std::to_string(response.status_code));
        }
        result.retryable = response.status_code == 0 || response.status_code >= 500;
        return result;
    }

    result.success = true;
    result.machine_id = JsonGetString(response.body, "machine_id");
    result.machine_key = JsonGetString(response.body, "machine_key");
    result.access_token = JsonGetString(response.body, "access_token");
    result.refresh_token = JsonGetString(response.body, "refresh_token");
    if (result.machine_id.empty() || result.machine_key.empty() || result.access_token.empty() || result.refresh_token.empty()) {
        result.success = false;
        result.error = "invalid enrollment response";
    }
    return result;
}

static uint64_t GetUnixTimeSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

static void SaveRegDword(const char* name, DWORD value) {
    remotex::host::detail::WriteRegistryDwordValue(HKEY_LOCAL_MACHINE, kRegKey, name, value);
}

static DWORD LoadRegDword(const char* name, DWORD fallback) {
    return remotex::host::detail::ReadRegistryDwordValue(HKEY_LOCAL_MACHINE, kRegKey, name, fallback);
}

static void SaveRegQword(const char* name, uint64_t value) {
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExA(hkey, name, 0, REG_QWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   static_cast<DWORD>(sizeof(value)));
    RegCloseKey(hkey);
}

static uint64_t LoadRegQword(const char* name, uint64_t fallback = 0) {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return fallback;
    uint64_t value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_QWORD;
    if (RegQueryValueExA(hkey, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_QWORD) {
        RegCloseKey(hkey);
        return fallback;
    }
    RegCloseKey(hkey);
    return value;
}

static void ClearStoredCredentialTarget(const char* target, std::vector<std::string> legacy_targets = {}) {
    remotex::core::AuthClient auth;
    remotex::core::AuthConfig auth_cfg;
    auth_cfg.credential_target = target;
    auth_cfg.legacy_credential_targets = std::move(legacy_targets);
    auth.Configure(auth_cfg);
    auth.ClearStoredTokens();
}

static bool DeleteHostRegistryState(const wchar_t* subkey, REGSAM view_flags) {
    const LONG status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, subkey, view_flags, 0);
    return status == ERROR_SUCCESS ||
           status == ERROR_FILE_NOT_FOUND ||
           status == ERROR_PATH_NOT_FOUND ||
           status == ERROR_INVALID_PARAMETER;
}

static void ClearPersistedHostState() {
    ClearStoredCredentialTarget(kMachineCredentialTarget, {kLegacyMachineCredentialTarget});
    ClearStoredCredentialTarget(kUserCredentialTarget, {kLegacyUserCredentialTarget});

    DeleteBootstrapFile();
    DeleteCredentialsFile();

    const bool cleared_new_32 = DeleteHostRegistryState(kRegKey, KEY_WOW64_32KEY);
    const bool cleared_new_64 = DeleteHostRegistryState(kRegKey, KEY_WOW64_64KEY);
    const bool cleared_legacy_32 = DeleteHostRegistryState(kLegacyRegKey, KEY_WOW64_32KEY);
    const bool cleared_legacy_64 = DeleteHostRegistryState(kLegacyRegKey, KEY_WOW64_64KEY);
    if ((!cleared_new_32 && !cleared_new_64) || (!cleared_legacy_32 && !cleared_legacy_64)) {
        Log("WARNING: Failed to remove persisted host registry state from HKLM\\SOFTWARE\\RemoteX\\Host or HKLM\\SOFTWARE\\RemoteAccess\\Host");
    }
}

static void ClearPersistedMachineIdentityRegistryValues() {
    SaveRegString("machine_id", "");
    SaveRegString("machine_key", "");
    remotex::host::detail::WriteRegistryStringValue(HKEY_LOCAL_MACHINE, kLegacyRegKey, "machine_id", "");
    remotex::host::detail::WriteRegistryStringValue(HKEY_LOCAL_MACHINE, kLegacyRegKey, "machine_key", "");
}

static void ClearPersistedMachineIdentity(remotex::core::AuthClient* auth = nullptr) {
    if (auth) {
        auth->Logout();
    } else {
        ClearStoredCredentialTarget(kMachineCredentialTarget, {kLegacyMachineCredentialTarget});
    }

    ClearPersistedMachineIdentityRegistryValues();
}

static bool IsTerminalMachineSessionError(const std::string& error) {
    return error == "machine not found" ||
           error == "invalid refresh token" ||
           error == "refresh token expired, please login again" ||
           error == "user not found" ||
           error == "no refresh token available";
}

static void RequestPersistedHostStateClearOnExit() {
    SaveRegDword(kRegClearStateOnExit, 1);
}

static bool ShouldClearPersistedHostStateOnExit() {
    return LoadRegDword(kRegClearStateOnExit, 0) != 0;
}

static void SaveWorkerHeartbeatState(DWORD session_id, DWORD pid) {
    SaveRegQword(kRegWorkerHeartbeatUtc, GetUnixTimeSeconds());
    SaveRegDword(kRegWorkerSessionId, session_id);
    SaveRegDword(kRegWorkerPid, pid);
}

static void SaveWorkerSignalingState(bool connected) {
    SaveRegQword(kRegWorkerSignalingUtc, GetUnixTimeSeconds());
    SaveRegDword(kRegWorkerSignalingUp, connected ? 1u : 0u);
}

static void ResetWorkerState() {
    SaveRegQword(kRegWorkerHeartbeatUtc, 0);
    SaveRegDword(kRegWorkerSessionId, 0);
    SaveRegDword(kRegWorkerPid, 0);
    SaveRegQword(kRegWorkerSignalingUtc, 0);
    SaveRegDword(kRegWorkerSignalingUp, 0);
}

class WorkerHeartbeatReporter {
public:
    void Start(DWORD session_id) {
        if (running_.exchange(true)) return;
        stop_requested_.store(false);
        session_id_ = session_id;
        thread_ = std::thread([this] {
            while (!stop_requested_.load()) {
                SaveWorkerHeartbeatState(session_id_, GetCurrentProcessId());
                for (int i = 0; i < 20 && !stop_requested_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        });
    }

    void Stop() {
        if (!running_.load()) return;
        stop_requested_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        running_.store(false);
    }

    ~WorkerHeartbeatReporter() {
        Stop();
    }

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    DWORD session_id_ = 0;
};

// ─── Obter informações do SO ────────────────────────────────────────────────

static std::string GetOSInfo() {
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(&vi);
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu",
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    return std::string(buf);
}

static std::string GetHostname() {
    char buf[256]{};
    DWORD size = sizeof(buf);
    GetComputerNameA(buf, &size);
    return std::string(buf);
}

// ─── Spawn do processo filho na sessão interativa (Sessão 1+) ───────────────

static bool SpawnDesktopProcess(DWORD session_id, HANDLE& out_process) {
    HANDLE hToken = NULL;
    // Usa o token do próprio processo (SYSTEM)
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken)) {
        Log("ERROR: OpenProcessToken failed (err=%lu)", GetLastError());
        return false;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDupToken)) {
        Log("ERROR: DuplicateTokenEx failed (err=%lu)", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    // Altera o ID da Sessão do token para a sessão do desktop interativo
    if (!SetTokenInformation(hDupToken, TokenSessionId, &session_id, sizeof(DWORD))) {
        Log("ERROR: SetTokenInformation failed to set Session %lu (err=%lu)", session_id, GetLastError());
        CloseHandle(hDupToken);
        CloseHandle(hToken);
        return false;
    }

    void* lpEnvironment = NULL;
    if (!CreateEnvironmentBlock(&lpEnvironment, hDupToken, FALSE)) {
        Log("WARNING: CreateEnvironmentBlock failed (err=%lu)", GetLastError());
        lpEnvironment = NULL;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --desktop";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi = { 0 };

    bool result = CreateProcessAsUserW(
        hDupToken,
        exePath,
        cmdLine.data(),
        NULL,
        NULL,
        FALSE,
        NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        lpEnvironment,
        NULL,
        &si,
        &pi
    );

    if (lpEnvironment) DestroyEnvironmentBlock(lpEnvironment);
    CloseHandle(hDupToken);
    CloseHandle(hToken);

    if (result) {
        out_process = pi.hProcess;
        CloseHandle(pi.hThread);
        Log("Desktop worker spawned in session %lu (PID=%lu)", session_id, pi.dwProcessId);
    } else {
        Log("ERROR: CreateProcessAsUserW failed, err=%lu", GetLastError());
    }

    return result;
}

static void ConfigureAuthClient(remotex::core::AuthClient& auth,
                                const std::string& credential_target,
                                std::vector<std::string> legacy_targets = {}) {
    remotex::core::AuthConfig auth_cfg;
    auth_cfg.api_host = g_connection_settings.api_host;
    auth_cfg.api_port = g_connection_settings.api_port;
    auth_cfg.use_tls  = g_connection_settings.api_use_tls;
    auth_cfg.credential_target = credential_target;
    auth_cfg.legacy_credential_targets = std::move(legacy_targets);
    auth.Configure(auth_cfg);
}

static bool AuthenticateHost(remotex::core::AuthClient& auth,
                             bool is_service,
                             bool is_desktop,
                             const std::string& cli_email,
                             const std::string& cli_password,
                             bool& authenticated_as_machine) {
    authenticated_as_machine = false;
    g_connection_settings = LoadConnectionSettings();

    if (auto bootstrap = LoadBootstrapFile()) {
        const std::string previous_machine_id = LoadMachineId();
        if (!previous_machine_id.empty()) {
            Log("Bootstrap file present - replacing persisted machine identity %s",
                remotex::core::RedactIdentifier(previous_machine_id, "machine").c_str());
        } else {
            Log("Bootstrap file present - enrolling machine from installer bundle");
        }

        g_connection_settings = bootstrap->settings;
        SaveConnectionSettings(g_connection_settings);
        ConfigureAuthClient(auth, kMachineCredentialTarget, {kLegacyMachineCredentialTarget});
        Log("Bootstrap enrollment target configured: api_host=%s api_port=%u tls=%d",
            remotex::core::RedactIdentifier(g_connection_settings.api_host, "api_host").c_str(),
            g_connection_settings.api_port,
            g_connection_settings.api_use_tls ? 1 : 0);

        for (;;) {
            const auto enrollment = EnrollWithBootstrap(*bootstrap, GetHostname(), GetOSInfo());
            if (enrollment.success) {
                auth.SetTokensDirect(enrollment.machine_id, enrollment.access_token, enrollment.refresh_token);
                auth.SaveTokens();
                SaveMachineId(enrollment.machine_id);
                SaveMachineKey(enrollment.machine_key);
                DeleteBootstrapFile();
                authenticated_as_machine = true;
                if (enrollment.used_revocation_offline_fallback) {
                    Log("WARNING: Bootstrap enrollment succeeded with revocation-offline TLS fallback.");
                }
                Log("Host enrolled successfully as machine %s",
                    remotex::core::RedactIdentifier(enrollment.machine_id, "machine").c_str());
                return true;
            }

            Log("ERROR: Host enrollment failed: %s", enrollment.error.c_str());
            if (!(is_service || is_desktop) || !enrollment.retryable) {
                if (is_service || is_desktop) {
                    return false;
                }
                break;
            }

            static constexpr auto kBootstrapRetryDelay = std::chrono::seconds(10);
            Log("Retrying bootstrap enrollment in %llds", static_cast<long long>(kBootstrapRetryDelay.count()));
            for (int second = 0; second < kBootstrapRetryDelay.count(); ++second) {
                if (!IsRunning()) {
                    return false;
                }
                Sleep(1000);
            }
        }
    }

    ConfigureAuthClient(auth, kMachineCredentialTarget, {kLegacyMachineCredentialTarget});
    if (auth.LoadStoredTokens() && !LoadMachineId().empty()) {
        const std::string persisted_machine_id = auth.GetUserID().empty()
            ? LoadMachineId()
            : auth.GetUserID();
        const bool has_access_token = auth.IsAuthenticated();
        const bool has_refresh_token = !auth.GetRefreshToken().empty();

        if (has_refresh_token) {
            const auto refresh = auth.RefreshToken();
            if (refresh.success) {
                authenticated_as_machine = true;
                Log("Loaded saved machine session for %s",
                    remotex::core::RedactIdentifier(auth.GetUserID(), "machine").c_str());
                return true;
            }

            if (IsTerminalMachineSessionError(refresh.error)) {
                Log("Persisted machine session for %s is no longer valid: %s",
                    remotex::core::RedactIdentifier(persisted_machine_id, "machine").c_str(),
                    refresh.error.c_str());
                ClearPersistedMachineIdentity(&auth);
                Log("Cleared persisted machine identity. Falling back to bootstrap or user credentials.");
            } else if (has_access_token) {
                authenticated_as_machine = true;
                Log("Loaded saved machine session for %s",
                    remotex::core::RedactIdentifier(persisted_machine_id, "machine").c_str());
                Log("WARNING: Could not refresh persisted machine session (%s). Continuing with saved access token.",
                    refresh.error.c_str());
                return true;
            } else {
                Log("WARNING: Could not refresh persisted machine session for %s: %s",
                    remotex::core::RedactIdentifier(persisted_machine_id, "machine").c_str(),
                    refresh.error.c_str());
            }
        } else if (has_access_token) {
            authenticated_as_machine = true;
            Log("Loaded saved machine session for %s",
                remotex::core::RedactIdentifier(persisted_machine_id, "machine").c_str());
            Log("WARNING: Persisted machine session has no refresh token; continuing with saved access token.");
            return true;
        }
    }

    ConfigureAuthClient(auth, kUserCredentialTarget, {kLegacyUserCredentialTarget});

    bool auth_from_file = false;
    if (auto creds = LoadCredentialsFile()) {
        if (creds->has_tokens()) {
            auth.SetTokensDirect(creds->user_id, creds->access_token, creds->refresh_token);
            auth.SaveTokens();
            Log("Credentials injected from file for user %s",
                remotex::core::RedactIdentifier(creds->user_id, "user").c_str());
            auth_from_file = true;
        } else if (creds->has_credentials()) {
            Log("Logging in from credentials file as %s...",
                remotex::core::RedactEmail(creds->email).c_str());
            auto result = auth.Login(creds->email, creds->password);
            if (result.success) {
                auth.SaveTokens();
                Log("Authenticated from file as %s",
                    remotex::core::RedactIdentifier(result.user_id, "user").c_str());
                auth_from_file = true;
            } else {
                Log("ERROR: Login from credentials file failed: %s", result.error.c_str());
            }
        }
    }

    if (!auth_from_file && auth.LoadStoredTokens() && auth.IsAuthenticated()) {
        Log("Loaded saved session for user %s",
            remotex::core::RedactIdentifier(auth.GetUserID(), "user").c_str());
        return true;
    }

    if (auth_from_file) {
        return true;
    }

    std::string email = cli_email;
    std::string password = cli_password;

    if (email.empty()) {
        if (is_service || is_desktop) {
            Log("ERROR: No machine enrollment or user credentials available. Use bootstrap.json or --email/--password.");
            return false;
        }
        printf("Email: ");
        fflush(stdout);
        char buf[256]{};
        if (!fgets(buf, sizeof(buf), stdin) || buf[0] == '\n') {
            Log("ERROR: Email required");
            return false;
        }
        email = buf;
        while (!email.empty() && (email.back() == '\n' || email.back() == '\r'))
            email.pop_back();
    }

    if (password.empty()) {
        if (is_service || is_desktop) {
            Log("ERROR: No machine enrollment or user credentials available. Use bootstrap.json or --email/--password.");
            return false;
        }
        printf("Password: ");
        fflush(stdout);
        char buf[256]{};
        if (!fgets(buf, sizeof(buf), stdin) || buf[0] == '\n') {
            Log("ERROR: Password required");
            return false;
        }
        password = buf;
        while (!password.empty() && (password.back() == '\n' || password.back() == '\r'))
            password.pop_back();
    }

    Log("Logging in as %s...",
        remotex::core::RedactEmail(email).c_str());
    auto result = auth.Login(email, password);
    if (!result.success) {
        Log("ERROR: Login failed: %s", result.error.c_str());
        Log("Hint: provision the account in the Viewer or export a new installer.");
        if (!is_service && !is_desktop) {
            printf("Press Enter to exit...\n");
            getchar();
        }
        return false;
    }

    Log("Authenticated as %s",
        remotex::core::RedactIdentifier(result.user_id, "user").c_str());
    auth.SaveTokens();
    return true;
}

static bool EnsureMachineRegistration(remotex::core::AuthClient& auth,
                                      remotex::core::MachineClient& machines,
                                      bool is_service,
                                      bool is_desktop,
                                      bool authenticated_as_machine,
                                      std::string& machine_id,
                                      std::string& machine_key) {
    machine_id = LoadMachineId();
    machine_key = LoadMachineKey();

    if (authenticated_as_machine) {
        if (machine_id.empty() || machine_key.empty()) {
            Log("ERROR: Machine credentials loaded but machine identity is incomplete.");
            return false;
        }
        Log("Machine online with persisted identity: %s",
            remotex::core::RedactIdentifier(machine_id, "machine").c_str());
        return true;
    }

    if (!machine_id.empty()) {
        if (!auth.IsAuthenticated()) {
            Log("Token may be expired, attempting refresh...");
            auth.RefreshToken();
        }

        auto result = machines.GetMachineStatusEx(machine_id);
        if (!result.status && result.http_code == 404) {
            Log("Machine %s deleted from server (404), re-registering...",
                remotex::core::RedactIdentifier(machine_id, "machine").c_str());
            machine_id.clear();
            machine_key.clear();
        } else if (!result.status) {
            if (result.http_code == 401) {
                Log("Token expired (401), attempting refresh...");
                if (auth.RefreshToken().success) {
                    Log("Token refreshed OK");
                } else {
                    Log("WARNING: Token refresh failed — signaling will likely reject connection");
                }
            }
            Log("WARNING: Could not verify machine %s (HTTP %d), using saved ID",
                remotex::core::RedactIdentifier(machine_id, "machine").c_str(), result.http_code);
        }
    }

    if (machine_id.empty()) {
        if (!auth.IsAuthenticated()) {
            Log("Token may be expired, attempting refresh...");
            auth.RefreshToken();
        }

        auto info = machines.RegisterMachine(GetHostname(), GetOSInfo());
        if (!info) {
            Log("Register failed, refreshing token and retrying...");
            if (auth.RefreshToken().success) {
                info = machines.RegisterMachine(GetHostname(), GetOSInfo());
            }
        }
        if (!info) {
            Log("ERROR: Failed to register machine (check backend logs)");
            Log("Hint: try deleting saved credentials and logging in again");
            Log("      reg delete HKLM\\SOFTWARE\\RemoteX\\Host /f");
            Log("      reg delete HKLM\\SOFTWARE\\RemoteAccess\\Host /f");
            if (!is_service && !is_desktop) {
                printf("Press Enter to exit...\n");
                getchar();
            }
            return false;
        }
        machine_id  = info->id;
        machine_key = info->machine_key;
        SaveMachineId(machine_id);
        SaveMachineKey(machine_key);
        Log("Machine registered: %s",
            remotex::core::RedactIdentifier(machine_id, "machine").c_str());
    } else {
        Log("Machine online: %s",
            remotex::core::RedactIdentifier(machine_id, "machine").c_str());
    }

    return true;
}
