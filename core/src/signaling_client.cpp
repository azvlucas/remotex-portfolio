// RemoteX — core/signaling_client.cpp
// Cliente WebSocket via WinHTTP para sinalização WebRTC.

#include "core/signaling_client.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <future>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

// Fallback: SDKs mais antigos não definem este buffer type para WebSocket ping/pong.
#ifndef WINHTTP_WEB_SOCKET_PING_PONG_BUFFER_TYPE
#define WINHTTP_WEB_SOCKET_PING_PONG_BUFFER_TYPE 0x6
#endif

namespace remotex::core {

// ─── Helpers JSON mínimos (sem nlohmann em runtime obrigatório) ─────────────
// Produz JSON de forma simples para as mensagens de sinalização.
// Usamos string concatenation para evitar dependência hard de nlohmann/json.

static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Extrai o valor string de uma chave em um JSON plano (não-nested para campos simples).
static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    // Avançar até o ':'
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // Pular espaços
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] == '"') {
        // Valor string — encontrar o fechamento (simples, sem escape handling sofisticado).
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        return json.substr(pos + 1, end - pos - 1);
    }

    // Valor não-string (null, number, etc)
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

// Extrai o bloco JSON raw do campo "payload" (pode ser um objeto nested).
static std::string JsonGetPayload(const std::string& json) {
    const std::string key = "\"payload\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + key.size());
    if (pos == std::string::npos) return "";

    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] == '{') {
        // Contar chaves para encontrar o fim do objeto.
        int depth = 0;
        auto start = pos;
        for (auto i = pos; i < json.size(); ++i) {
            if (json[i] == '{') ++depth;
            else if (json[i] == '}') { --depth; if (depth == 0) return json.substr(start, i - start + 1); }
        }
    }

    if (json[pos] == '"') {
        auto end = pos + 1;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == '\\') ++end;
            ++end;
        }
        return json.substr(pos + 1, end - pos - 1);
    }

    if (json[pos] == 'n') return ""; // null

    return "";
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

static std::string TrimAsciiWhitespace(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

static bool IsValidBearerToken(const std::string& token) {
    if (token.empty()) {
        return false;
    }

    return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
        return ch > 0x20 && ch != 0x7f;
    });
}

static std::string AppendQueryParameter(std::string path,
                                        const std::string& key,
                                        const std::string& value) {
    path += (path.find('?') == std::string::npos) ? '?' : '&';
    path += key;
    path += '=';
    path += value;
    return path;
}

static std::string ReadHttpResponseBody(HINTERNET request) {
    constexpr size_t kMaxBodyBytes = 8 * 1024;

    std::string body;
    if (!request) {
        return body;
    }

    while (body.size() < kMaxBodyBytes) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }

        const size_t remaining = kMaxBodyBytes - body.size();
        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(available, remaining));
        std::string chunk(to_read, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, chunk.data(), to_read, &bytes_read) || bytes_read == 0) {
            break;
        }

        chunk.resize(bytes_read);
        body += chunk;
    }

    return TrimAsciiWhitespace(body);
}

static std::string BuildHttpStatusErrorMessage(DWORD status_code,
                                               const std::string& response_body) {
    std::string message = "HTTP " + std::to_string(status_code);

    std::string detail = TrimAsciiWhitespace(JsonGetString(response_body, "error"));
    if (detail.empty()) {
        detail = TrimAsciiWhitespace(response_body);
    }
    if (!detail.empty()) {
        message += " (" + detail + ")";
    }

    if (status_code == 401 &&
        detail.find("query-string bearer token is no longer supported") != std::string::npos) {
        message += " — backend expects Authorization bearer headers; upgrade outdated Viewer builds";
    }

    return message;
}

// ─── SignalingClient ────────────────────────────────────────────────────────

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient()
{
    Disconnect();
}

void SignalingClient::SetListener(SignalingListener* listener)
{
    listener_ = listener;
}

SignalingState SignalingClient::GetState() const
{
    return state_.load();
}

bool SignalingClient::Connect(const SignalingConfig& config)
{
    if (state_ != SignalingState::Disconnected)
        Disconnect();

    config_ = config;
    last_error_.clear();
    should_stop_ = false;
    native_ping_enabled_ = true;
    state_ = SignalingState::Connecting;

    conn_thread_ = std::thread(&SignalingClient::ConnectionThread, this);
    return true;
}

void SignalingClient::Disconnect()
{
    should_stop_ = true;
    state_ = SignalingState::Disconnected;

    {
        std::lock_guard lock(connect_mutex_);
        CloseHandles();
    }

    if (conn_thread_.joinable()) {
        // Timeout de 3s — se o thread travar, detach em vez de bloquear.
        auto future = std::async(std::launch::async, [this]{ conn_thread_.join(); });
        if (future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
            conn_thread_.detach();
        }
    }
}

// ─── Envio de mensagens ─────────────────────────────────────────────────────

void SignalingClient::JoinSession(const std::string& session_id, const std::string& role)
{
    SendSignalingMessage(signal_msg::SessionJoin, session_id, role);
}

void SignalingClient::LeaveSession()
{
    SendSignalingMessage(signal_msg::SessionLeave);
}

void SignalingClient::SendOffer(const std::string& session_id,
                                const std::string& sdp,
                                const std::string& access_mode,
                                const std::string& backend_session_id)
{
    std::string payload = "{\"sdp\":\"" + EscapeJson(sdp) + "\"";
    if (!access_mode.empty()) {
        payload += ",\"access_mode\":\"" + EscapeJson(access_mode) + "\"";
    }
    if (!backend_session_id.empty()) {
        payload += ",\"session_id\":\"" + EscapeJson(backend_session_id) + "\"";
    }
    payload += "}";
    SendSignalingMessage(signal_msg::Offer, session_id, "", payload);
}

void SignalingClient::SendAnswer(const std::string& session_id, const std::string& sdp)
{
    std::string payload = "{\"sdp\":\"" + EscapeJson(sdp) + "\"}";
    SendSignalingMessage(signal_msg::Answer, session_id, "", payload);
}

void SignalingClient::SendICECandidate(const std::string& session_id,
                                       const std::string& candidate,
                                       const std::string& sdp_mid,
                                       int sdp_mline_index)
{
    std::string payload = "{\"candidate\":\"" + EscapeJson(candidate) +
                          "\",\"sdp_mid\":\"" + EscapeJson(sdp_mid) +
                          "\",\"sdp_mline_index\":" + std::to_string(sdp_mline_index) + "}";
    SendSignalingMessage(signal_msg::ICECandidate, session_id, "", payload);
}

void SignalingClient::SendError(const std::string& session_id, const std::string& error)
{
    std::ostringstream oss;
    oss << "{\"type\":\"" << EscapeJson(signal_msg::Error) << "\"";
    if (!session_id.empty())
        oss << ",\"session_id\":\"" << EscapeJson(session_id) << "\"";
    oss << ",\"error\":\"" << EscapeJson(error) << "\"}";
    SendMessage(oss.str());
}

void SignalingClient::ChatJoin(const std::string& machine_id)
{
    std::ostringstream oss;
    oss << "{\"type\":\"" << EscapeJson(signal_msg::ChatJoin) << "\""
        << ",\"machine_id\":\"" << EscapeJson(machine_id) << "\"}";
    SendMessage(oss.str());
}

void SignalingClient::ChatLeave(const std::string& machine_id)
{
    std::ostringstream oss;
    oss << "{\"type\":\"" << EscapeJson(signal_msg::ChatLeave) << "\""
        << ",\"machine_id\":\"" << EscapeJson(machine_id) << "\"}";
    SendMessage(oss.str());
}

void SignalingClient::SendChatMessage(const std::string& machine_id,
                                       const std::string& from,
                                       const std::string& text)
{
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "{\"type\":\"" << EscapeJson(signal_msg::Chat) << "\""
        << ",\"machine_id\":\"" << EscapeJson(machine_id) << "\""
        << ",\"payload\":{\"from\":\"" << EscapeJson(from)
        << "\",\"text\":\"" << EscapeJson(text)
        << "\",\"ts\":" << now << "}}";
    SendMessage(oss.str());
}

void SignalingClient::SendSignalingMessage(const std::string& type,
                                            const std::string& session_id,
                                            const std::string& role,
                                            const std::string& payload_json)
{
    std::ostringstream oss;
    oss << "{\"type\":\"" << EscapeJson(type) << "\"";
    if (!session_id.empty())
        oss << ",\"session_id\":\"" << EscapeJson(session_id) << "\"";
    if (!role.empty())
        oss << ",\"role\":\"" << EscapeJson(role) << "\"";
    if (!payload_json.empty())
        oss << ",\"payload\":" << payload_json; // payload já é JSON válido
    oss << "}";
    SendMessage(oss.str());
}

void SignalingClient::SendMessage(const std::string& json_str)
{
    DWORD err = NO_ERROR;
    {
        std::lock_guard send_lock(send_mutex_);
        std::lock_guard conn_lock(connect_mutex_);
        if (!websocket_ || state_ != SignalingState::Connected)
            return;

        err = WinHttpWebSocketSend(
            websocket_,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)json_str.data(),
            (DWORD)json_str.size());
    }

    if (err != NO_ERROR) {
        if (listener_)
            listener_->OnError("WebSocket send failed: " + std::to_string(err));
        std::lock_guard conn_lock(connect_mutex_);
        CloseHandles();
    }
}

// ─── Thread de conexão com reconexão ────────────────────────────────────────

void SignalingClient::ConnectionThread()
{
    uint32_t attempt = 0;

    while (!should_stop_) {
        state_ = (attempt == 0) ? SignalingState::Connecting : SignalingState::Reconnecting;

        {
            bool connected = false;
            {
                std::lock_guard lock(connect_mutex_);
                if (EstablishConnection()) {
                    attempt = 0;
                    state_ = SignalingState::Connected;
                    connected = true;
                }
            }
            // Notificar FORA do lock — o listener pode chamar JoinSession/SendMessage
            // que precisam adquirir connect_mutex_.
            if (connected && listener_)
                listener_->OnConnected();

            // Reportar falha na primeira tentativa para que o Viewer saiba.
            if (!connected && listener_ && attempt == 0) {
                std::string msg = "Signaling connection failed";
                if (!last_error_.empty())
                    msg += ": " + last_error_;
                listener_->OnError(msg);
            }
        }

        if (state_ == SignalingState::Connected) {
            // Iniciar thread de keepalive (ping a cada 30s).
            ping_thread_ = std::thread(&SignalingClient::PingThread, this);

            // ReadLoop bloqueia até a conexão cair.
            ReadLoop();

            // Parar ping thread.
            ping_cv_.notify_all();
            if (ping_thread_.joinable())
                ping_thread_.join();

            if (listener_ && !should_stop_)
                listener_->OnDisconnected();
        }

        if (should_stop_)
            break;

        // Verificar limite de tentativas.
        ++attempt;
        if (config_.max_reconnect_attempts > 0 && attempt >= config_.max_reconnect_attempts) {
            if (listener_)
                listener_->OnError("Max reconnection attempts reached");
            break;
        }

        // Backoff exponencial.
        uint32_t wait_ms = CalculateBackoff(attempt);
        // Dormir em intervalos curtos para checar should_stop_.
        for (uint32_t elapsed = 0; elapsed < wait_ms && !should_stop_; elapsed += 100)
            Sleep(100);
    }

    {
        std::lock_guard lock(connect_mutex_);
        CloseHandles();
    }

    state_ = SignalingState::Disconnected;
}

bool SignalingClient::EstablishConnection()
{
    bool force_refresh = false;
retry:
    CloseHandles();
    native_ping_enabled_ = true;

    // Criar sessão WinHTTP.
    session_handle_ = WinHttpOpen(
        L"RemoteX/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session_handle_)
        return false;

    // Conectar ao host.
    auto wide_host = ToWide(config_.host);
    connection_ = WinHttpConnect(
        session_handle_,
        wide_host.c_str(),
        config_.port,
        0);

    if (!connection_)
        return false;

    // Montar path da conexão. A autenticação segue no header Authorization.
    std::string token = config_.token_provider ? TrimAsciiWhitespace(config_.token_provider(force_refresh)) : "";
    if (token.empty()) {
        last_error_ = force_refresh ? "missing refreshed signaling token" : "missing signaling token";
        return false;
    }
    if (!IsValidBearerToken(token)) {
        last_error_ = force_refresh ? "invalid refreshed signaling token" : "invalid signaling token";
        return false;
    }

    std::string full_path = AppendQueryParameter(config_.path, "client_id", config_.client_id);
    auto wide_path = ToWide(full_path);

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (config_.use_tls)
        flags |= WINHTTP_FLAG_SECURE;

    request_ = WinHttpOpenRequest(
        connection_,
        L"GET",
        wide_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);

    if (!request_)
        return false;

    // Solicitar upgrade para WebSocket.
    BOOL ok = WinHttpSetOption(
        request_,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
        nullptr, 0);

    if (!ok)
        return false;

    std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
    ok = WinHttpAddRequestHeaders(
        request_,
        auth_header.c_str(),
        (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);
    if (!ok) {
        last_error_ = "WinHttpAddRequestHeaders failed: " + std::to_string(GetLastError());
        return false;
    }

    // Enviar o request HTTP.
    ok = WinHttpSendRequest(
        request_,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!ok) {
        last_error_ = "WinHttpSendRequest failed: " + std::to_string(GetLastError());
        return false;
    }

    // Receber a resposta do upgrade.
    ok = WinHttpReceiveResponse(request_, nullptr);
    if (!ok) {
        last_error_ = "WinHttpReceiveResponse failed: " + std::to_string(GetLastError());
        return false;
    }

    // Verificar status HTTP antes do upgrade.
    DWORD status_code = 0;
    DWORD sc_size = sizeof(status_code);
    WinHttpQueryHeaders(request_,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code, &sc_size, WINHTTP_NO_HEADER_INDEX);

    std::string response_body;
    if (status_code != 101) {
        response_body = ReadHttpResponseBody(request_);
    }

    if (status_code == 401 && !force_refresh && config_.token_provider) {
        force_refresh = true;
        goto retry;
    }

    if (status_code != 101) {
        last_error_ = BuildHttpStatusErrorMessage(status_code, response_body);
        return false;
    }

    // Completar o upgrade para WebSocket.
    websocket_ = WinHttpWebSocketCompleteUpgrade(request_, 0);
    if (!websocket_) {
        last_error_ = "WebSocket upgrade failed: " + std::to_string(GetLastError());
        return false;
    }

    // O request handle não é mais necessário após o upgrade.
    WinHttpCloseHandle(request_);
    request_ = nullptr;

    return true;
}

void SignalingClient::PingThread()
{
    constexpr auto kInterval = std::chrono::seconds(30);

    while (true) {
        std::unique_lock<std::mutex> lock(ping_mutex_);
        bool interrupted = ping_cv_.wait_for(lock, kInterval, [this] {
            return should_stop_.load() || state_ != SignalingState::Connected;
        });
        if (interrupted)
            break;

        if (!native_ping_enabled_.load())
            break;

        // Enviar WebSocket ping frame para manter a conexão viva.
        DWORD err = NO_ERROR;
        bool keepalive_sent = false;
        {
            std::lock_guard send_lock(send_mutex_);
            std::lock_guard conn_lock(connect_mutex_);
            if (websocket_ && state_ == SignalingState::Connected) {
                err = WinHttpWebSocketSend(websocket_,
                    static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(WINHTTP_WEB_SOCKET_PING_PONG_BUFFER_TYPE),
                    nullptr, 0);
                keepalive_sent = (err == NO_ERROR);
            }
        }

        if (keepalive_sent && listener_) {
            listener_->OnSocketActivity();
        }

        if (err != NO_ERROR) {
            native_ping_enabled_ = false;
            if (listener_ && err != ERROR_INVALID_PARAMETER) {
                listener_->OnError(
                    "WebSocket ping failed (" + std::to_string(err) +
                    "); native keepalive disabled for this connection");
            }
            break;
        }
    }
}

void SignalingClient::ReadLoop()
{
    constexpr DWORD kBufSize = 64 * 1024;
    auto buffer = std::make_unique<uint8_t[]>(kBufSize);
    std::string accumulated;

    while (!should_stop_ && websocket_) {
        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type;

        DWORD err = WinHttpWebSocketReceive(
            websocket_,
            buffer.get(),
            kBufSize,
            &bytes_read,
            &buf_type);

        if (err != NO_ERROR) {
            last_error_ = "WinHttpWebSocketReceive failed: " + std::to_string(err);
            if (listener_ && !should_stop_) {
                listener_->OnError(last_error_);
            }
            break;
        }

        if (listener_)
            listener_->OnSocketActivity();

        if (buf_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;

        if (buf_type == static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(WINHTTP_WEB_SOCKET_PING_PONG_BUFFER_TYPE))
            continue;

        accumulated.append(reinterpret_cast<char*>(buffer.get()), bytes_read);

        // UTF8_MESSAGE indica o fragmento final da mensagem.
        if (buf_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            HandleMessage(accumulated);
            accumulated.clear();
        }
        // UTF8_FRAGMENT: continuar acumulando.
    }

    std::lock_guard lock(connect_mutex_);
    CloseHandles();
}

void SignalingClient::HandleMessage(const std::string& json_str)
{
    if (!listener_)
        return;

    SignalingEvent event;
    event.type       = JsonGetString(json_str, "type");
    event.session_id = JsonGetString(json_str, "session_id");
    event.role       = JsonGetString(json_str, "role");
    event.machine_id = JsonGetString(json_str, "machine_id");
    event.error      = JsonGetString(json_str, "error");
    event.payload    = JsonGetPayload(json_str);

    listener_->OnSignalingMessage(event);
}

void SignalingClient::CloseHandles()
{
    if (websocket_) {
        // Abort direto — WinHttpWebSocketClose bloqueia esperando
        // o close handshake do servidor, causando freeze no shutdown.
        WinHttpCloseHandle(websocket_);
        websocket_ = nullptr;
    }
    if (request_) {
        WinHttpCloseHandle(request_);
        request_ = nullptr;
    }
    if (connection_) {
        WinHttpCloseHandle(connection_);
        connection_ = nullptr;
    }
    if (session_handle_) {
        WinHttpCloseHandle(session_handle_);
        session_handle_ = nullptr;
    }
}

uint32_t SignalingClient::CalculateBackoff(uint32_t attempt) const
{
    // Backoff exponencial: initial * 2^(attempt-1), capped em max.
    uint64_t backoff = static_cast<uint64_t>(config_.initial_backoff_ms)
                       << std::min(attempt - 1, 15u);
    return static_cast<uint32_t>(std::min(backoff, static_cast<uint64_t>(config_.max_backoff_ms)));
}

} // namespace remotex::core
