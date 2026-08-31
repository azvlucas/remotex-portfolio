#pragma once
// RemoteX — viewer/login_dialog.h
// Tela de login/registro com AuthClient.

#include "core/auth_client.h"

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

namespace remotex::viewer {

class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(remotex::core::AuthClient* auth,
                         const QUrl& health_url = QUrl(),
                         QWidget* parent = nullptr);

signals:
    // Emitido quando o login é bem-sucedido.
    void Authenticated();

private slots:
    void OnLoginClicked();

private:
    void SetupUI();
    void SetStatus(const QString& message, bool is_error = true);
    void PreFillRememberedEmail();
    void StartHealthCheck();

    remotex::core::AuthClient* auth_;
    QUrl health_url_;

    QLineEdit*              email_edit_        = nullptr;
    QLineEdit*              password_edit_     = nullptr;
    QLabel*                 status_label_      = nullptr;
    QPushButton*            login_btn_         = nullptr;
    QCheckBox*              remember_me_check_ = nullptr;
    QLabel*                 connection_label_  = nullptr;
    QNetworkAccessManager*  network_manager_   = nullptr;
    QTimer*                 health_timer_      = nullptr;
    bool                    login_in_flight_   = false;
    bool                    server_reachable_  = false;
};

} // namespace remotex::viewer
