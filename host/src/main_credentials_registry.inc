struct CredentialsFile {
    // Formato token (gerado pelo servidor — modo embedded):
    std::string user_id;
    std::string access_token;
    std::string refresh_token;

    // Formato email/senha (gerado pelo instalador interativo):
    std::string email;
    std::string password;

    bool has_tokens() const { return !access_token.empty() || !refresh_token.empty(); }
    bool has_credentials() const { return !email.empty() && !password.empty(); }
};

static std::optional<CredentialsFile> LoadCredentialsFile() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    fs::path dir = fs::path(exe_path).parent_path();
    fs::path cred_path = dir / "credentials.json";

    if (!fs::exists(cred_path)) return std::nullopt;

    FILE* f = _wfopen(cred_path.c_str(), L"rb");
    if (!f) return std::nullopt;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string json(sz, '\0');
    fread(json.data(), 1, sz, f);
    fclose(f);

    auto get = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return {};
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return {};
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    };

    CredentialsFile creds;
    creds.user_id       = get("user_id");
    creds.access_token  = get("access_token");
    creds.refresh_token = get("refresh_token");
    creds.email         = get("email");
    creds.password      = get("password");

    if (!creds.has_tokens() && !creds.has_credentials())
        return std::nullopt;

    Log("Loaded credentials file from %s (format: %s)",
        remotex::core::RedactPath(cred_path, "credentials_file").c_str(),
        creds.has_tokens() ? "token" : "email/password");
    return creds;
}

// ─── Persistência de machine_id via registro ────────────────────────────────

// HKLM para que o machine_id seja compartilhado entre o serviço (SYSTEM) e
// qualquer sessão de usuário — evita re-registro a cada boot.
static const wchar_t* kRegKey = L"SOFTWARE\\RemoteX\\Host";
static const wchar_t* kLegacyRegKey = L"SOFTWARE\\RemoteAccess\\Host";
static const char* kRegWorkerHeartbeatUtc = "worker_heartbeat_utc";
static const char* kRegWorkerSessionId    = "worker_session_id";
static const char* kRegWorkerPid          = "worker_pid";
static const char* kRegWorkerSignalingUtc = "worker_signaling_utc";
static const char* kRegWorkerSignalingUp  = "worker_signaling_up";
static const char* kRegClearStateOnExit   = "clear_state_on_exit";

static void SaveRegString(const char* name, const std::string& value);
static void SaveConnectionSettings(const HostConnectionSettings& settings);
static void SaveRegDword(const char* name, DWORD value);
static DWORD LoadRegDword(const char* name, DWORD fallback = 0);

static std::string EscapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return {};
    if (json.compare(pos, 4, "null") == 0) return {};
    if (json[pos] != '"') {
        const auto end = json.find_first_of(",}\r\n\t ", pos);
        return json.substr(pos, end - pos);
    }

    ++pos;
    std::string out;
    while (pos < json.size()) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            out += json[pos + 1];
            pos += 2;
            continue;
        }
        if (json[pos] == '"') {
            break;
        }
        out += json[pos++];
    }
    return out;
}

static int JsonGetInt(const std::string& json, const std::string& key, int fallback = 0) {
    const std::string value = JsonGetString(json, key);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

static bool JsonGetBool(const std::string& json, const std::string& key, bool fallback = false) {
    const std::string value = JsonGetString(json, key);
    if (value.empty()) return fallback;
    return value == "true" || value == "1";
}

static std::string LoadRegString(const char* name) {
    bool used_legacy = false;
    const std::string value = remotex::host::detail::ReadRegistryStringValueWithFallback(
        HKEY_LOCAL_MACHINE, kRegKey, kLegacyRegKey, name, &used_legacy);
    if (used_legacy && !value.empty()) {
        SaveRegString(name, value);
        Log("Migrated persisted host registry value '%s' from HKLM\\SOFTWARE\\RemoteAccess\\Host", name);
    }
    return value;
}

static void SaveRegString(const char* name, const std::string& value) {
    remotex::host::detail::WriteRegistryStringValue(HKEY_LOCAL_MACHINE, kRegKey, name, value);
}

static HostConnectionSettings LoadConnectionSettings() {
    bool used_legacy = false;
    HostConnectionSettings settings = remotex::host::detail::LoadConnectionSettingsFromRegistryWithFallback(
        HKEY_LOCAL_MACHINE, kRegKey, kLegacyRegKey, &used_legacy);
    if (used_legacy) {
        SaveConnectionSettings(settings);
        Log("Migrated persisted host connection settings from HKLM\\SOFTWARE\\RemoteAccess\\Host");
    }
    return settings;
}

static void SaveConnectionSettings(const HostConnectionSettings& settings) {
    remotex::host::detail::SaveConnectionSettingsToRegistry(HKEY_LOCAL_MACHINE, kRegKey, settings);
}

static std::string LoadMachineId() {
    return LoadRegString("machine_id");
}

static void SaveMachineId(const std::string& id) {
    SaveRegString("machine_id", id);
}

static std::string LoadMachineKey() {
    return LoadRegString("machine_key");
}

static void SaveMachineKey(const std::string& key) {
    SaveRegString("machine_key", key);
}

static fs::path GetCredentialsFilePath() {
    const fs::path exe_path = GetCurrentExecutablePath();
    if (exe_path.empty()) {
        return {};
    }
    return exe_path.parent_path() / "credentials.json";
}

static fs::path GetBootstrapFilePath() {
    const fs::path exe_path = GetCurrentExecutablePath();
    if (exe_path.empty()) {
        return {};
    }
    return exe_path.parent_path() / "bootstrap.json";
}

static std::optional<BootstrapFile> LoadBootstrapFile() {
    const fs::path bootstrap_path = GetBootstrapFilePath();
    if (bootstrap_path.empty() || !fs::exists(bootstrap_path)) {
        return std::nullopt;
    }

    std::ifstream file(bootstrap_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();
    if (json.empty()) {
        return std::nullopt;
    }

    BootstrapFile bootstrap;
    bootstrap.settings.api_host = JsonGetString(json, "api_host");
    bootstrap.settings.api_port = static_cast<uint16_t>(JsonGetInt(json, "api_port", bootstrap.settings.api_port));
    bootstrap.settings.api_use_tls = JsonGetBool(json, "api_use_tls", true);
    bootstrap.settings.signaling_host = JsonGetString(json, "signaling_host");
    bootstrap.settings.signaling_port = static_cast<uint16_t>(JsonGetInt(json, "signaling_port", bootstrap.settings.signaling_port));
    bootstrap.settings.signaling_use_tls = JsonGetBool(json, "signaling_use_tls", true);
    bootstrap.settings.relay_host = JsonGetString(json, "relay_host");
    bootstrap.settings.relay_port = static_cast<uint16_t>(JsonGetInt(json, "relay_port", bootstrap.settings.relay_port));
    bootstrap.enrollment_token = JsonGetString(json, "enrollment_token");

    if (bootstrap.settings.api_host.empty()) bootstrap.settings.api_host = "demo.remotex.example";
    if (bootstrap.settings.signaling_host.empty()) bootstrap.settings.signaling_host = "demo.remotex.example";
    if (bootstrap.settings.relay_host.empty()) bootstrap.settings.relay_host = "demo.remotex.example";
    if (bootstrap.enrollment_token.empty()) {
        return std::nullopt;
    }

    Log("Loaded bootstrap file from %s",
        remotex::core::RedactPath(bootstrap_path, "bootstrap_file").c_str());
    return bootstrap;
}

static void DeleteBootstrapFile() {
    const fs::path bootstrap_path = GetBootstrapFilePath();
    if (bootstrap_path.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove(bootstrap_path, ec);
    if (ec) {
        Log("WARNING: Failed to delete bootstrap file %s: %s",
            remotex::core::RedactPath(bootstrap_path, "bootstrap_file").c_str(),
            ec.message().c_str());
    }
}

static void DeleteCredentialsFile() {
    const fs::path credentials_path = GetCredentialsFilePath();
    if (credentials_path.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove(credentials_path, ec);
    if (ec) {
        Log("WARNING: Failed to delete credentials file %s: %s",
            remotex::core::RedactPath(credentials_path, "credentials_file").c_str(),
            ec.message().c_str());
    }
}

struct SimpleHttpResponse {
    int status_code = 0;
    std::string body;
    DWORD winhttp_error = 0;
    DWORD secure_failure_flags = 0;
    std::string error_stage;
    std::string error_detail;
    bool used_revocation_offline_fallback = false;
};
