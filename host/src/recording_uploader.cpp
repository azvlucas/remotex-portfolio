// RemoteX — host/recording_uploader.cpp
// Upload de gravações de sessão para R2 via presigned URL.

#include "host/recording_uploader.h"
#include "core/log_redaction.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace remotex::host {

// ─── Log ──────────────────────────────────────────────────────────────────────

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const std::string sanitized = remotex::core::SanitizeLogMessage(buf);
    fprintf(stderr, "[RecordingUploader] %s\n", sanitized.c_str());
}

static void ApplyWinHttpTimeouts(HINTERNET session,
                                 const RecordingUploader::Config& config) {
    if (!session) {
        return;
    }

    if (!WinHttpSetTimeouts(session,
                            config.resolve_timeout_ms,
                            config.connect_timeout_ms,
                            config.send_timeout_ms,
                            config.receive_timeout_ms)) {
        Log("WARNING: WinHttpSetTimeouts failed: err=%lu", GetLastError());
    }
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────

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

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

struct ParsedUrl {
    std::string host;
    std::string path;
    uint16_t    port    = 443;
    bool        use_tls = true;
};

static bool ParseAbsoluteUrl(const std::string& url, ParsedUrl& out) {
    std::string work = url;
    if (work.rfind("https://", 0) == 0) {
        work.erase(0, 8);
        out.use_tls = true;
        out.port = 443;
    } else if (work.rfind("http://", 0) == 0) {
        work.erase(0, 7);
        out.use_tls = false;
        out.port = 80;
    } else {
        return false;
    }

    const auto slash_pos = work.find('/');
    std::string host_port = slash_pos == std::string::npos ? work : work.substr(0, slash_pos);
    out.path = slash_pos == std::string::npos ? "/" : work.substr(slash_pos);
    if (host_port.empty() || out.path.empty()) {
        return false;
    }

    const auto colon_pos = host_port.rfind(':');
    if (colon_pos != std::string::npos && host_port.find(':') == colon_pos) {
        try {
            out.port = static_cast<uint16_t>(std::stoi(host_port.substr(colon_pos + 1)));
        } catch (...) {
            return false;
        }
        out.host = host_port.substr(0, colon_pos);
    } else {
        out.host = host_port;
    }

    return !out.host.empty();
}

static std::string UnescapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'u' && i + 5 < s.size()) {
                // Decode \uXXXX (BMP only, sufficient for URL chars)
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
                    i += 5; // skip \uXXXX
                    continue;
                }
            }
            // Other escapes: \n, \t, \\, \", etc.
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

std::string RecordingUploader::JsonGetString(const std::string& json, const std::string& key) {
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
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

RecordingUploader::RecordingUploader() = default;

RecordingUploader::~RecordingUploader() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RecordingUploader::Initialize(const Config& config, remotex::core::AuthClient* auth) {
    config_ = config;
    auth_   = auth;
}

// ─── UploadAsync ─────────────────────────────────────────────────────────────

void RecordingUploader::UploadAsync(const std::filesystem::path& mp4_path,
                                     const std::string& session_id,
                                     int duration_sec) {
    if (thread_.joinable()) thread_.join();
    uploading_.store(true);
    thread_ = std::thread(&RecordingUploader::UploadWorker, this,
                          mp4_path, session_id, duration_sec);
}

// ─── UploadWorker ────────────────────────────────────────────────────────────

void RecordingUploader::UploadWorker(std::filesystem::path mp4_path,
                                      std::string session_id,
                                      int duration_sec) {
    Log("Starting upload file=%s session_id=%s duration=%ds",
        mp4_path.string().c_str(), session_id.c_str(), duration_sec);

    // 1. Obter tamanho do arquivo.
    std::error_code ec;
    auto file_size = std::filesystem::file_size(mp4_path, ec);
    if (ec || file_size == 0) {
        Log("Failed to get file size or empty file: file=%s", mp4_path.string().c_str());
        uploading_.store(false);
        return;
    }

    // 2. POST /recordings/upload/{machine_id} → presigned URL + recording ID.
    std::ostringstream body;
    body << "{\"file_size\":" << file_size
         << ",\"duration_sec\":" << duration_sec;
    if (!session_id.empty()) {
        body << ",\"session_id\":\"" << EscapeJson(session_id) << "\"";
    }
    body << "}";

    auto resp = HttpRequest("POST",
                            "/recordings/upload/" + config_.machine_id,
                            body.str());
    if (resp.status_code != 200) {
        Log("Failed to get presigned URL: HTTP %d body=%s",
            resp.status_code, resp.body.c_str());
        uploading_.store(false);
        return;
    }

    std::string upload_url  = JsonGetString(resp.body, "url");
    std::string recording_id = JsonGetString(resp.body, "id");
    if (upload_url.empty() || recording_id.empty()) {
        Log("Invalid response: missing url or id");
        uploading_.store(false);
        return;
    }

    // 3. PUT arquivo no presigned URL.
    auto put_resp = HttpPut(upload_url, mp4_path);
    if (put_resp.status_code < 200 || put_resp.status_code >= 300) {
        Log("Failed to upload file: HTTP %d body=%s",
            put_resp.status_code, put_resp.body.c_str());
        uploading_.store(false);
        return;
    }

    // 4. PUT /recordings/confirm/{id} → marca upload completo.
    auto confirm_resp = HttpRequest("PUT", "/recordings/confirm/" + recording_id, "");
    if (confirm_resp.status_code != 200 && confirm_resp.status_code != 204) {
        Log("Failed to confirm upload: HTTP %d body=%s",
            confirm_resp.status_code, confirm_resp.body.c_str());
        uploading_.store(false);
        return;
    }

    // 5. Remove arquivo local.
    std::filesystem::remove(mp4_path, ec);
    Log("Upload complete, local file removed: file=%s", mp4_path.string().c_str());

    uploading_.store(false);
}

// ─── HTTP helpers (WinHTTP) ──────────────────────────────────────────────────

RecordingUploader::HttpResponse RecordingUploader::HttpRequest(
    const std::string& method,
    const std::string& path,
    const std::string& json_body) {

    auto attempt = [&]() -> HttpResponse {
        HttpResponse result;

        const std::wstring host_w = ToWide(config_.api_host);
        const std::wstring path_w = ToWide(path);
        const std::wstring method_w = ToWide(method);

        HINTERNET session = WinHttpOpen(L"RemoteX-RecordingUploader/1.0",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;
        ApplyWinHttpTimeouts(session, config_);

        HINTERNET connect = WinHttpConnect(session, host_w.c_str(),
                                            config_.api_port, 0);
        if (!connect) {
            WinHttpCloseHandle(session);
            return result;
        }

        DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, method_w.c_str(),
                                                path_w.c_str(),
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;
        }

        if (auth_) {
            std::string token = auth_->GetAccessToken();
            if (!token.empty()) {
                std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
                WinHttpAddRequestHeaders(request, auth_header.c_str(),
                                          static_cast<DWORD>(-1),
                                          WINHTTP_ADDREQ_FLAG_ADD);
            }
        }

        const wchar_t* content_type = L"Content-Type: application/json";
        WinHttpAddRequestHeaders(request, content_type,
                                  static_cast<DWORD>(wcslen(content_type)),
                                  WINHTTP_ADDREQ_FLAG_ADD);

        BOOL ok = WinHttpSendRequest(request,
                                      WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      json_body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                        : (LPVOID)json_body.c_str(),
                                      static_cast<DWORD>(json_body.size()),
                                      static_cast<DWORD>(json_body.size()), 0);

        if (ok) {
            ok = WinHttpReceiveResponse(request, nullptr);
        }
        if (!ok) {
            const DWORD err = GetLastError();
            result.body = "winhttp_error=" + std::to_string(err);
            Log("HTTP %s path=%s failed: err=%lu",
                method.c_str(),
                path.c_str(),
                static_cast<unsigned long>(err));
        }

        if (ok) {
            DWORD status = 0, size = sizeof(DWORD);
            WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            result.status_code = static_cast<int>(status);

            DWORD bytes_available = 0;
            while (WinHttpQueryDataAvailable(request, &bytes_available) && bytes_available > 0) {
                std::vector<char> buf(bytes_available);
                DWORD bytes_read = 0;
                WinHttpReadData(request, buf.data(), bytes_available, &bytes_read);
                result.body.append(buf.data(), bytes_read);
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
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

RecordingUploader::HttpResponse RecordingUploader::HttpPut(
    const std::string& url,
    const std::filesystem::path& file_path) {

    HttpResponse result;

    // Lê o arquivo inteiro.
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return result;
    std::vector<char> file_data((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
    ifs.close();

    ParsedUrl parsed;
    if (!ParseAbsoluteUrl(url, parsed)) {
        Log("Failed to parse presigned URL");
        return result;
    }

    HINTERNET session = WinHttpOpen(L"RemoteX-RecordingUploader/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    ApplyWinHttpTimeouts(session, config_);

    const std::wstring host_w = ToWide(parsed.host);
    const std::wstring path_w = ToWide(parsed.path);
    HINTERNET connect = WinHttpConnect(session, host_w.c_str(), parsed.port, 0);
    if (!connect) { WinHttpCloseHandle(session); return result; }

    DWORD flags = parsed.use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"PUT", path_w.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    const wchar_t* ct = L"Content-Type: video/mp4";
    WinHttpAddRequestHeaders(request, ct, static_cast<DWORD>(wcslen(ct)),
                              WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(request,
                                  WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  (LPVOID)file_data.data(),
                                  static_cast<DWORD>(file_data.size()),
                                  static_cast<DWORD>(file_data.size()), 0);

    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
        const DWORD err = GetLastError();
        result.body = "winhttp_error=" + std::to_string(err);
        Log("HTTP PUT %s failed: err=%lu",
            url.c_str(),
            static_cast<unsigned long>(err));
    }

    if (ok) {
        DWORD status = 0, size = sizeof(DWORD);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        result.status_code = static_cast<int>(status);

        DWORD bytes_available = 0;
        while (WinHttpQueryDataAvailable(request, &bytes_available) && bytes_available > 0) {
            std::vector<char> buf(bytes_available);
            DWORD bytes_read = 0;
            WinHttpReadData(request, buf.data(), bytes_available, &bytes_read);
            result.body.append(buf.data(), bytes_read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

} // namespace remotex::host
