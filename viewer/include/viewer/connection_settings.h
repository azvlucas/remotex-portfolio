#pragma once

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <cstdint>

namespace remotex::viewer {

struct ConnectionSettings {
    QString  api_host = "demo.remotex.example";
    uint16_t api_port = 443;
    bool     api_use_tls = true;

    QString  signaling_host = "demo.remotex.example";
    uint16_t signaling_port = 443;
    bool     signaling_use_tls = true;

    QString  relay_host = "demo.remotex.example";
    uint16_t relay_port = 9100;

    static ConnectionSettings Load();
    void Save() const;

    remotex::core::AuthConfig ToAuthConfig(const std::string& credential_target = "RemoteX") const;
    remotex::core::MachineClientConfig ToMachineClientConfig() const;

    QUrl BuildApiUrl(const QString& path) const;
    QJsonObject ToInstallerPayload() const;
};

} // namespace remotex::viewer
