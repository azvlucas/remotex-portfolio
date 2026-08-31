// RemoteX — viewer/standalone_chat_dialog.cpp

#include "viewer/standalone_chat_dialog.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace remotex::viewer {

// ── ChatSignalingWorker ──────────────────────────────────────────────────────

ChatSignalingWorker::ChatSignalingWorker(QObject* parent)
    : QObject(parent)
{
}

ChatSignalingWorker::~ChatSignalingWorker()
{
    signaling_.Disconnect();
}

void ChatSignalingWorker::ConnectToSignaling(const QString& host, quint16 port, bool use_tls,
                                              const QString& client_id, const QString& machine_id,
                                              remotex::core::AuthClient* auth)
{
    machine_id_ = machine_id.toStdString();

    remotex::core::SignalingConfig cfg;
    cfg.host      = host.toStdString();
    cfg.port      = port;
    cfg.path      = "/ws";
    cfg.use_tls   = use_tls;
    cfg.client_id = client_id.toStdString();
    cfg.token_provider = [auth](bool force_refresh) {
        if (force_refresh) auth->RefreshToken();
        return auth->GetAccessToken();
    };
    cfg.max_reconnect_attempts = 5;
    cfg.initial_backoff_ms     = 500;
    cfg.max_backoff_ms         = 10000;

    signaling_.SetListener(this);
    signaling_.Connect(cfg);
}

void ChatSignalingWorker::DisconnectFromSignaling()
{
    if (signaling_.GetState() != remotex::core::SignalingState::Disconnected) {
        signaling_.ChatLeave(machine_id_);
    }
    signaling_.Disconnect();
}

void ChatSignalingWorker::SendMessage(const QString& text)
{
    if (signaling_.GetState() != remotex::core::SignalingState::Connected)
        return;

    signaling_.SendChatMessage(machine_id_, "viewer", text.toStdString());
}

void ChatSignalingWorker::OnConnected()
{
    // Inscrever-se para receber mensagens de chat desta máquina.
    signaling_.ChatJoin(machine_id_);
    emit Connected();
}

void ChatSignalingWorker::OnDisconnected()
{
    emit Disconnected();
}

void ChatSignalingWorker::OnSignalingMessage(const remotex::core::SignalingEvent& event)
{
    if (event.type != remotex::core::signal_msg::Chat)
        return;

    // Extrair "from" e "text" do payload JSON.
    auto extract = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\":\"";
        auto pos = event.payload.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        std::string value;
        for (size_t i = pos; i < event.payload.size(); ++i) {
            if (event.payload[i] == '\\' && i + 1 < event.payload.size()) {
                char next = event.payload[i + 1];
                if (next == '"') { value += '"'; ++i; }
                else if (next == '\\') { value += '\\'; ++i; }
                else if (next == 'n') { value += '\n'; ++i; }
                else if (next == 'r') { value += '\r'; ++i; }
                else { value += next; ++i; }
            } else if (event.payload[i] == '"') {
                break;
            } else {
                value += event.payload[i];
            }
        }
        return value;
    };

    QString sender = QString::fromStdString(extract("from"));
    QString text = QString::fromStdString(extract("text"));
    if (!text.isEmpty()) {
        emit ChatMessageReceived(sender, text);
    }
}

void ChatSignalingWorker::OnError(const std::string& error)
{
    emit ErrorOccurred(QString::fromStdString(error));
}

// ── StandaloneChatDialog ─────────────────────────────────────────────────────

StandaloneChatDialog::StandaloneChatDialog(remotex::core::AuthClient* auth,
                                            const QString& machine_id,
                                            const QString& machine_name,
                                            QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , machine_id_(machine_id)
    , machine_name_(machine_name)
{
    SetupUI();

    // Worker roda no thread principal — o SignalingClient já tem suas próprias threads.
    worker_ = new ChatSignalingWorker(this);

    connect(worker_, &ChatSignalingWorker::Connected,
            this, &StandaloneChatDialog::OnConnected);
    connect(worker_, &ChatSignalingWorker::Disconnected,
            this, [this]() { OnDisconnected(); });
    connect(worker_, &ChatSignalingWorker::ChatMessageReceived,
            this, &StandaloneChatDialog::OnMessageReceived);
    connect(worker_, &ChatSignalingWorker::ErrorOccurred,
            this, &StandaloneChatDialog::OnError);

    // Conectar ao signaling.
    ConnectionSettings cs = ConnectionSettings::Load();
    worker_->ConnectToSignaling(
        cs.signaling_host,
        cs.signaling_port,
        cs.signaling_use_tls,
        QString::fromStdString(auth_->GetUserID()),
        machine_id_,
        auth_);
}

StandaloneChatDialog::~StandaloneChatDialog()
{
    if (worker_) {
        worker_->DisconnectFromSignaling();
    }
}

void StandaloneChatDialog::SetupUI()
{
    setWindowTitle(QString("Chat — %1").arg(machine_name_));
    resize(420, 480);
    setMinimumSize(300, 250);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    history_ = new QTextEdit(this);
    history_->setReadOnly(true);
    history_->setPlaceholderText("Nenhuma mensagem ainda...");
    layout->addWidget(history_, 1);

    auto* input_layout = new QHBoxLayout();
    input_layout->setSpacing(4);

    input_ = new QLineEdit(this);
    input_->setPlaceholderText("Digite uma mensagem...");
    input_layout->addWidget(input_, 1);

    send_btn_ = new QPushButton("Enviar", this);
    send_btn_->setFixedWidth(70);
    input_layout->addWidget(send_btn_);

    layout->addLayout(input_layout);

    connect(send_btn_, &QPushButton::clicked, this, &StandaloneChatDialog::OnSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &StandaloneChatDialog::OnSendClicked);

    SetConnectedState(false);
}

void StandaloneChatDialog::SetConnectedState(bool connected)
{
    input_->setEnabled(connected);
    send_btn_->setEnabled(connected);
    if (!connected) {
        input_->setPlaceholderText("Conectando...");
    } else {
        input_->setPlaceholderText("Digite uma mensagem...");
    }
}

void StandaloneChatDialog::OnSendClicked()
{
    const QString text = input_->text().trimmed();
    if (text.isEmpty()) return;

    input_->clear();

    // Mostrar a própria mensagem no histórico.
    OnMessageReceived("viewer", text);

    // Enviar via signaling.
    worker_->SendMessage(text);
}

void StandaloneChatDialog::OnMessageReceived(const QString& sender, const QString& text)
{
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

void StandaloneChatDialog::OnConnected()
{
    SetConnectedState(true);
}

void StandaloneChatDialog::OnDisconnected(const QString&)
{
    SetConnectedState(false);
}

void StandaloneChatDialog::OnError(const QString& error)
{
    Q_UNUSED(error);
}

} // namespace remotex::viewer
