// RemoteX â€” host/main.cpp
// Entry point do agente Host.
//
// Modos de execuÃ§Ã£o:
//   --install    â†’ registra o Windows Service e sai
//   --uninstall  â†’ remove o Windows Service e sai
//   --service    â†’ entry point do SCM (RunAsService)
//   (sem args)   â†’ modo console (debug)
//
// Fluxo (RunHost):
//   1. Auth (login com credenciais salvas ou interativo)
//   2. Registrar mÃ¡quina + heartbeat
//   3. Iniciar captura de tela
//   4. Conectar ao signaling, aguardar viewer
//   5. Trocar SDP/ICE via signaling â†’ WebRTC
//   6. Enviar frames de vÃ­deo, receber input remoto
//   7. File transfer + backup agent + auto-updater em background

#include "core/auth_client.h"
#include "core/machine_client.h"
#include "core/signaling_client.h"
#include "core/webrtc_peer.h"
#include "core/file_transfer.h"
#include "core/turn_client.h"
#include "core/input_protocol.h"
#include "core/log_redaction.h"

#include "host/screen_capture.h"
#include "host/input_injector.h"
#include "host/heartbeat.h"
#include "host/backup_agent.h"
#include "host/recording_utils.h"
#include "host/recording_finalize.h"
#include "host/session_recorder.h"
#include "host/recording_uploader.h"
#include "host/capture_cadence.h"
#include "host/frame_rotator.h"
#include "host/gdi_desktop_snapshot.h"
#include "host/session_instrumentation.h"
#include "host/connection_settings_store.h"
#include "host/rotation_geometry.h"
#include "host/service_manager.h"
#include "host/chat_window.h"
#include "host/clipboard_sync.h"
#include "core/windows_shell_utils.h"
#include "host/privacy_screen.h"
#include "host/worker_watchdog.h"
#include "host/runtime_paths.h"

#include <windows.h>
#include <winhttp.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <dbghelp.h>
#include <reason.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <crtdbg.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <array>
#include <map>

#include <nlohmann/json.hpp>

static constexpr wchar_t kSecureAttentionPipeName[] = L"\\\\.\\pipe\\RemoteXSecureAttention";
static constexpr wchar_t kSecureDesktopInputPipePrefix[] = L"\\\\.\\pipe\\RemoteXSecureDesktopInput-";

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

#ifndef IDTIMEOUT
#define IDTIMEOUT 32000
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// â”€â”€â”€ Constantes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

using HostConnectionSettings = remotex::host::detail::HostConnectionSettings;

static constexpr const char* kVersion                 = REMOTEX_HOST_VERSION;
static constexpr const char* kMachineCredentialTarget = "RemoteXHostMachine";
static constexpr const char* kUserCredentialTarget    = "RemoteXHostUser";
static constexpr const char* kLegacyMachineCredentialTarget = "RemoteAccessHostMachine";
static constexpr const char* kLegacyUserCredentialTarget    = "RemoteAccessHostUser";
static constexpr int         kCaptureFPS              = 30;
static constexpr uintmax_t   kMaxLogFileSizeBytes     = 10ull * 1024ull * 1024ull;

struct BootstrapFile {
    HostConnectionSettings settings;
    std::string enrollment_token;
};

struct EnrollmentResponse {
    bool        success = false;
    std::string machine_id;
    std::string machine_key;
    std::string access_token;
    std::string refresh_token;
    int         status_code = 0;
    bool        retryable = false;
    bool        used_revocation_offline_fallback = false;
    std::string error;
};

static HostConnectionSettings g_connection_settings;

// â”€â”€â”€ Global shutdown â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Ponteiro para o stop flag ativo (console ou serviÃ§o).
static std::atomic<bool>* g_stop_flag = nullptr;

static bool IsRunning() {
    return g_stop_flag && !g_stop_flag->load();
}

static BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        printf("[host] Shutdown requested...\n");
        if (g_stop_flag) g_stop_flag->store(true);
        return TRUE;
    }
    return FALSE;
}

// â”€â”€â”€ Logging helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Log para arquivo + stderr (stderr visÃ­vel apenas em modo console).
static FILE* g_log_file = nullptr;

static fs::path GetLogFilePath() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return fs::path(exe_path).parent_path() / "remotex_host.log";
}

static void ReopenLogFile(const wchar_t* mode) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }

    const fs::path log_path = GetLogFilePath();
    g_log_file = _wfopen(log_path.c_str(), mode);
}

static void TrimLogFileIfNeeded() {
    const fs::path log_path = GetLogFilePath();
    std::error_code ec;
    const auto size = fs::file_size(log_path, ec);
    if (ec || size <= kMaxLogFileSizeBytes) {
        return;
    }

    ReopenLogFile(L"w");
    if (g_log_file) {
        fflush(g_log_file);
    }
    ReopenLogFile(L"a");
}

static void OpenLogFile() {
    ReopenLogFile(L"a");
    TrimLogFileIfNeeded();
}

static void WriteCrashOutput(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const std::string sanitized = remotex::core::SanitizeLogMessage(msg);

    if (g_log_file) {
        fprintf(g_log_file, "%s", sanitized.c_str());
        fflush(g_log_file);
    }
    fprintf(stderr, "%s", sanitized.c_str());
    fflush(stderr);
}

static void Log(const char* fmt, ...) {
    // Timestamp simples
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d [host] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, fmt);

    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const std::string sanitized = remotex::core::SanitizeLogMessage(msg);

    if (g_log_file) {
        TrimLogFileIfNeeded();
        fprintf(g_log_file, "%s%s\n", prefix, sanitized.c_str());
        fflush(g_log_file);
    }
    fprintf(stderr, "%s%s\n", prefix, sanitized.c_str());
    fflush(stderr);
}

static fs::path GetCurrentExecutablePath() {
    wchar_t exe_path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return fs::path(exe_path);
}

static DWORD GetCurrentProcessSessionIdForLogging() {
    DWORD session_id = 0xFFFFFFFF;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        return session_id;
    }
    return WTSGetActiveConsoleSessionId();
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

static void ConfigureProcessDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(HANDLE);
    using SetProcessDpiAwarenessFn = HRESULT (WINAPI*)(int);
    using SetProcessDPIAwareFn = BOOL (WINAPI*)();

    const HANDLE kPerMonitorAwareV2Context = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4));
    constexpr int kProcessPerMonitorDpiAware = 2;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context && set_context(kPerMonitorAwareV2Context)) {
            Log("Process DPI awareness set to Per-Monitor v2");
            return;
        }
    }

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_awareness) {
            const HRESULT hr = set_awareness(kProcessPerMonitorDpiAware);
            FreeLibrary(shcore);
            if (SUCCEEDED(hr) || hr == E_ACCESSDENIED) {
                Log("Process DPI awareness set to Per-Monitor");
                return;
            }
        } else {
            FreeLibrary(shcore);
        }
    }

    if (user32) {
        auto set_legacy = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (set_legacy && set_legacy()) {
            Log("Process DPI awareness set to System DPI aware");
            return;
        }
    }

    Log("WARNING: unable to enable DPI awareness; coordinate mapping may be scaled by Windows");
}

// â”€â”€â”€ Crash diagnostics â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void WriteUpdateHelperStatus(const fs::path& status_file,
                                    const std::string& outcome,
                                    int code,
                                    const std::string& stage,
                                    const std::string& message,
                                    const fs::path& target_exe,
                                    const fs::path& source_binary,
                                    const std::string& version) {
    std::error_code ec;
    fs::create_directories(status_file.parent_path(), ec);

    std::ofstream file(status_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    file << "outcome=" << outcome << "\n";
    file << "code=" << code << "\n";
    file << "stage=" << stage << "\n";
    file << "message=" << message << "\n";
    if (!version.empty()) {
        file << "version=" << version << "\n";
    }
    if (!target_exe.empty()) {
        file << "target_exe=" << target_exe.string() << "\n";
    }
    if (!source_binary.empty()) {
        file << "source_binary=" << source_binary.string() << "\n";
    }
}

static void FlushPendingUpdateStatusToLog() {
    const fs::path status_file = fs::temp_directory_path() / "remotex_apply_update_status.txt";
    if (!fs::exists(status_file)) {
        return;
    }

    std::ifstream file(status_file, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            Log("Auto-update helper report: %s", line.c_str());
        }
    }

    std::error_code ec;
    fs::remove(status_file, ec);
}

static void DumpStackTrace(const char* reason) {
    WriteCrashOutput("\n===============================================\n");
    WriteCrashOutput("CRASH DETECTED: %s\n", reason);
    WriteCrashOutput("===============================================\n");

    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256, 1);
    if (symbol) {
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

        WriteCrashOutput("Stack trace (%u frames):\n", frames);
        for (USHORT i = 0; i < frames; ++i) {
            SymFromAddr(process, (DWORD64)stack[i], 0, symbol);
            WriteCrashOutput("  [%2u] %s (0x%llx)\n", i, symbol->Name, symbol->Address);
        }
        free(symbol);
    }

    SymCleanup(process);
}

static void InvalidParameterHandler(
    const wchar_t* expression, const wchar_t* function,
    const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
{
    WriteCrashOutput("\n[CRASH] CRT Invalid Parameter!\n");
    if (function) WriteCrashOutput("  Function: %ls\n", function);
    if (expression) WriteCrashOutput("  Expression: %ls\n", expression);
    if (file) WriteCrashOutput("  File: %ls (line %u)\n", file, line);
    DumpStackTrace("CRT Invalid Parameter");
}

[[noreturn]] static void TerminateHandler() {
    DumpStackTrace("std::terminate()");
    try {
        auto eptr = std::current_exception();
        if (eptr) std::rethrow_exception(eptr);
    } catch (const std::exception& e) {
        WriteCrashOutput("  Exception: %s\n", e.what());
    } catch (...) {
        WriteCrashOutput("  Unknown exception type\n");
    }
    _exit(99);
}

static void SignalAbortHandler(int /*sig*/) {
    DumpStackTrace("SIGABRT");
    _exit(98);
}

static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* info) {
    WriteCrashOutput("\n[CRASH] Unhandled SEH Exception: 0x%08lX\n",
                     info->ExceptionRecord->ExceptionCode);
    WriteCrashOutput("  Address: 0x%p\n", info->ExceptionRecord->ExceptionAddress);
    DumpStackTrace("SEH Exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

static void InstallCrashHandlers() {
    _set_invalid_parameter_handler(InvalidParameterHandler);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    std::set_terminate(TerminateHandler);
    signal(SIGABRT, SignalAbortHandler);
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
}

static void SaveWorkerSignalingState(bool connected);

struct SecureAttentionAttemptResult {
    bool success = false;
    std::string error;
    DWORD windows_error = 0;
    bool attempted_os_path = false;
};

static std::string EscapeJsonValue(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

static std::string BuildSecureAttentionStatusPayload(bool acknowledged,
                                                     const std::string& error = {},
                                                     std::optional<DWORD> windows_error = std::nullopt) {
    std::string payload = R"({"type":"secure_attention_status","acknowledged":)";
    payload += (acknowledged ? "true" : "false");
    if (!error.empty()) {
        payload += R"(,"error":")" + EscapeJsonValue(error) + '"';
    }
    if (windows_error.has_value()) {
        payload += R"(,"windows_error":)" + std::to_string(*windows_error);
    }
    payload += '}';
    return payload;
}

static std::optional<std::string> ExtractJsonStringField(const std::string& json,
                                                         const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    std::string value;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char next = json[i + 1];
            switch (next) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += next; break;
            }
            ++i;
            continue;
        }
        if (json[i] == '"') {
            return value;
        }
        value += json[i];
    }
    return std::nullopt;
}

static std::optional<DWORD> ExtractJsonDwordField(const std::string& json,
                                                  const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') {
        ++pos;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

static SecureAttentionAttemptResult AttemptSecureAttentionInService() {
    SecureAttentionAttemptResult result;

    const DWORD console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
        result.error = "no_active_console_session";
        Log("Secure attention request failed: no active console session");
        return result;
    }

    DWORD current_session_id = 0xFFFFFFFF;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id)) {
        result.windows_error = GetLastError();
        result.error = "session_query_failed";
        Log("Secure attention request failed: ProcessIdToSessionId err=%lu",
            result.windows_error);
        return result;
    }

    Log("Secure attention request runtime: current_session=%lu active_console_session=%lu is_service=%s",
        static_cast<unsigned long>(current_session_id),
        static_cast<unsigned long>(console_session_id),
        current_session_id == 0 ? "true" : "false");

    if (current_session_id != 0) {
        result.error = "unsupported_runtime_context";
        Log("Secure attention request failed: current session %lu is not Session 0 service runtime",
            static_cast<unsigned long>(current_session_id));
        return result;
    }

    using SendSASFn = VOID (WINAPI*)(BOOL);
    HMODULE sas_module = LoadLibraryW(L"sas.dll");
    if (!sas_module) {
        result.windows_error = GetLastError();
        result.error = "sas_api_unavailable";
        Log("Secure attention request failed: LoadLibraryW(sas.dll) err=%lu",
            result.windows_error);
        return result;
    }

    auto* send_sas = reinterpret_cast<SendSASFn>(GetProcAddress(sas_module, "SendSAS"));
    if (!send_sas) {
        result.windows_error = GetLastError();
        result.error = "sas_api_unavailable";
        Log("Secure attention request failed: GetProcAddress(SendSAS) err=%lu",
            result.windows_error);
        FreeLibrary(sas_module);
        return result;
    }

    result.attempted_os_path = true;
    SetLastError(ERROR_SUCCESS);
    send_sas(FALSE);
    result.windows_error = GetLastError();
    FreeLibrary(sas_module);

    if (result.windows_error != ERROR_SUCCESS) {
        result.error = "sas_api_failed";
        Log("Secure attention request failed: SendSAS err=%lu", result.windows_error);
        return result;
    }

    result.success = true;
    Log("Secure attention request dispatched successfully via SendSAS");
    return result;
}

static SecureAttentionAttemptResult RequestSecureAttentionFromService() {
    SecureAttentionAttemptResult result;

    HANDLE pipe = CreateFileW(kSecureAttentionPipeName,
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        result.windows_error = err;
        result.error = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY)
            ? "unsupported_runtime_context"
            : "session_query_failed";
        Log("Secure attention bridge connect failed: err=%lu", err);
        return result;
    }

    const std::string request = R"({"type":"secure_attention"})";
    DWORD bytes_written = 0;
    if (!WriteFile(pipe,
                   request.data(),
                   static_cast<DWORD>(request.size()),
                   &bytes_written,
                   nullptr)) {
        result.windows_error = GetLastError();
        result.error = "session_query_failed";
        Log("Secure attention bridge write failed: err=%lu", result.windows_error);
        CloseHandle(pipe);
        return result;
    }

    if (!FlushFileBuffers(pipe)) {
        Log("WARNING: Secure attention bridge flush failed: err=%lu", GetLastError());
    }

    std::array<char, 512> buffer{};
    DWORD bytes_read = 0;
    if (!ReadFile(pipe,
                  buffer.data(),
                  static_cast<DWORD>(buffer.size() - 1),
                  &bytes_read,
                  nullptr)) {
        result.windows_error = GetLastError();
        result.error = "session_query_failed";
        Log("Secure attention bridge read failed: err=%lu", result.windows_error);
        CloseHandle(pipe);
        return result;
    }
    CloseHandle(pipe);

    const std::string response(buffer.data(), bytes_read);
    result.success = response.find("\"acknowledged\":true") != std::string::npos;
    if (const auto error = ExtractJsonStringField(response, "error")) {
        result.error = *error;
    }
    if (const auto windows_error = ExtractJsonDwordField(response, "windows_error")) {
        result.windows_error = *windows_error;
    }
    result.attempted_os_path = response.find("\"windows_error\"") != std::string::npos || result.success;
    return result;
}

static void RunSecureAttentionPipeServer(std::atomic<bool>& stop_flag) {
    while (!stop_flag.load()) {
        HANDLE pipe = CreateNamedPipeW(
            kSecureAttentionPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            512,
            512,
            1000,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("ERROR: CreateNamedPipeW(%ls) failed: %lu", kSecureAttentionPipeName, GetLastError());
            Sleep(1000);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err != ERROR_NO_DATA && err != ERROR_OPERATION_ABORTED) {
                Log("WARNING: Secure attention pipe connect failed: %lu", err);
            }
            CloseHandle(pipe);
            continue;
        }

        std::array<char, 256> request_buffer{};
        DWORD bytes_read = 0;
        const BOOL read_ok = ReadFile(pipe,
                                      request_buffer.data(),
                                      static_cast<DWORD>(request_buffer.size() - 1),
                                      &bytes_read,
                                      nullptr);
        std::string response;
        if (!read_ok) {
            const DWORD err = GetLastError();
            Log("WARNING: Secure attention pipe read failed: %lu", err);
            response = BuildSecureAttentionStatusPayload(false, "session_query_failed", err);
        } else {
            const std::string request(request_buffer.data(), bytes_read);
            if (request.find("\"type\":\"secure_attention\"") == std::string::npos) {
                Log("WARNING: Secure attention pipe received malformed request: %s", request.c_str());
                response = BuildSecureAttentionStatusPayload(false, "session_query_failed");
            } else {
                const auto result = AttemptSecureAttentionInService();
                response = BuildSecureAttentionStatusPayload(
                    result.success,
                    result.success ? std::string{} : result.error,
                    result.windows_error != ERROR_SUCCESS
                        ? std::optional<DWORD>(result.windows_error)
                        : std::nullopt);
            }
        }

        DWORD bytes_written = 0;
        if (!WriteFile(pipe,
                       response.data(),
                       static_cast<DWORD>(response.size()),
                       &bytes_written,
                       nullptr)) {
            Log("WARNING: Secure attention pipe write failed: %lu", GetLastError());
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

namespace {

#pragma pack(push, 1)
struct SecureDesktopInputRequestHeader {
    uint32_t version = 1;
    int32_t monitor_left = 0;
    int32_t monitor_top = 0;
    int32_t monitor_right = 0;
    int32_t monitor_bottom = 0;
    uint32_t payload_size = 0;
};

struct SecureDesktopInputResponse {
    uint32_t version = 1;
    uint32_t events_processed = 0;
    uint32_t desktop_switch_attempted = 0;
    uint32_t desktop_switch_succeeded = 0;
    uint32_t malformed_input = 0;
    uint32_t desktop_switch_error = ERROR_SUCCESS;
    uint32_t send_input_failures = 0;
    uint32_t last_send_input_error = ERROR_SUCCESS;
    char input_desktop_name[64]{};
    char thread_desktop_name[64]{};
};
#pragma pack(pop)

static_assert(sizeof(SecureDesktopInputRequestHeader) == 24);
static_assert(sizeof(SecureDesktopInputResponse) == 160);

std::wstring BuildSecureDesktopInputPipeName()
{
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        session_id = 0;
    }

    return std::wstring(kSecureDesktopInputPipePrefix) +
           std::to_wstring(session_id) +
           L"-" +
           std::to_wstring(GetCurrentProcessId());
}

void CopyStringToFixedBuffer(const std::string& value, char (&buffer)[64])
{
    std::memset(buffer, 0, sizeof(buffer));
    if (value.empty()) {
        return;
    }

    const size_t bytes_to_copy = std::min(value.size(), sizeof(buffer) - 1);
    std::memcpy(buffer, value.data(), bytes_to_copy);
    buffer[bytes_to_copy] = '\0';
}

std::string ReadFixedBufferString(const char (&buffer)[64])
{
    const size_t length = strnlen_s(buffer, sizeof(buffer));
    return std::string(buffer, length);
}

class SecureDesktopInputHelperBridge final : public remotex::host::InputInjector::SecureDesktopBridge {
public:
    explicit SecureDesktopInputHelperBridge(fs::path executable_path)
        : executable_path_(std::move(executable_path)),
          pipe_name_(BuildSecureDesktopInputPipeName())
    {
    }

    ~SecureDesktopInputHelperBridge() override
    {
        std::scoped_lock lock(mutex_);
        StopHelperLocked();
    }

    bool Submit(std::span<const uint8_t> data,
                LONG monitor_left,
                LONG monitor_top,
                LONG monitor_right,
                LONG monitor_bottom,
                remotex::host::InputInjector::ProcessInputResult& result) override
    {
        std::scoped_lock lock(mutex_);
        DWORD error = ERROR_SUCCESS;
        if (!EnsureHelperRunningLocked(error)) {
            result.secure_desktop_bridge_error = error;
            return false;
        }

        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 10; ++attempt) {
            pipe = CreateFileW(pipe_name_.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               nullptr,
                               OPEN_EXISTING,
                               0,
                               nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                break;
            }

            error = GetLastError();
            if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
                break;
            }
            Sleep(100);
        }

        if (pipe == INVALID_HANDLE_VALUE) {
            result.secure_desktop_bridge_error = error;
            return false;
        }

        SecureDesktopInputRequestHeader request;
        request.monitor_left = monitor_left;
        request.monitor_top = monitor_top;
        request.monitor_right = monitor_right;
        request.monitor_bottom = monitor_bottom;
        request.payload_size = static_cast<uint32_t>(data.size());

        DWORD bytes_written = 0;
        if (!WriteFile(pipe,
                       &request,
                       static_cast<DWORD>(sizeof(request)),
                       &bytes_written,
                       nullptr) ||
            bytes_written != sizeof(request)) {
            result.secure_desktop_bridge_error = GetLastError();
            CloseHandle(pipe);
            return false;
        }

        if (!data.empty()) {
            if (!WriteFile(pipe,
                           data.data(),
                           static_cast<DWORD>(data.size()),
                           &bytes_written,
                           nullptr) ||
                bytes_written != data.size()) {
                result.secure_desktop_bridge_error = GetLastError();
                CloseHandle(pipe);
                return false;
            }
        }

        if (!FlushFileBuffers(pipe)) {
            result.secure_desktop_bridge_error = GetLastError();
            CloseHandle(pipe);
            return false;
        }

        SecureDesktopInputResponse response;
        DWORD bytes_read = 0;
        if (!ReadFile(pipe,
                      &response,
                      static_cast<DWORD>(sizeof(response)),
                      &bytes_read,
                      nullptr) ||
            bytes_read != sizeof(response)) {
            result.secure_desktop_bridge_error = GetLastError();
            CloseHandle(pipe);
            return false;
        }
        CloseHandle(pipe);

        if (response.version != 1) {
            result.secure_desktop_bridge_error = ERROR_REVISION_MISMATCH;
            return false;
        }

        result.secure_desktop_bridge_error = ERROR_SUCCESS;
        result.events_processed = response.events_processed;
        result.desktop_switch_attempted = response.desktop_switch_attempted != 0;
        result.desktop_switch_succeeded = response.desktop_switch_succeeded != 0;
        result.malformed_input = response.malformed_input != 0;
        result.desktop_switch_error = response.desktop_switch_error;
        result.send_input_failures = response.send_input_failures;
        result.last_send_input_error = response.last_send_input_error;
        result.input_desktop_name = ReadFixedBufferString(response.input_desktop_name);
        result.thread_desktop_name = ReadFixedBufferString(response.thread_desktop_name);
        return true;
    }

private:
    bool EnsureHelperRunningLocked(DWORD& error)
    {
        error = ERROR_SUCCESS;
        if (helper_process_ != nullptr) {
            const DWORD wait_result = WaitForSingleObject(helper_process_, 0);
            if (wait_result == WAIT_TIMEOUT) {
                return true;
            }
            StopHelperLocked();
        }

        std::wstring command_line = L"\"" + executable_path_.wstring() +
                                    L"\" --secure-input-helper --secure-input-pipe \"" +
                                    pipe_name_ +
                                    L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\Winlogon");
        PROCESS_INFORMATION pi{};

        const BOOL launched = CreateProcessW(
            executable_path_.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            executable_path_.parent_path().empty() ? nullptr : executable_path_.parent_path().c_str(),
            &si,
            &pi);
        if (!launched) {
            error = GetLastError();
            Log("Secure desktop input helper launch failed: err=%lu", static_cast<unsigned long>(error));
            return false;
        }

        helper_process_ = pi.hProcess;
        helper_pid_ = pi.dwProcessId;
        CloseHandle(pi.hThread);
        Log("Secure desktop input helper launched (pid=%lu pipe=%ls)",
            static_cast<unsigned long>(helper_pid_),
            pipe_name_.c_str());
        return true;
    }

    void StopHelperLocked()
    {
        if (helper_process_ == nullptr) {
            return;
        }

        TerminateProcess(helper_process_, 0);
        CloseHandle(helper_process_);
        helper_process_ = nullptr;
        helper_pid_ = 0;
    }

    fs::path executable_path_;
    std::wstring pipe_name_;
    HANDLE helper_process_ = nullptr;
    DWORD helper_pid_ = 0;
    std::mutex mutex_;
};

int RunSecureDesktopInputHelper(const std::wstring& pipe_name)
{
    Log("Secure desktop input helper starting (session=%lu pipe=%ls)",
        static_cast<unsigned long>(GetCurrentProcessSessionIdForLogging()),
        pipe_name.c_str());

    remotex::host::InputInjector injector;
    injector.Start();

    for (;;) {
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       1,
                                       4096,
                                       4096,
                                       0,
                                       nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("ERROR: CreateNamedPipeW(%ls) failed for secure input helper: %lu",
                pipe_name.c_str(),
                static_cast<unsigned long>(GetLastError()));
            return 1;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
            (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (!connected) {
            const DWORD connect_error = GetLastError();
            CloseHandle(pipe);
            if (connect_error == ERROR_NO_DATA || connect_error == ERROR_BROKEN_PIPE) {
                continue;
            }
            Log("WARNING: Secure input helper pipe connect failed: %lu",
                static_cast<unsigned long>(connect_error));
            continue;
        }

        SecureDesktopInputRequestHeader request;
        DWORD bytes_read = 0;
        if (!ReadFile(pipe,
                      &request,
                      static_cast<DWORD>(sizeof(request)),
                      &bytes_read,
                      nullptr) ||
            bytes_read != sizeof(request)) {
            CloseHandle(pipe);
            continue;
        }

        std::vector<uint8_t> payload(request.payload_size);
        if (request.payload_size != 0) {
            if (!ReadFile(pipe,
                          payload.data(),
                          request.payload_size,
                          &bytes_read,
                          nullptr) ||
                bytes_read != request.payload_size) {
                CloseHandle(pipe);
                continue;
            }
        }

        injector.SetCapturedMonitorBounds(request.monitor_left,
                                          request.monitor_top,
                                          request.monitor_right,
                                          request.monitor_bottom);
        const auto result = injector.ProcessInputData(payload);

        SecureDesktopInputResponse response;
        response.events_processed = static_cast<uint32_t>(result.events_processed);
        response.desktop_switch_attempted = result.desktop_switch_attempted ? 1u : 0u;
        response.desktop_switch_succeeded = result.desktop_switch_succeeded ? 1u : 0u;
        response.malformed_input = result.malformed_input ? 1u : 0u;
        response.desktop_switch_error = result.desktop_switch_error;
        response.send_input_failures = result.send_input_failures;
        response.last_send_input_error = result.last_send_input_error;
        CopyStringToFixedBuffer(result.input_desktop_name, response.input_desktop_name);
        CopyStringToFixedBuffer(result.thread_desktop_name, response.thread_desktop_name);

        DWORD bytes_written = 0;
        WriteFile(pipe,
                  &response,
                  static_cast<DWORD>(sizeof(response)),
                  &bytes_written,
                  nullptr);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace

// â”€â”€â”€ HostSession: orquestra signaling + WebRTC + capture para uma sessÃ£o â”€â”€â”€â”€

class HostSession : public remotex::core::SignalingListener,
                    public remotex::core::WebRTCPeerListener,
                    public remotex::core::FileTransferListener {
public:
#include "main_host_session_run.inc"
#include "main_host_session_signaling.inc"
#include "main_host_session_capture_prepare.inc"
#include "main_host_session_capture_publish.inc"
#include "main_host_session_control.inc"
};

#include "main_credentials_registry.inc"
#include "main_http_auth.inc"
#include "main_service_update.inc"

static int RunHost(bool is_service,
                   bool is_desktop,
                   std::atomic<bool>& stop_flag,
                   const std::string& cli_email,
                   const std::string& cli_password)
{
    g_stop_flag = &stop_flag;
    const fs::path exe_path = GetCurrentExecutablePath();

    Log("RemoteX Host v%s starting... (mode: %s%s)",
        kVersion, is_service ? "service" : "console",
        is_desktop ? " desktop" : "");
    Log("Runtime identity: pid=%lu session=%lu stop_flag=%s",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentProcessSessionIdForLogging()),
        g_stop_flag ? "set" : "null");
    if (!exe_path.empty()) {
        Log("Executable path: %s", exe_path.string().c_str());
    }
    FlushPendingUpdateStatusToLog();

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // SERVICE WATCHER LOOP (Session 0)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    if (is_service && !is_desktop) {
        remotex::core::AuthClient auth;
        bool authenticated_as_machine = false;
        if (!AuthenticateHost(auth, is_service, is_desktop, cli_email, cli_password, authenticated_as_machine)) {
            return 1;
        }

        remotex::core::MachineClient machines;
        remotex::core::MachineClientConfig machine_cfg;
        machine_cfg.api_host = g_connection_settings.api_host;
        machine_cfg.api_port = g_connection_settings.api_port;
        machine_cfg.use_tls  = g_connection_settings.api_use_tls;
        machines.Configure(machine_cfg, &auth);

        std::string machine_id;
        std::string machine_key;
        if (!EnsureMachineRegistration(auth, machines, is_service, is_desktop, authenticated_as_machine, machine_id, machine_key)) {
            return 1;
        }

        remotex::host::Heartbeat heartbeat;
        remotex::host::HeartbeatConfig hb_cfg;
        hb_cfg.machine_id    = machine_id;
        hb_cfg.machine_key   = machine_key;
        hb_cfg.agent_version = kVersion;
        hb_cfg.on_error      = [](const std::string& message) {
            Log("WARNING: %s", message.c_str());
        };
        hb_cfg.on_force_update = []() {
            Log("Force update requested via heartbeat");
            if (IsUpdaterServiceRunning()) {
                // Signal the updater's named wakeup event to trigger an immediate check.
                // This is non-disruptive: the updater wakes from sleep without restarting.
                HANDLE evt = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                        L"Global\\RemoteXUpdaterWakeup");
                if (evt) {
                    SetEvent(evt);
                    CloseHandle(evt);
                    Log("Signaled RemoteXUpdater wakeup event for immediate update check");
                } else {
                    // Fallback: send custom service control code to wake the updater.
                    Log("Wakeup event not found, sending service control wakeup...");
                    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                    if (scm) {
                        SC_HANDLE svc = OpenServiceW(scm, L"RemoteXUpdater",
                                                     SERVICE_USER_DEFINED_CONTROL);
                        if (svc) {
                            SERVICE_STATUS status{};
                            ControlService(svc, 128, &status);  // kControlWakeup = 128
                            CloseServiceHandle(svc);
                            Log("Sent wakeup control to RemoteXUpdater service");
                        }
                        CloseServiceHandle(scm);
                    }
                }
            } else {
                Log("WARNING: Force update requested but RemoteXUpdater is not running");
            }
        };
        hb_cfg.interval_sec  = 30;
        heartbeat.Start(hb_cfg, &machines);
        Log("Heartbeat started in service watcher (every %us)", hb_cfg.interval_sec);

        // Ensure RemoteXUpdater service is installed and running (sole owner of updates).
        TryInstallUpdaterService();
        if (IsUpdaterServiceRunning()) {
            Log("RemoteXUpdater service is managing updates");
        } else {
            Log("WARNING: RemoteXUpdater service is not running â€” host will not auto-update");
        }

        Log("Running in Session 0 Watcher mode. Monitoring active sessions...");
        std::thread secure_attention_pipe_thread([&stop_flag]() {
            RunSecureAttentionPipeServer(stop_flag);
        });
        HANDLE hDesktopProcess = NULL;
        DWORD currentSessionId = 0xFFFFFFFF;
        auto worker_started_at = std::chrono::steady_clock::now();
        bool worker_heartbeat_degraded_logged = false;

      try {
        while (!stop_flag.load()) {
            DWORD activeSession = WTSGetActiveConsoleSessionId();

            if (activeSession != 0xFFFFFFFF) {
                if (activeSession != currentSessionId) {
                    // SessÃ£o mudou (ex: Fast User Switching). Matar antigo se existir.
                    if (hDesktopProcess) {
                        Log("Session changed from %lu to %lu. Terminating old worker.", currentSessionId, activeSession);
                        TerminateProcess(hDesktopProcess, 0);
                        CloseHandle(hDesktopProcess);
                        hDesktopProcess = NULL;
                        ResetWorkerState();
                        worker_heartbeat_degraded_logged = false;
                    }
                    ResetWorkerState();
                    currentSessionId = activeSession;
                }

                // Verificar se o processo estÃ¡ vivo
                if (hDesktopProcess) {
                    const DWORD worker_pid = GetProcessId(hDesktopProcess);
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(hDesktopProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                        const auto worker_state = LoadDesktopWorkerRegistryState();
                        const auto worker_runtime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - worker_started_at).count();
                        Log("Worker process exited with code %lu after %llds (pid=%lu reg_pid=%lu session=%lu reg_session=%lu heartbeat_utc=%llu signaling_up=%lu signaling_utc=%llu). Respawning...",
                            exitCode,
                            static_cast<long long>(worker_runtime_seconds),
                            static_cast<unsigned long>(worker_pid),
                            static_cast<unsigned long>(worker_state.pid),
                            static_cast<unsigned long>(activeSession),
                            static_cast<unsigned long>(worker_state.session_id),
                            static_cast<unsigned long long>(worker_state.heartbeat_utc),
                            static_cast<unsigned long>(worker_state.signaling_up),
                            static_cast<unsigned long long>(worker_state.signaling_utc));
                        CloseHandle(hDesktopProcess);
                        hDesktopProcess = NULL;
                        ResetWorkerState();
                        worker_heartbeat_degraded_logged = false;
                    } else {
                        const bool worker_heartbeat_healthy =
                            DesktopWorkerHealthy(activeSession, worker_pid, worker_started_at);
                        const bool worker_signaling_healthy =
                            DesktopWorkerSignalingHealthy(worker_started_at);
                        const auto watchdog_decision =
                            remotex::host::EvaluateWorkerWatchdogDecision(worker_heartbeat_healthy,
                                                                          worker_signaling_healthy);

                        if (watchdog_decision.deferred_for_signaling()) {
                            if (!worker_heartbeat_degraded_logged) {
                                const auto worker_state = LoadDesktopWorkerRegistryState();
                                const auto worker_runtime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - worker_started_at).count();
                                Log("Worker heartbeat stale for session %lu after %llds, but signaling is still healthy (pid=%lu reg_pid=%lu up=%lu heartbeat_utc=%llu signaling_utc=%llu). Deferring restart.",
                                    activeSession,
                                    static_cast<long long>(worker_runtime_seconds),
                                    static_cast<unsigned long>(worker_pid),
                                    static_cast<unsigned long>(worker_state.pid),
                                    static_cast<unsigned long>(worker_state.signaling_up),
                                    static_cast<unsigned long long>(worker_state.heartbeat_utc),
                                    static_cast<unsigned long long>(worker_state.signaling_utc));
                                worker_heartbeat_degraded_logged = true;
                            }
                        } else {
                            worker_heartbeat_degraded_logged = false;
                            if (watchdog_decision.should_restart) {
                                const auto worker_state = LoadDesktopWorkerRegistryState();
                                const auto worker_runtime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - worker_started_at).count();
                                if (watchdog_decision.heartbeat_stale) {
                                    Log("Worker heartbeat and signaling unhealthy for session %lu after %llds (pid=%lu reg_pid=%lu reg_session=%lu up=%lu heartbeat_utc=%llu signaling_utc=%llu). Restarting worker.",
                                        activeSession,
                                        static_cast<long long>(worker_runtime_seconds),
                                        static_cast<unsigned long>(worker_pid),
                                        static_cast<unsigned long>(worker_state.pid),
                                        static_cast<unsigned long>(worker_state.session_id),
                                        static_cast<unsigned long>(worker_state.signaling_up),
                                        static_cast<unsigned long long>(worker_state.heartbeat_utc),
                                        static_cast<unsigned long long>(worker_state.signaling_utc));
                                } else {
                                    Log("Worker signaling unhealthy for session %lu after %llds (pid=%lu reg_pid=%lu reg_session=%lu up=%lu heartbeat_utc=%llu signaling_utc=%llu). Restarting worker.",
                                        activeSession,
                                        static_cast<long long>(worker_runtime_seconds),
                                        static_cast<unsigned long>(worker_pid),
                                        static_cast<unsigned long>(worker_state.pid),
                                        static_cast<unsigned long>(worker_state.session_id),
                                        static_cast<unsigned long>(worker_state.signaling_up),
                                        static_cast<unsigned long long>(worker_state.heartbeat_utc),
                                        static_cast<unsigned long long>(worker_state.signaling_utc));
                                }
                                TerminateProcess(hDesktopProcess, 0);
                                CloseHandle(hDesktopProcess);
                                hDesktopProcess = NULL;
                                ResetWorkerState();
                            }
                        }
                    }
                }

                // Iniciar novo se nÃ£o houver um vivo
                if (!hDesktopProcess) {
                    ResetWorkerState();
                    worker_heartbeat_degraded_logged = false;
                    if (SpawnDesktopProcess(activeSession, hDesktopProcess)) {
                        worker_started_at = std::chrono::steady_clock::now();
                    }
                }
            } else {
                // Nenhuma sessÃ£o ativa conectada na console fÃ­sica (ex: tela de lock sem usuÃ¡rio)
                if (hDesktopProcess) {
                    Log("No active console session. Terminating worker.");
                    TerminateProcess(hDesktopProcess, 0);
                    CloseHandle(hDesktopProcess);
                    hDesktopProcess = NULL;
                }
                ResetWorkerState();
                worker_heartbeat_degraded_logged = false;
                currentSessionId = 0xFFFFFFFF;
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
      } catch (const std::exception& e) {
        Log("FATAL EXCEPTION in watcher loop: %s", e.what());
        DumpStackTrace("exception in watcher loop");
      } catch (...) {
        Log("FATAL unknown exception in watcher loop");
        DumpStackTrace("unknown exception in watcher loop");
      }

        if (hDesktopProcess) {
            TerminateProcess(hDesktopProcess, 0);
            CloseHandle(hDesktopProcess);
        }

        ResetWorkerState();
        heartbeat.Stop();
        if (secure_attention_pipe_thread.joinable()) {
            const HANDLE wake_pipe = CreateFileW(kSecureAttentionPipeName,
                                                 GENERIC_READ | GENERIC_WRITE,
                                                 0,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 0,
                                                 nullptr);
            if (wake_pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(wake_pipe);
            }
            secure_attention_pipe_thread.join();
        }

        if (ShouldClearPersistedHostStateOnExit()) {
            Log("Service stop requested persisted host state cleanup. Clearing LocalSystem host identity...");
            ClearPersistedHostState();
        }

        Log("Watcher shutdown complete.");
        return 0;
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // WORKER / CONSOLE MODE (Session 1+)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
  try {

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // 1. AutenticaÃ§Ã£o
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    remotex::core::AuthClient auth;
    bool authenticated_as_machine = false;
    if (!AuthenticateHost(auth, is_service, is_desktop, cli_email, cli_password, authenticated_as_machine)) {
        return 1;
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // 2. Registro da mÃ¡quina + Heartbeat
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    remotex::core::MachineClient machines;
    remotex::core::MachineClientConfig machine_cfg;
    machine_cfg.api_host = g_connection_settings.api_host;
    machine_cfg.api_port = g_connection_settings.api_port;
    machine_cfg.use_tls  = g_connection_settings.api_use_tls;
    machines.Configure(machine_cfg, &auth);

    std::string machine_id;
    std::string machine_key;
    if (!EnsureMachineRegistration(auth, machines, is_service, is_desktop, authenticated_as_machine, machine_id, machine_key)) {
        return 1;
    }

    bool heartbeat_started = false;
    remotex::host::Heartbeat heartbeat;
    if (!is_desktop) {
        Log("[STEP] Machine registered OK, starting heartbeat...");

        remotex::host::HeartbeatConfig hb_cfg;
        hb_cfg.machine_id    = machine_id;
        hb_cfg.machine_key   = machine_key;
        hb_cfg.agent_version = kVersion;
        hb_cfg.on_error      = [](const std::string& message) {
            Log("WARNING: %s", message.c_str());
        };
        hb_cfg.interval_sec  = 30;
        heartbeat.Start(hb_cfg, &machines);
        heartbeat_started = true;
        Log("Heartbeat started (every %us)", hb_cfg.interval_sec);
    } else {
        Log("[STEP] Machine registered OK, desktop worker skipping heartbeat");
    }

    Log("[STEP] Heartbeat started OK, initializing backup agent...");

    WorkerHeartbeatReporter worker_reporter;
    if (is_desktop) {
        DWORD session_id = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
            session_id = WTSGetActiveConsoleSessionId();
        }
        ResetWorkerState();
        SaveWorkerSignalingState(false);
        worker_reporter.Start(session_id);
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // 3. Backup Agent (background)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    remotex::host::BackupAgent backup;
    remotex::host::BackupAgent::Config backup_cfg;
    backup_cfg.api_host      = g_connection_settings.api_host;
    backup_cfg.api_port      = g_connection_settings.api_port;
    backup_cfg.use_tls       = g_connection_settings.api_use_tls;
    backup_cfg.machine_id    = machine_id;
    backup_cfg.manifest_path = fs::temp_directory_path() / "remotex_backup_manifest.json";
    backup_cfg.temp_dir      = fs::temp_directory_path() / "remotex_backup";
    fs::create_directories(backup_cfg.temp_dir);

    backup.Initialize(backup_cfg, &auth);
    backup.Start();
    Log("Backup agent started");

    Log("[STEP] Backup agent started OK, starting main session...");

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // 5. SessÃ£o principal (signaling + WebRTC + capture + input)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    HostSession session(&auth, machine_id, machine_key);
    Log("[STEP] HostSession created, entering Run()...");
    session.Run(); // Bloqueia atÃ© stop_flag == true.

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // 6. Shutdown
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    Log("Shutting down...");
    backup.Stop();
    if (heartbeat_started) heartbeat.Stop();
    SaveWorkerSignalingState(false);
    worker_reporter.Stop();
    Log("Goodbye.");

    return 0;

  } catch (const std::exception& e) {
    Log("FATAL EXCEPTION: %s", e.what());
    DumpStackTrace("std::exception in main");
    return 2;
  } catch (...) {
    Log("FATAL: Unknown exception in main");
    DumpStackTrace("Unknown exception in main");
    return 3;
  }
}

// â”€â”€â”€ Entrada principal â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

int main(int argc, char* argv[])
{
    OpenLogFile();
    ConfigureProcessDpiAwareness();
    printf("DEBUG: remotex_host.exe started (argc=%d)\n", argc);
    fflush(stdout);
    Log("Process started (argc=%d)", argc);
    InstallCrashHandlers();

    // Parse de argumentos.
    std::string cli_email, cli_password;
    std::string cli_service_name;
    std::string cli_target_exe;
    std::string cli_source_binary;
    std::string cli_backup_exe;
    std::string cli_target_version;
    std::string cli_status_file;
    std::string cli_secure_input_pipe;
    bool do_install   = false;
    bool do_uninstall = false;
    bool do_service   = false;
    bool do_desktop   = false;
    bool do_apply_update = false;
    bool do_secure_input_helper = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--install")        do_install = true;
        else if (arg == "--uninstall") do_uninstall = true;
        else if (arg == "--service")   do_service = true;
        else if (arg == "--desktop")   do_desktop = true;
        else if (arg == "--apply-update") do_apply_update = true;
        else if (arg == "--secure-input-helper") do_secure_input_helper = true;
        else if (arg == "--email"    && i + 1 < argc) cli_email    = argv[++i];
        else if (arg == "--password" && i + 1 < argc) cli_password = argv[++i];
        else if (arg == "--service-name"  && i + 1 < argc) cli_service_name = argv[++i];
        else if (arg == "--target-exe"    && i + 1 < argc) cli_target_exe = argv[++i];
        else if (arg == "--source-binary" && i + 1 < argc) cli_source_binary = argv[++i];
        else if (arg == "--backup-exe"    && i + 1 < argc) cli_backup_exe = argv[++i];
        else if (arg == "--target-version" && i + 1 < argc) cli_target_version = argv[++i];
        else if (arg == "--status-file"    && i + 1 < argc) cli_status_file = argv[++i];
        else if (arg == "--secure-input-pipe" && i + 1 < argc) cli_secure_input_pipe = argv[++i];
    }

    if (do_secure_input_helper) {
        if (cli_secure_input_pipe.empty()) {
            Log("ERROR: --secure-input-helper requires --secure-input-pipe");
            return 1;
        }
        return RunSecureDesktopInputHelper(ToWide(cli_secure_input_pipe));
    }

    if (do_apply_update) {
        if (cli_service_name.empty() || cli_target_exe.empty() ||
            cli_source_binary.empty() || cli_backup_exe.empty()) {
            Log("ERROR: --apply-update requires --service-name, --target-exe, --source-binary and --backup-exe");
            return 1;
        }

        fs::path status_file = cli_status_file.empty()
            ? (fs::temp_directory_path() / "remotex_apply_update_status.txt")
            : fs::path(ToWide(cli_status_file));

        return RunApplyUpdateHelper(ToWide(cli_service_name),
                                    fs::path(ToWide(cli_target_exe)),
                                    fs::path(ToWide(cli_source_binary)),
                                    fs::path(ToWide(cli_backup_exe)),
                                    cli_target_version,
                                    status_file);
    }

    // â”€â”€ Modo: instalar serviÃ§o â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (do_install) {
        return remotex::host::ServiceManager::Install() ? 0 : 1;
    }

    // â”€â”€ Modo: desinstalar serviÃ§o â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (do_uninstall) {
        RequestPersistedHostStateClearOnExit();
        const bool uninstall_ok = remotex::host::ServiceManager::Uninstall();
        ClearPersistedHostState();
        return uninstall_ok ? 0 : 1;
    }

    // â”€â”€ Modo: serviÃ§o (chamado pelo SCM) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (do_service && !do_desktop) {
        auto host_fn = [&](std::atomic<bool>& stop_flag) -> int {
            return RunHost(/*is_service=*/true, /*is_desktop=*/false, stop_flag, cli_email, cli_password);
        };
        return remotex::host::ServiceManager::RunAsService(host_fn) ? 0 : 1;
    }

    // â”€â”€ Modo: console (debug) ou desktop worker â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Log("Crash handlers installed â€” any abort() will show stack trace");

    std::atomic<bool> console_stop{false};
    return RunHost(/*is_service=*/do_service, /*is_desktop=*/do_desktop, console_stop, cli_email, cli_password);
}
