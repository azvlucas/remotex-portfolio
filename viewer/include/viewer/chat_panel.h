#pragma once
// RemoteX — viewer/chat_panel.h
// Painel de chat integrado à sessão remota (QDockWidget).

#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>

namespace remotex::viewer {

class ChatPanel : public QDockWidget {
    Q_OBJECT

public:
    explicit ChatPanel(QWidget* parent = nullptr);

signals:
    void MessageSent(const QString& text);

public slots:
    void OnMessageReceived(const QString& sender, const QString& text);
    void OnConnectionStateChanged(bool connected);

private slots:
    void SendCurrentMessage();

private:
    QTextEdit*   history_  = nullptr;
    QLineEdit*   input_    = nullptr;
    QPushButton* send_btn_ = nullptr;
};

} // namespace remotex::viewer
