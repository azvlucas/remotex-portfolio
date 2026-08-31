// RemoteX — viewer/chat_panel.cpp

#include "viewer/chat_panel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace remotex::viewer {

ChatPanel::ChatPanel(QWidget* parent)
    : QDockWidget("Chat", parent) {
    setObjectName("ChatPanel");
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    history_ = new QTextEdit(container);
    history_->setReadOnly(true);
    history_->setPlaceholderText("Nenhuma mensagem ainda...");
    layout->addWidget(history_, 1);

    auto* input_layout = new QHBoxLayout();
    input_layout->setSpacing(4);

    input_ = new QLineEdit(container);
    input_->setPlaceholderText("Digite uma mensagem...");
    input_layout->addWidget(input_, 1);

    send_btn_ = new QPushButton("Enviar", container);
    send_btn_->setFixedWidth(70);
    input_layout->addWidget(send_btn_);

    layout->addLayout(input_layout);
    setWidget(container);

    connect(send_btn_, &QPushButton::clicked, this, &ChatPanel::SendCurrentMessage);
    connect(input_, &QLineEdit::returnPressed, this, &ChatPanel::SendCurrentMessage);

    // Start disabled until connected.
    OnConnectionStateChanged(false);
}

void ChatPanel::SendCurrentMessage() {
    const QString text = input_->text().trimmed();
    if (text.isEmpty()) return;

    input_->clear();
    emit MessageSent(text);

    // Show own message immediately in history.
    OnMessageReceived("viewer", text);
}

void ChatPanel::OnMessageReceived(const QString& sender, const QString& text) {
    const QString time = QDateTime::currentDateTime().toString("HH:mm");
    const bool is_viewer = (sender == "viewer");
    const QString color = is_viewer ? "#3b82f6" : "#22c55e";
    const QString label = is_viewer ? "Voce" : "Cliente";

    QString html = QString("<span style=\"color:%1; font-weight:bold;\">%2</span> "
                           "<span style=\"color:#888; font-size:small;\">%3</span><br>"
                           "<span>%4</span><br>")
                       .arg(color, label, time, text.toHtmlEscaped());
    history_->append(html);
}

void ChatPanel::OnConnectionStateChanged(bool connected) {
    input_->setEnabled(connected);
    send_btn_->setEnabled(connected);
    if (!connected) {
        input_->clear();
    }
}

} // namespace remotex::viewer
