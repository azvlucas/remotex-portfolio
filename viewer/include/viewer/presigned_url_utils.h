#pragma once

#include <QByteArray>
#include <QDesktopServices>
#include <QString>
#include <QUrl>

namespace remotex::viewer {

inline QUrl BuildPresignedExternalUrl(const QString& raw_url, QString* error = nullptr) {
    if (error) {
        error->clear();
    }

    const QString trimmed_url = raw_url.trimmed();
    if (trimmed_url.isEmpty()) {
        if (error) {
            *error = "URL de download vazia.";
        }
        return {};
    }

    const QByteArray encoded_url = trimmed_url.toUtf8();
    const QUrl url = QUrl::fromEncoded(encoded_url, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();

    if (!url.isValid() || url.isRelative() || url.host().isEmpty() ||
        (scheme != "http" && scheme != "https")) {
        if (error) {
            *error = "URL de download invalida.";
        }
        return {};
    }

    if (url.toEncoded() != encoded_url) {
        if (error) {
            *error = "URL de download foi alterada ao ser interpretada.";
        }
        return {};
    }

    return url;
}

inline bool OpenPresignedExternalUrlInBrowser(const QString& raw_url, QString* error = nullptr) {
    const QUrl url = BuildPresignedExternalUrl(raw_url, error);
    if (!url.isValid()) {
        return false;
    }

    if (!QDesktopServices::openUrl(url)) {
        if (error) {
            *error = "Falha ao abrir o link de download no navegador.";
        }
        return false;
    }

    return true;
}

} // namespace remotex::viewer
