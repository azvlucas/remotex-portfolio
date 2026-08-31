#include "viewer/connection_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>
#include <QUrl>

namespace remotex::viewer {

namespace {

constexpr auto kOrgName = "RemoteX";
constexpr auto kAppName = "Viewer";
constexpr auto kInstallerConfigEnv = "REMOTEX_VIEWER_CONFIG_PATH";
constexpr auto kUserSettingsPathEnv = "REMOTEX_VIEWER_USER_SETTINGS_PATH";

uint16_t ReadPort(const QSettings& settings, const char* key, uint16_t fallback) {
    if (!settings.contains(key)) {
        return fallback;
    }

    const int value = settings.value(key, static_cast<int>(fallback)).toInt();
    if (value <= 0 || value > 65535) {
        return fallback;
    }
    return static_cast<uint16_t>(value);
}

bool ReadBool(const QSettings& settings, const char* key, bool fallback) {
    if (!settings.contains(key)) {
        return fallback;
    }

    const QVariant raw_value = settings.value(key);
    const QString text = raw_value.toString().trimmed().toLower();
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }

    bool ok = false;
    const int numeric_value = raw_value.toInt(&ok);
    if (ok) {
        return numeric_value != 0;
    }

    return fallback;
}

QString ReadString(const QSettings& settings, const char* key, const QString& fallback) {
    if (!settings.contains(key)) {
        return fallback;
    }

    const QString value = settings.value(key).toString().trimmed();
    if (value.isEmpty()) {
        return fallback;
    }

    return value;
}

void ApplyUrlOverride(const QString& raw_url,
                      QString& host,
                      uint16_t& port,
                      bool& use_tls) {
    const QUrl url = QUrl::fromUserInput(raw_url.trimmed());
    if (!url.isValid()) {
        return;
    }

    const QString scheme = url.scheme().trimmed().toLower();
    if (scheme != "http" && scheme != "https") {
        return;
    }

    const QString parsed_host = url.host().trimmed();
    if (parsed_host.isEmpty()) {
        return;
    }

    host = parsed_host;
    use_tls = scheme == "https";

    const int parsed_port = url.port(use_tls ? 443 : 80);
    if (parsed_port > 0 && parsed_port <= 65535) {
        port = static_cast<uint16_t>(parsed_port);
    }
}

void ApplySettings(ConnectionSettings& current, const QSettings& settings) {
    ApplyUrlOverride(settings.value("connection/api_url").toString(),
                     current.api_host,
                     current.api_port,
                     current.api_use_tls);
    ApplyUrlOverride(settings.value("connection/signaling_url").toString(),
                     current.signaling_host,
                     current.signaling_port,
                     current.signaling_use_tls);

    current.api_host = ReadString(settings, "connection/api_host", current.api_host);
    current.api_port = ReadPort(settings, "connection/api_port", current.api_port);
    current.api_use_tls = ReadBool(settings, "connection/api_use_tls", current.api_use_tls);

    current.signaling_host = ReadString(settings, "connection/signaling_host", current.signaling_host);
    current.signaling_port = ReadPort(settings, "connection/signaling_port", current.signaling_port);
    current.signaling_use_tls = ReadBool(settings, "connection/signaling_use_tls", current.signaling_use_tls);

    current.relay_host = ReadString(settings, "connection/relay_host", current.relay_host);
    current.relay_port = ReadPort(settings, "connection/relay_port", current.relay_port);
}

QString ResolveInstallerConfigPath() {
    const QString configured_path = qEnvironmentVariable(kInstallerConfigEnv).trimmed();
    if (!configured_path.isEmpty()) {
        return QDir::fromNativeSeparators(configured_path);
    }

    if (QCoreApplication::instance() == nullptr) {
        return {};
    }

    return QDir(QCoreApplication::applicationDirPath()).filePath("connection.ini");
}

QSettings OpenViewerUserSettings() {
    const QString configured_path = qEnvironmentVariable(kUserSettingsPathEnv).trimmed();
    if (!configured_path.isEmpty()) {
        return QSettings(QDir::fromNativeSeparators(configured_path), QSettings::IniFormat);
    }

    return QSettings(kOrgName, kAppName);
}

} // namespace

ConnectionSettings ConnectionSettings::Load() {
    ConnectionSettings current;
    const QString installer_config_path = ResolveInstallerConfigPath();
    if (!installer_config_path.isEmpty() && QFileInfo::exists(installer_config_path)) {
        QSettings installer_settings(installer_config_path, QSettings::IniFormat);
        ApplySettings(current, installer_settings);
    }

    auto settings = OpenViewerUserSettings();
    ApplySettings(current, settings);

    return current;
}

void ConnectionSettings::Save() const {
    auto settings = OpenViewerUserSettings();
    settings.setValue("connection/api_host", api_host.trimmed());
    settings.setValue("connection/api_port", static_cast<int>(api_port));
    settings.setValue("connection/api_use_tls", api_use_tls);
    settings.setValue("connection/signaling_host", signaling_host.trimmed());
    settings.setValue("connection/signaling_port", static_cast<int>(signaling_port));
    settings.setValue("connection/signaling_use_tls", signaling_use_tls);
    settings.setValue("connection/relay_host", relay_host.trimmed());
    settings.setValue("connection/relay_port", static_cast<int>(relay_port));
}

remotex::core::AuthConfig ConnectionSettings::ToAuthConfig(const std::string& credential_target) const {
    remotex::core::AuthConfig config;
    config.api_host = api_host.toStdString();
    config.api_port = api_port;
    config.use_tls = api_use_tls;
    config.credential_target = credential_target;
    return config;
}

remotex::core::MachineClientConfig ConnectionSettings::ToMachineClientConfig() const {
    remotex::core::MachineClientConfig config;
    config.api_host = api_host.toStdString();
    config.api_port = api_port;
    config.use_tls = api_use_tls;
    return config;
}

QUrl ConnectionSettings::BuildApiUrl(const QString& path) const {
    QUrl url;
    url.setScheme(api_use_tls ? "https" : "http");
    url.setHost(api_host.trimmed());
    url.setPort(api_port);
    url.setPath(path.startsWith('/') ? path : "/" + path);
    return url;
}

QJsonObject ConnectionSettings::ToInstallerPayload() const {
    QJsonObject payload;
    payload["api_host"] = api_host.trimmed();
    payload["api_port"] = static_cast<int>(api_port);
    payload["api_use_tls"] = api_use_tls;
    payload["signaling_host"] = signaling_host.trimmed();
    payload["signaling_port"] = static_cast<int>(signaling_port);
    payload["signaling_use_tls"] = signaling_use_tls;
    payload["relay_host"] = relay_host.trimmed();
    payload["relay_port"] = static_cast<int>(relay_port);
    return payload;
}

} // namespace remotex::viewer
