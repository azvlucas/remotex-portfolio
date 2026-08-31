// RemoteX - core/machine_client.cpp
// Cliente HTTP para gestao de maquinas e grupos via WinHTTP.

#include "core/machine_client.h"
#include "core/auth_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <sstream>

namespace remotex::core {

static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string UnescapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'u' && i + 5 < s.size()) {
                unsigned int cp = 0;
                bool valid = true;
                for (int j = 0; j < 4; ++j) {
                    char c = s[i + 2 + j];
                    cp <<= 4;
                    if (c >= '0' && c <= '9')      cp |= (c - '0');
                    else if (c >= 'a' && c <= 'f')  cp |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')  cp |= (c - 'A' + 10);
                    else { valid = false; break; }
                }
                if (valid) {
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    i += 5;
                    continue;
                }
            }
            char next = s[i + 1];
            switch (next) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                default: out += '\\'; out += next; break;
            }
            ++i;
        } else {
            out += s[i];
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
        return UnescapeJsonString(json.substr(pos + 1, end - pos - 1));
    }
    if (json.compare(pos, 4, "null") == 0) {
        return "";
    }
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

static bool JsonGetBool(const std::string& json, const std::string& key) {
    auto val = JsonGetString(json, key);
    return val == "true";
}

static int JsonGetInt(const std::string& json, const std::string& key) {
    const auto value = JsonGetString(json, key);
    if (value.empty()) return 0;
    try {
        return std::stoi(value);
    } catch (...) {
        return 0;
    }
}

static uint64_t JsonGetUInt64(const std::string& json, const std::string& key) {
    const auto value = JsonGetString(json, key);
    if (value.empty()) return 0;
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (...) {
        return 0;
    }
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

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

void MachineClient::Configure(const MachineClientConfig& config, AuthClient* auth) {
    config_ = config;
    auth_   = auth;
}

std::vector<MachineInfo> MachineClient::ListMachines() {
    auto resp = HttpRequest("GET", "/machines");
    std::vector<MachineInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseMachineJson(obj));
    }
    return result;
}

std::optional<MachineInfo> MachineClient::RegisterMachine(const std::string& name,
                                                          const std::string& os_info) {
    std::string body = "{\"name\":\"" + EscapeJson(name) + "\"";
    if (!os_info.empty())
        body += ",\"os_info\":\"" + EscapeJson(os_info) + "\"";
    body += "}";

    auto resp = HttpRequest("POST", "/machines", body);
    if (resp.status_code == 201)
        return ParseMachineJson(resp.body);
    return std::nullopt;
}

std::optional<MachineInfo> MachineClient::UpdateMachine(const std::string& id,
                                                        const std::string& name,
                                                        const std::string& os_info,
                                                        const std::string& mac_address) {
    MachineUpdateRequest request;
    request.name = name;
    request.os_info = os_info;
    request.mac_address = mac_address;

    auto result = UpdateMachineDetails(id, request);
    return result.machine;
}

MachineInfoResult MachineClient::UpdateMachineDetails(const std::string& id,
                                                      const MachineUpdateRequest& request) {
    std::ostringstream body;
    body << "{"
         << "\"name\":\"" << EscapeJson(request.name) << "\","
         << "\"os_info\":\"" << EscapeJson(request.os_info) << "\","
         << "\"mac_address\":\"" << EscapeJson(request.mac_address) << "\","
         << "\"group_id\":\"" << EscapeJson(request.group_id) << "\","
         << "\"subgroup_id\":\"" << EscapeJson(request.subgroup_id) << "\","
         << "\"legal_name\":\"" << EscapeJson(request.legal_name) << "\","
         << "\"trade_name\":\"" << EscapeJson(request.trade_name) << "\","
         << "\"document_type\":\"" << EscapeJson(request.document_type) << "\","
         << "\"document_number\":\"" << EscapeJson(request.document_number) << "\","
         << "\"managers_only_access\":" << (request.managers_only_access ? "true" : "false") << ","
         << "\"require_client_consent\":" << (request.require_client_consent ? "true" : "false")
         << "}";

    auto resp = HttpRequest("PUT", "/machines/" + id, body.str());
    MachineInfoResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 200) {
        result.machine = ParseMachineJson(resp.body);
    }
    return result;
}

MachineResult MachineClient::DeleteMachine(const std::string& id) {
    auto resp = HttpRequest("DELETE", "/machines/" + id);
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

std::vector<MachineGroupInfo> MachineClient::ListGroups() {
    auto resp = HttpRequest("GET", "/machine-groups");
    std::vector<MachineGroupInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseGroupJson(obj));
    }
    return result;
}

MachineGroupResult MachineClient::CreateGroup(const std::string& name) {
    auto resp = HttpRequest("POST", "/machine-groups",
        "{\"name\":\"" + EscapeJson(name) + "\"}");

    MachineGroupResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 201) {
        result.group = ParseGroupJson(resp.body);
    }
    return result;
}

MachineGroupResult MachineClient::UpdateGroup(const std::string& id, const std::string& name) {
    auto resp = HttpRequest("PUT", "/machine-groups/" + id,
        "{\"name\":\"" + EscapeJson(name) + "\"}");

    MachineGroupResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 200) {
        result.group = ParseGroupJson(resp.body);
    }
    return result;
}

MachineResult MachineClient::DeleteGroup(const std::string& id) {
    auto resp = HttpRequest("DELETE", "/machine-groups/" + id);
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

std::vector<MachineSubgroupInfo> MachineClient::ListSubgroups(const std::string& group_id) {
    auto resp = HttpRequest("GET", "/machine-groups/" + group_id + "/subgroups");
    std::vector<MachineSubgroupInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseSubgroupJson(obj));
    }
    return result;
}

MachineSubgroupResult MachineClient::CreateSubgroup(const std::string& group_id,
                                                    const std::string& name) {
    auto resp = HttpRequest("POST", "/machine-groups/" + group_id + "/subgroups",
        "{\"name\":\"" + EscapeJson(name) + "\"}");

    MachineSubgroupResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 201) {
        result.subgroup = ParseSubgroupJson(resp.body);
    }
    return result;
}

MachineSubgroupResult MachineClient::UpdateSubgroup(const std::string& id,
                                                    const std::string& name) {
    auto resp = HttpRequest("PUT", "/machine-subgroups/" + id,
        "{\"name\":\"" + EscapeJson(name) + "\"}");

    MachineSubgroupResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 200) {
        result.subgroup = ParseSubgroupJson(resp.body);
    }
    return result;
}

MachineResult MachineClient::DeleteSubgroup(const std::string& id) {
    auto resp = HttpRequest("DELETE", "/machine-subgroups/" + id);
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

SessionInfoResult MachineClient::StartSession(const std::string& machine_id,
                                              SessionAccessMode access_mode) {
    auto resp = HttpRequest("POST", "/sessions",
        "{\"machine_id\":\"" + EscapeJson(machine_id) + "\","
        "\"access_mode\":\"" + EscapeJson(std::string(SessionAccessModeToString(access_mode))) + "\"}");

    SessionInfoResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 201) {
        result.session = ParseSessionJson(resp.body);
    }
    return result;
}

SessionInfoResult MachineClient::GetSession(const std::string& session_id) {
    auto resp = HttpRequest("GET", "/sessions/" + session_id);

    SessionInfoResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 200) {
        result.session = ParseSessionJson(resp.body);
    }
    return result;
}

SessionInfoResult MachineClient::ResolveSessionConsent(const std::string& session_id,
                                                       const std::string& decision,
                                                       const std::string& machine_key) {
    std::string body = "{\"decision\":\"" + EscapeJson(decision) + "\"";
    if (!machine_key.empty()) {
        body += ",\"machine_key\":\"" + EscapeJson(machine_key) + "\"";
    }
    body += "}";

    auto resp = HttpRequest("POST", "/sessions/" + session_id + "/consent",
        body);

    SessionInfoResult result;
    result.http_code = resp.status_code;
    result.error = JsonGetString(resp.body, "error");
    if (resp.status_code == 200) {
        result.session = ParseSessionJson(resp.body);
    }
    return result;
}

MachineResult MachineClient::EndSession(const std::string& session_id) {
    auto resp = HttpRequest("PUT", "/sessions/" + session_id);
    if (resp.status_code == 200) {
        return {true, ""};
    }
    return {false, JsonGetString(resp.body, "error")};
}

MachineResult MachineClient::SessionHeartbeat(const std::string& session_id) {
    auto resp = HttpRequest("PUT", "/sessions/" + session_id + "/heartbeat");
    if (resp.status_code == 204 || resp.status_code == 200) {
        return {true, ""};
    }
    return {false, JsonGetString(resp.body, "error")};
}

std::vector<MachineAuditEvent> MachineClient::ListMachineAudit(const std::string& machine_id,
                                                               int limit,
                                                               int offset) {
    std::ostringstream path;
    path << "/machines/" << machine_id << "/audit?limit=" << limit << "&offset=" << offset;

    auto resp = HttpRequest("GET", path.str());
    std::vector<MachineAuditEvent> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body)) {
            result.push_back(ParseAuditEventJson(obj));
        }
    }
    return result;
}

MachineResult MachineClient::CreateMachineAuditEvent(const std::string& machine_id,
                                                     const MachineAuditEventRequest& request) {
    std::ostringstream body;
    body << "{"
         << "\"event_type\":\"" << EscapeJson(request.event_type) << "\"";
    if (!request.session_id.empty())
        body << ",\"session_id\":\"" << EscapeJson(request.session_id) << "\"";
    if (!request.status.empty())
        body << ",\"status\":\"" << EscapeJson(request.status) << "\"";
    if (!request.summary.empty())
        body << ",\"summary\":\"" << EscapeJson(request.summary) << "\"";
    if (!request.transfer_direction.empty())
        body << ",\"transfer_direction\":\"" << EscapeJson(request.transfer_direction) << "\"";
    if (!request.transfer_root.empty())
        body << ",\"transfer_root\":\"" << EscapeJson(request.transfer_root) << "\"";
    if (!request.transfer_item.empty())
        body << ",\"transfer_item\":\"" << EscapeJson(request.transfer_item) << "\"";
    if (request.transfer_total_bytes > 0)
        body << ",\"transfer_total_bytes\":" << request.transfer_total_bytes;
    if (request.transfer_total_entries > 0)
        body << ",\"transfer_total_entries\":" << request.transfer_total_entries;
    if (request.duration_sec > 0)
        body << ",\"duration_sec\":" << request.duration_sec;
    if (!request.error_message.empty())
        body << ",\"error_message\":\"" << EscapeJson(request.error_message) << "\"";
    body << "}";

    auto resp = HttpRequest("POST", "/machines/" + machine_id + "/audit", body.str());
    if (resp.status_code == 201) {
        return {true, ""};
    }
    return {false, JsonGetString(resp.body, "error")};
}

std::optional<MachineStatus> MachineClient::GetMachineStatus(const std::string& id) {
    auto result = GetMachineStatusEx(id);
    return result.status;
}

MachineClient::StatusResult MachineClient::GetMachineStatusEx(const std::string& id) {
    StatusResult result;
    auto resp = HttpRequest("GET", "/machines/" + id + "/status");
    result.http_code = resp.status_code;
    if (resp.status_code == 200) {
        MachineStatus status;
        status.id             = JsonGetString(resp.body, "id");
        status.is_online      = JsonGetBool(resp.body, "is_online");
        status.last_heartbeat = JsonGetString(resp.body, "last_heartbeat");
        result.status = status;
    }
    return result;
}

MachineClient::HeartbeatResult MachineClient::SendHeartbeat(const std::string& machine_id,
                                                            const std::string& machine_key,
                                                            int64_t uptime_sec,
                                                            const std::string& agent_version) {
    std::ostringstream body;
    body << "{\"machine_key\":\"" << EscapeJson(machine_key) << "\"";
    if (uptime_sec > 0)
        body << ",\"uptime\":" << uptime_sec;
    if (!agent_version.empty())
        body << ",\"agent_version\":\"" << EscapeJson(agent_version) << "\"";
    body << "}";

    auto resp = HttpRequest("POST", "/machines/" + machine_id + "/heartbeat", body.str());
    if (resp.status_code == 200) {
        bool force = JsonGetBool(resp.body, "force_update");
        return {true, force, ""};
    }
    // Accept 204 for backward compat with older servers
    if (resp.status_code == 204)
        return {true, false, ""};
    return {false, false, JsonGetString(resp.body, "error")};
}

MachineResult MachineClient::RequestForceUpdate(const std::string& machine_id) {
    auto resp = HttpRequest("POST", "/machines/" + machine_id + "/force-update", "");
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

// ─── Recordings API ──────────────────────────────────────────────────────────

std::vector<RecordingInfo> MachineClient::ListRecordings(int limit, int offset) {
    std::ostringstream path;
    path << "/recordings?limit=" << limit << "&offset=" << offset;
    auto resp = HttpRequest("GET", path.str());
    std::vector<RecordingInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseRecordingJson(obj));
    }
    return result;
}

std::vector<RecordingInfo> MachineClient::ListRecordingsByMachine(const std::string& machine_id,
                                                                   int limit, int offset) {
    std::ostringstream path;
    path << "/recordings/machine/" << machine_id << "?limit=" << limit << "&offset=" << offset;
    auto resp = HttpRequest("GET", path.str());
    std::vector<RecordingInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseRecordingJson(obj));
    }
    return result;
}

RecordingDownloadLinkResult MachineClient::GetRecordingDownloadURL(const std::string& id) {
    auto resp = HttpRequest("GET", "/recordings/" + id + "/download");
    RecordingDownloadLinkResult result;
    result.http_code = resp.status_code;
    if (resp.status_code == 200) {
        result.url = JsonGetString(resp.body, "url");
        if (result.url.empty()) {
            result.error = "missing download url";
        }
        return result;
    }

    result.error = JsonGetString(resp.body, "error");
    if (result.error.empty()) {
        result.error = "failed to get recording download url";
    }
    return result;
}

MachineResult MachineClient::DeleteRecording(const std::string& id) {
    auto resp = HttpRequest("DELETE", "/recordings/" + id);
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

// ─── Company Users API ──────────────────────────────────────────────────────

std::vector<CompanyUserInfo> MachineClient::ListCompanyUsers() {
    auto resp = HttpRequest("GET", "/users/company/");
    std::vector<CompanyUserInfo> result;
    if (resp.status_code == 200) {
        for (const auto& obj : SplitJsonArray(resp.body))
            result.push_back(ParseCompanyUserJson(obj));
    }
    return result;
}

std::optional<CompanyUserInfo> MachineClient::CreateCompanyUser(const std::string& name,
                                                                  const std::string& email,
                                                                  const std::string& password) {
    std::string body = "{\"name\":\"" + EscapeJson(name) +
                       "\",\"email\":\"" + EscapeJson(email) +
                       "\",\"password\":\"" + EscapeJson(password) + "\"}";
    auto resp = HttpRequest("POST", "/users/company/", body);
    if (resp.status_code == 201)
        return ParseCompanyUserJson(resp.body);
    return std::nullopt;
}

MachineResult MachineClient::ActivateCompanyUser(const std::string& id) {
    auto resp = HttpRequest("PUT", "/users/company/" + id + "/activate");
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

MachineResult MachineClient::DeactivateCompanyUser(const std::string& id) {
    auto resp = HttpRequest("PUT", "/users/company/" + id + "/deactivate");
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

MachineResult MachineClient::ResetCompanyUserPassword(const std::string& id,
                                                        const std::string& new_password) {
    std::string body = "{\"password\":\"" + EscapeJson(new_password) + "\"}";
    auto resp = HttpRequest("PUT", "/users/company/" + id + "/reset-password", body);
    if (resp.status_code == 204)
        return {true, ""};
    return {false, JsonGetString(resp.body, "error")};
}

// ─── Parse helpers ──────────────────────────────────────────────────────────

RecordingInfo MachineClient::ParseRecordingJson(const std::string& json) {
    RecordingInfo r;
    r.id           = JsonGetString(json, "id");
    r.session_id   = JsonGetString(json, "session_id");
    r.machine_id   = JsonGetString(json, "machine_id");
    r.file_size    = static_cast<int64_t>(JsonGetUInt64(json, "file_size"));
    r.duration_sec = JsonGetInt(json, "duration_sec");
    r.status       = JsonGetString(json, "status");
    r.created_at   = JsonGetString(json, "created_at");
    return r;
}

CompanyUserInfo MachineClient::ParseCompanyUserJson(const std::string& json) {
    CompanyUserInfo u;
    u.id         = JsonGetString(json, "id");
    u.name       = JsonGetString(json, "name");
    u.email      = JsonGetString(json, "email");
    u.role       = JsonGetString(json, "role");
    u.is_active  = JsonGetBool(json, "is_active");
    u.created_at = JsonGetString(json, "created_at");
    return u;
}

MachineClient::HttpResponse MachineClient::HttpRequest(const std::string& method,
                                                       const std::string& path,
                                                       const std::string& json_body) {
    auto attempt = [&]() -> HttpResponse {
        HttpResponse result;

        HINTERNET session = WinHttpOpen(L"RemoteX/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;

        // 5s resolve, 5s connect, 5s send, 10s receive — prevents blocking
        // the caller indefinitely when the server is slow or unreachable.
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

        auto wide_host = ToWide(config_.api_host);
        HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
        if (!conn) { WinHttpCloseHandle(session); return result; }

        auto wide_path = ToWide(path);
        auto wide_method = ToWide(method);
        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET req = WinHttpOpenRequest(conn, wide_method.c_str(), wide_path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

        WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1,
            WINHTTP_ADDREQ_FLAG_ADD);

        if (auth_) {
            std::string token = auth_->GetAccessToken();
            if (!token.empty()) {
                std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
                WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                    WINHTTP_ADDREQ_FLAG_ADD);
            }
        }

        LPVOID body_ptr = json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data();
        DWORD body_len = (DWORD)json_body.size();

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            body_ptr, body_len, body_len, 0);
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

MachineInfo MachineClient::ParseMachineJson(const std::string& json) {
    MachineInfo m;
    m.id              = JsonGetString(json, "id");
    m.name            = JsonGetString(json, "name");
    m.machine_key     = JsonGetString(json, "machine_key");
    m.os_info         = JsonGetString(json, "os_info");
    m.mac_address     = JsonGetString(json, "mac_address");
    m.agent_version   = JsonGetString(json, "agent_version");
    m.last_heartbeat  = JsonGetString(json, "last_heartbeat");
    m.created_at      = JsonGetString(json, "created_at");
    m.group_id        = JsonGetString(json, "group_id");
    m.group_name      = JsonGetString(json, "group_name");
    m.subgroup_id     = JsonGetString(json, "subgroup_id");
    m.subgroup_name   = JsonGetString(json, "subgroup_name");
    m.legal_name      = JsonGetString(json, "legal_name");
    m.trade_name      = JsonGetString(json, "trade_name");
    m.document_type   = JsonGetString(json, "document_type");
    m.document_number = JsonGetString(json, "document_number");
    m.state           = JsonGetString(json, "state");
    m.block_reason    = JsonGetString(json, "block_reason");
    m.managers_only_access = JsonGetBool(json, "managers_only_access");
    m.require_client_consent = JsonGetBool(json, "require_client_consent");
    m.is_online       = JsonGetBool(json, "is_online");
    return m;
}

MachineGroupInfo MachineClient::ParseGroupJson(const std::string& json) {
    MachineGroupInfo group;
    group.id = JsonGetString(json, "id");
    group.name = JsonGetString(json, "name");
    return group;
}

MachineSubgroupInfo MachineClient::ParseSubgroupJson(const std::string& json) {
    MachineSubgroupInfo subgroup;
    subgroup.id = JsonGetString(json, "id");
    subgroup.group_id = JsonGetString(json, "group_id");
    subgroup.name = JsonGetString(json, "name");
    return subgroup;
}

SessionInfo MachineClient::ParseSessionJson(const std::string& json) {
    SessionInfo session;
    session.id = JsonGetString(json, "id");
    session.user_id = JsonGetString(json, "user_id");
    session.company_id = JsonGetString(json, "company_id");
    session.machine_id = JsonGetString(json, "machine_id");
    session.access_mode = SessionAccessModeFromString(JsonGetString(json, "access_mode"));
    session.status = JsonGetString(json, "status");
    session.started_at = JsonGetString(json, "started_at");
    session.ended_at = JsonGetString(json, "ended_at");
    session.requester_name = JsonGetString(json, "requester_name");
    if (json.find("\"duration_sec\"") != std::string::npos) {
        session.duration_sec = JsonGetInt(json, "duration_sec");
        session.has_duration = true;
    }
    session.relay_protocol_version = JsonGetInt(json, "relay_protocol_version");
    session.relay_session_id = JsonGetString(json, "relay_session_id");
    session.relay_role = JsonGetString(json, "relay_role");
    session.relay_ticket = JsonGetString(json, "relay_ticket");
    session.relay_ticket_expires_unix = static_cast<int64_t>(JsonGetUInt64(json, "relay_ticket_expires_unix"));
    session.relay_host = JsonGetString(json, "relay_host");
    session.relay_port = static_cast<uint16_t>(JsonGetInt(json, "relay_port"));
    return session;
}

MachineAuditEvent MachineClient::ParseAuditEventJson(const std::string& json) {
    MachineAuditEvent event;
    event.id = JsonGetString(json, "id");
    event.machine_id = JsonGetString(json, "machine_id");
    event.session_id = JsonGetString(json, "session_id");
    event.user_id = JsonGetString(json, "user_id");
    event.user_name = JsonGetString(json, "user_name");
    event.event_type = JsonGetString(json, "event_type");
    event.status = JsonGetString(json, "status");
    event.summary = JsonGetString(json, "summary");
    event.transfer_direction = JsonGetString(json, "transfer_direction");
    event.transfer_root = JsonGetString(json, "transfer_root");
    event.transfer_item = JsonGetString(json, "transfer_item");
    event.transfer_total_bytes = JsonGetUInt64(json, "transfer_total_bytes");
    event.transfer_total_entries = static_cast<uint32_t>(JsonGetInt(json, "transfer_total_entries"));
    if (json.find("\"duration_sec\"") != std::string::npos) {
        event.duration_sec = JsonGetInt(json, "duration_sec");
        event.has_duration = true;
    }
    event.error_message = JsonGetString(json, "error_message");
    event.created_at = JsonGetString(json, "created_at");
    return event;
}

// ── Reports API ──────────────────────────────────────────────────────

// Extrai o conteudo de um array JSON aninhado por chave.
// Ex: JsonGetArray(json, "by_machine") retorna "[{...},{...}]"
static std::string JsonGetArray(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "[]";
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return "[]";
    int depth = 0;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']') {
            --depth;
            if (depth == 0)
                return json.substr(pos, i - pos + 1);
        }
    }
    return "[]";
}

ReportDashboardData MachineClient::GetReportDashboard(const std::string& date_from,
                                                       const std::string& date_to,
                                                       const std::string& tz) {
    ReportDashboardData result;
    std::string path = "/reports/dashboard?date_from=" + date_from + "&date_to=" + date_to;
    if (!tz.empty()) {
        path += "&tz=" + tz;
    }
    auto resp = HttpRequest("GET", path);
    if (resp.status_code != 200) {
        result.error = JsonGetString(resp.body, "error");
        if (result.error.empty())
            result.error = "HTTP " + std::to_string(resp.status_code);
        return result;
    }

    result.total_sessions = JsonGetInt(resp.body, "total_sessions");
    result.total_duration_sec = JsonGetInt(resp.body, "total_duration_sec");
    result.avg_duration_sec = JsonGetInt(resp.body, "avg_duration_sec");

    // by_machine
    for (const auto& obj : SplitJsonArray(JsonGetArray(resp.body, "by_machine"))) {
        ReportMachineStats ms;
        ms.machine_id = JsonGetString(obj, "machine_id");
        ms.machine_name = JsonGetString(obj, "machine_name");
        ms.session_count = JsonGetInt(obj, "session_count");
        ms.total_duration_sec = JsonGetInt(obj, "total_duration_sec");
        result.by_machine.push_back(std::move(ms));
    }

    // by_technician
    for (const auto& obj : SplitJsonArray(JsonGetArray(resp.body, "by_technician"))) {
        ReportTechnicianStats ts;
        ts.user_id = JsonGetString(obj, "user_id");
        ts.user_name = JsonGetString(obj, "user_name");
        ts.session_count = JsonGetInt(obj, "session_count");
        ts.total_duration_sec = JsonGetInt(obj, "total_duration_sec");
        result.by_technician.push_back(std::move(ts));
    }

    // daily
    for (const auto& obj : SplitJsonArray(JsonGetArray(resp.body, "daily"))) {
        ReportDailyStats ds;
        ds.day = JsonGetString(obj, "day");
        ds.session_count = JsonGetInt(obj, "session_count");
        ds.total_duration_sec = JsonGetInt(obj, "total_duration_sec");
        result.daily.push_back(std::move(ds));
    }

    return result;
}

} // namespace remotex::core
