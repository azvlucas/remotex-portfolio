// RemoteX — core/turn_client.cpp
// Cliente HTTP para credenciais TURN e lista de servidores.

#include "core/turn_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace remotex::core {

// ─── JSON helpers mínimos ───────────────────────────────────────────────────

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

static int JsonGetInt(const std::string& json, const std::string& key) {
    auto val = JsonGetString(json, key);
    if (val.empty()) return 0;
    try { return std::stoi(val); } catch (...) { return 0; }
}

static bool JsonGetBool(const std::string& json, const std::string& key) {
    return JsonGetString(json, key) == "true";
}

static std::vector<std::string> JsonGetStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return result;
    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;
    std::string arr = json.substr(pos + 1, end - pos - 1);
    size_t i = 0;
    while (i < arr.size()) {
        auto q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        auto q2 = q1 + 1;
        while (q2 < arr.size()) {
            if (arr[q2] == '\\') { q2 += 2; continue; }
            if (arr[q2] == '"') break;
            ++q2;
        }
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

// Extrai objetos JSON de um array: [{...},{...}]
static std::vector<std::string> SplitJsonArray(const std::string& json) {
    std::vector<std::string> items;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0)
                items.push_back(json.substr(start, i - start + 1));
        }
    }
    return items;
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

// ─── Configure ──────────────────────────────────────────────────────────────

void TurnClient::Configure(const TurnClientConfig& config, AuthClient* auth) {
    config_ = config;
    auth_   = auth;
}

// ─── FetchCredentials ───────────────────────────────────────────────────────

bool TurnClient::FetchCredentials(const std::string& user_id, TurnCredentials& out) {
    std::string path = "/turn/credentials";
    if (!user_id.empty())
        path += "?user_id=" + user_id;

    auto resp = HttpGet(path);
    if (resp.status_code != 200) return false;

    out.username = JsonGetString(resp.body, "username");
    out.password = JsonGetString(resp.body, "password");
    out.ttl      = JsonGetInt(resp.body, "ttl");
    out.uris     = JsonGetStringArray(resp.body, "uris");

    return !out.username.empty();
}

// ─── FetchServers ───────────────────────────────────────────────────────────

bool TurnClient::FetchServers(std::vector<TurnServerInfo>& out) {
    auto resp = HttpGet("/turn/servers");
    if (resp.status_code != 200) return false;

    out.clear();
    for (const auto& obj : SplitJsonArray(resp.body)) {
        TurnServerInfo info;
        info.host    = JsonGetString(obj, "host");
        info.port    = JsonGetInt(obj, "port");
        info.healthy = JsonGetBool(obj, "healthy");
        if (!info.host.empty())
            out.push_back(info);
    }
    return true;
}

// ─── CredentialsToIceServers ────────────────────────────────────────────────

std::vector<IceServer> TurnClient::CredentialsToIceServers(const TurnCredentials& creds) {
    std::vector<IceServer> servers;

    // Separar STUN e TURN URIs.
    IceServer stun_server;
    IceServer turn_server;
    turn_server.username   = creds.username;
    turn_server.credential = creds.password;

    for (const auto& uri : creds.uris) {
        if (uri.substr(0, 5) == "stun:") {
            stun_server.urls.push_back(uri);
        } else if (uri.substr(0, 5) == "turn:") {
            turn_server.urls.push_back(uri);
        }
    }

    if (!stun_server.urls.empty())
        servers.push_back(stun_server);
    if (!turn_server.urls.empty())
        servers.push_back(turn_server);

    return servers;
}

// ─── HTTP GET via WinHTTP ───────────────────────────────────────────────────

TurnClient::HttpResponse TurnClient::HttpGet(const std::string& path) {
    auto attempt = [&]() -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX-TURN/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) { WinHttpCloseHandle(session); return result; }

        auto wide_path = ToWide(path);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

        // Authorization Bearer token.
        if (auth_) {
            std::string token = auth_->GetAccessToken();
            if (!token.empty()) {
                std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
                WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                    WINHTTP_ADDREQ_FLAG_ADD);
            }
        }

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);

        if (ok) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            result.status_code = static_cast<int>(status);

            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, 0);
                DWORD read = 0;
                WinHttpReadData(req, chunk.data(), avail, &read);
                result.body.append(chunk.data(), read);
            }
        }

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return result;
    };

    HttpResponse resp = attempt();
    if (resp.status_code == 401 && auth_) {
        if (auth_->RefreshToken().success) {
            resp = attempt();
        }
    }
    return resp;
}

} // namespace remotex::core
