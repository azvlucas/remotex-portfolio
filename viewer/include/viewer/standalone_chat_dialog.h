#pragma once
// RemoteX — viewer/standalone_chat_dialog.h
// Diálogo de chat independente de sessão remota.
// Usa o signaling WebSocket para enviar/receber mensagens de chat.

#include "core/auth_client.h"
#include "core/signaling_client.h"
#include "viewer/connection_settings.h"

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>

namespace remotex::viewer {

// Worker que roda o SignalingClient em uma thread dedicada.
class ChatSignalingWorker : public QObject, public remotex::core::SignalingListener {
    Q_OBJECT

public:
    explicit ChatSignalingWorker(QObject* parent = nullptr);
    ~ChatSignalingWorker() override;

public slots:
    void ConnectToSignaling(const QString& host, quint16 port, bool use_tls,
                            const QString& client_id, const QString& machine_id,
                            remotex::core::AuthClient* auth);
    void DisconnectFromSignaling();
    void SendMessage(const QString& text);

signals:
    void Connected();
    void Disconnected();
    void ChatMessageReceived(const QString& sender, const QString& text);
    void ErrorOccurred(const QString& error);

private:
    // SignalingListener
    void OnConnected() override;
    void OnDisconnected() override;
    void OnSignalingMessage(const remotex::core::SignalingEvent& event) override;
    void OnError(const std::string& error) override;

    remotex::core::SignalingClient signaling_;
    std::string machine_id_;
};

class StandaloneChatDialog : public QDialog {
    Q_OBJECT

public:
    explicit StandaloneChatDialog(remotex::core::AuthClient* auth,
                                  const QString& machine_id,
                                  const QString& machine_name,
                                  QWidget* parent = nullptr);
    ~StandaloneChatDialog() override;

private slots:
    void OnSendClicked();
    void OnMessageReceived(const QString& sender, const QString& text);
    void OnConnected();
    void OnDisconnected(const QString& reason = QString());
    void OnError(const QString& error);

private:
    void SetupUI();
    void SetConnectedState(bool connected);

    remotex::core::AuthClient* auth_ = nullptr;
    QString machine_id_;
    QString machine_name_;

    QTextEdit*   history_  = nullptr;
    QLineEdit*   input_    = nullptr;
    QPushButton* send_btn_ = nullptr;

    ChatSignalingWorker*   worker_        = nullptr;
};

} // namespace remotex::viewer
