// RemoteX — core/auth_client.cpp
// Cliente de autenticação HTTP + armazenamento seguro via Windows Credential Manager.

#include "core/auth_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <optional>
#include <sstream>
#include <vector>

namespace remotex::core {

// ─── JSON helpers mínimos ───────────────────────────────────────────────────

static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";
    if (json[pos] == '"') {
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        return json.substr(pos + 1, end - pos - 1);
    }
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

static int64_t JsonGetInt(const std::string& json, const std::string& key) {
    std::string val = JsonGetString(json, key);
    if (val.empty()) return 0;
    try { return std::stoll(val); } catch (...) { return 0; }
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

struct StoredTokens {
    std::string user_id;
    std::string access_token;
    std::string refresh_token;
};

static std::string MakeCredentialEntryTarget(const std::string& credential_target, const std::string& suffix) {
    return credential_target + "_" + suffix;
}

static std::optional<StoredTokens> ReadStoredTokensForTarget(const std::string& credential_target) {
    auto load = [&](const std::string& suffix, std::string* user_id_out) -> std::string {
        auto wide_target = ToWide(MakeCredentialEntryTarget(credential_target, suffix));

        PCREDENTIALW cred = nullptr;
        if (CredReadW(wide_target.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE) {
            return {};
        }

        std::string token(reinterpret_cast<char*>(cred->CredentialBlob),
                          cred->CredentialBlobSize);

        if (user_id_out != nullptr && cred->UserName) {
            int len = WideCharToMultiByte(CP_UTF8, 0, cred->UserName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::string uid(len - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, cred->UserName, -1, uid.data(), len, nullptr, nullptr);
                *user_id_out = uid;
            }
        }

        CredFree(cred);
        return token;
    };

    StoredTokens stored;
    stored.access_token = load("access", &stored.user_id);
    stored.refresh_token = load("refresh", stored.user_id.empty() ? &stored.user_id : nullptr);
    if (stored.access_token.empty() && stored.refresh_token.empty()) {
        return std::nullopt;
    }
    return stored;
}

static void DeleteStoredTokensForTarget(const std::string& credential_target) {
    for (const char* suffix : {"access", "refresh"}) {
        auto wide_target = ToWide(MakeCredentialEntryTarget(credential_target, suffix));
        CredDeleteW(wide_target.c_str(), CRED_TYPE_GENERIC, 0);
    }
}

static std::vector<std::string> BuildCredentialTargetSearchOrder(const AuthConfig& config) {
    std::vector<std::string> targets;
    auto append = [&](const std::string& target) {
        if (target.empty()) {
            return;
        }
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.push_back(target);
        }
    };

    append(config.credential_target);
    for (const auto& legacy_target : config.legacy_credential_targets) {
        append(legacy_target);
    }
    return targets;
}

// ─── Configure ──────────────────────────────────────────────────────────────

void AuthClient::Configure(const AuthConfig& config) {
    std::lock_guard lock(mutex_);
    config_ = config;
}

// ─── Operações de autenticação ──────────────────────────────────────────────

AuthResult AuthClient::Register(const std::string& email,
                                 const std::string& password,
                                 const std::string& name)
{
    std::string body = "{\"email\":\"" + EscapeJson(email) +
                       "\",\"password\":\"" + EscapeJson(password) +
                       "\",\"name\":\"" + EscapeJson(name) + "\"}";

    auto resp = HttpPost("/auth/register", body);
    auto result = ParseAuthResponse(resp);

    if (result.success) {
        std::lock_guard lock(mutex_);
        user_id_       = result.user_id;
        role_          = result.role;
        access_token_  = result.access_token;
        refresh_token_ = result.refresh_token;
        SaveTokens();
    }

    return result;
}

AuthResult AuthClient::Login(const std::string& email, const std::string& password)
{
    std::string body = "{\"email\":\"" + EscapeJson(email) +
                       "\",\"password\":\"" + EscapeJson(password) + "\"}";

    auto resp = HttpPost("/auth/login", body);
    auto result = ParseAuthResponse(resp);

    if (result.success) {
        std::lock_guard lock(mutex_);
        user_id_       = result.user_id;
        role_          = result.role;
        access_token_  = result.access_token;
        refresh_token_ = result.refresh_token;
        SaveTokens();
    }

    return result;
}

AuthResult AuthClient::RefreshToken()
{
    std::string rt;
    {
        std::lock_guard lock(mutex_);
        rt = refresh_token_;
    }

    if (rt.empty())
        return AuthResult{false, "", "", "", "", 0, "no refresh token available"};

    std::string body = "{\"refresh_token\":\"" + EscapeJson(rt) + "\"}";

    auto resp = HttpPost("/auth/refresh", body);
    auto result = ParseAuthResponse(resp);

    if (result.success) {
        std::lock_guard lock(mutex_);
        user_id_       = result.user_id;
        role_          = result.role;
        access_token_  = result.access_token;
        refresh_token_ = result.refresh_token;
        SaveTokens();
    }

    return result;
}

// ─── Estado ─────────────────────────────────────────────────────────────────

bool AuthClient::IsAuthenticated() const {
    std::lock_guard lock(mutex_);
    return !access_token_.empty();
}

std::string AuthClient::GetAccessToken() const {
    std::lock_guard lock(mutex_);
    return access_token_;
}

std::string AuthClient::GetUserID() const {
    std::lock_guard lock(mutex_);
    return user_id_;
}

std::string AuthClient::GetRole() const {
    std::lock_guard lock(mutex_);
    return role_;
}

std::string AuthClient::GetRefreshToken() const {
    std::lock_guard lock(mutex_);
    return refresh_token_;
}

void AuthClient::SetTokensDirect(const std::string& user_id,
                                  const std::string& access_token,
                                  const std::string& refresh_token) {
    std::lock_guard lock(mutex_);
    user_id_       = user_id;
    access_token_  = access_token;
    refresh_token_ = refresh_token;
}

void AuthClient::Logout() {
    std::lock_guard lock(mutex_);
    user_id_.clear();
    role_.clear();
    access_token_.clear();
    refresh_token_.clear();
    ClearStoredTokens();
}

// ─── Windows Credential Manager ─────────────────────────────────────────────

bool AuthClient::SaveTokens() const
{
    // Armazena como: "target_access" e "target_refresh".
    auto save = [&](const std::string& suffix, const std::string& token) -> bool {
        if (token.empty()) return true;

        auto wide_target = ToWide(MakeCredentialEntryTarget(config_.credential_target, suffix));
        auto wide_user   = ToWide(user_id_);

        CREDENTIALW cred{};
        cred.Type                = CRED_TYPE_GENERIC;
        cred.TargetName          = const_cast<LPWSTR>(wide_target.c_str());
        cred.UserName            = const_cast<LPWSTR>(wide_user.c_str());
        cred.CredentialBlobSize  = static_cast<DWORD>(token.size());
        cred.CredentialBlob      = reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));
        cred.Persist             = CRED_PERSIST_LOCAL_MACHINE;

        return CredWriteW(&cred, 0) == TRUE;
    };

    return save("access", access_token_) && save("refresh", refresh_token_);
}

bool AuthClient::LoadStoredTokens()
{
    std::lock_guard lock(mutex_);
    user_id_.clear();
    access_token_.clear();
    refresh_token_.clear();

    for (const auto& credential_target : BuildCredentialTargetSearchOrder(config_)) {
        auto stored = ReadStoredTokensForTarget(credential_target);
        if (!stored) {
            continue;
        }

        user_id_ = stored->user_id;
        access_token_ = stored->access_token;
        refresh_token_ = stored->refresh_token;

        if (credential_target != config_.credential_target) {
            if (SaveTokens()) {
                DeleteStoredTokensForTarget(credential_target);
            }
        }
        return true;
    }

    return false;
}

bool AuthClient::ClearStoredTokens()
{
    for (const auto& credential_target : BuildCredentialTargetSearchOrder(config_)) {
        DeleteStoredTokensForTarget(credential_target);
    }
    return true;
}

// ─── HTTP POST via WinHTTP ──────────────────────────────────────────────────

AuthClient::HttpResponse AuthClient::HttpPost(const std::string& path,
                                               const std::string& json_body)
{
    HttpResponse result;

    HINTERNET session = WinHttpOpen(
        L"RemoteX/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    auto wide_host = ToWide(config_.api_host);
    HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
    if (!conn) { WinHttpCloseHandle(session); return result; }

    auto wide_path = ToWide(path);
    DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(
        conn, L"POST", wide_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

    // Content-Type header.
    WinHttpAddRequestHeaders(req, L"Content-Type: application/json",
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    // Enviar.
    BOOL ok = WinHttpSendRequest(
        req,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)json_body.data(), (DWORD)json_body.size(),
        (DWORD)json_body.size(), 0);

    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    if (ok) {
        // Ler status code.
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        result.status_code = static_cast<int>(status);

        // Ler body.
        DWORD bytes_available = 0;
        while (WinHttpQueryDataAvailable(req, &bytes_available) && bytes_available > 0) {
            std::string chunk(bytes_available, 0);
            DWORD bytes_read = 0;
            WinHttpReadData(req, chunk.data(), bytes_available, &bytes_read);
            result.body.append(chunk.data(), bytes_read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

AuthResult AuthClient::ParseAuthResponse(const HttpResponse& resp)
{
    AuthResult result;

    if (resp.status_code == 0) {
        result.error = "network error";
        return result;
    }

    if (resp.status_code == 200 || resp.status_code == 201) {
        result.success       = true;
        result.user_id       = JsonGetString(resp.body, "user_id");
        result.role          = JsonGetString(resp.body, "role");
        result.access_token  = JsonGetString(resp.body, "access_token");
        result.refresh_token = JsonGetString(resp.body, "refresh_token");
        result.expires_in    = JsonGetInt(resp.body, "expires_in");
    } else {
        result.error = JsonGetString(resp.body, "error");
        if (result.error.empty())
            result.error = "HTTP " + std::to_string(resp.status_code);
    }

    return result;
}

} // namespace remotex::core
