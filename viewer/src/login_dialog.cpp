// RemoteX — viewer/login_dialog.cpp
// Tela de login/registro.

#include "viewer/login_dialog.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QVBoxLayout>
#include <QMessageBox>

namespace remotex::viewer {

namespace {

constexpr auto kOrgName = "RemoteX";
constexpr auto kAppName = "Viewer";
constexpr int  kHealthIntervalMs = 10000;  // 10s
constexpr int  kHealthTimeoutMs  = 5000;   // 5s

struct LoginTaskResult {
    bool    success = false;
    QString error;
};

} // namespace

LoginDialog::LoginDialog(remotex::core::AuthClient* auth,
                         const QUrl& health_url,
                         QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , health_url_(health_url)
{
    SetupUI();
    PreFillRememberedEmail();

    if (health_url_.isValid()) {
        StartHealthCheck();
    } else {
        // Sem URL de health → assumir conectado (comportamento legado).
        server_reachable_ = true;
    }
}

void LoginDialog::SetupUI() {
    setWindowTitle("RemoteX — Login");
    setFixedSize(380, 400);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(32, 24, 32, 24);

    auto* title = new QLabel("RemoteX");
    title->setAlignment(Qt::AlignCenter);
    auto font = title->font();
    font.setPointSize(18);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    layout->addSpacing(12);

    layout->addWidget(new QLabel("Email"));
    email_edit_ = new QLineEdit;
    email_edit_->setPlaceholderText("user@example.com");
    layout->addWidget(email_edit_);

    layout->addWidget(new QLabel("Senha"));
    password_edit_ = new QLineEdit;
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setPlaceholderText("Senha");
    layout->addWidget(password_edit_);

    remember_me_check_ = new QCheckBox("Lembrar login");
    layout->addWidget(remember_me_check_);
    layout->addSpacing(4);

    status_label_ = new QLabel;
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setStyleSheet("color: red;");
    layout->addWidget(status_label_);

    login_btn_ = new QPushButton("Entrar");
    login_btn_->setDefault(true);
    layout->addWidget(login_btn_);

    layout->addStretch();

    // Connection status label at the bottom.
    connection_label_ = new QLabel;
    connection_label_->setAlignment(Qt::AlignCenter);
    auto small_font = connection_label_->font();
    small_font.setPointSize(8);
    connection_label_->setFont(small_font);
    layout->addWidget(connection_label_);

    connect(login_btn_, &QPushButton::clicked, this, &LoginDialog::OnLoginClicked);

    // Enter key na password submete login.
    connect(password_edit_, &QLineEdit::returnPressed, this, &LoginDialog::OnLoginClicked);
}

void LoginDialog::PreFillRememberedEmail() {
    QSettings settings(kOrgName, kAppName);
    if (settings.value("login/remember_me", false).toBool()) {
        const auto email = settings.value("login/remembered_email").toString();
        if (!email.isEmpty()) {
            email_edit_->setText(email);
            remember_me_check_->setChecked(true);
            password_edit_->setFocus();
        }
    }
}

void LoginDialog::StartHealthCheck() {
    network_manager_ = new QNetworkAccessManager(this);
    health_timer_ = new QTimer(this);
    health_timer_->setInterval(kHealthIntervalMs);

    auto doCheck = [this]() {
        QNetworkRequest request(health_url_);
        request.setTransferTimeout(kHealthTimeoutMs);
        auto* reply = network_manager_->get(request);

        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            const bool ok = (reply->error() == QNetworkReply::NoError);
            server_reachable_ = ok;

            if (ok) {
                connection_label_->setStyleSheet("color: green;");
                connection_label_->setText(QString::fromUtf8("\u25CF Conectado ao servidor"));
            } else {
                connection_label_->setStyleSheet("color: red;");
                connection_label_->setText(QString::fromUtf8("\u25CF Servidor indispon\u00EDvel"));
            }

            // Only touch login_btn_ if we're not mid-login.
            if (!login_in_flight_) {
                login_btn_->setEnabled(ok);
            }
        });
    };

    connect(health_timer_, &QTimer::timeout, this, doCheck);
    health_timer_->start();

    // Run first check immediately.
    doCheck();
}

void LoginDialog::SetStatus(const QString& message, bool is_error) {
    status_label_->setStyleSheet(is_error ? "color: red;" : "color: green;");
    status_label_->setText(message);
}

void LoginDialog::OnLoginClicked() {
    if (login_in_flight_) {
        return;
    }

    if (!server_reachable_) {
        SetStatus(QString::fromUtf8("Servidor indispon\u00EDvel. Aguarde a reconex\u00E3o."));
        return;
    }

    auto email = email_edit_->text().trimmed().toStdString();
    auto password = password_edit_->text().toStdString();

    if (email.empty() || password.empty()) {
        SetStatus("Preencha email e senha.");
        return;
    }

    login_in_flight_ = true;
    login_btn_->setEnabled(false);
    SetStatus("Entrando...", false);
    email_edit_->setEnabled(false);
    password_edit_->setEnabled(false);

    const bool remember = remember_me_check_->isChecked();
    const QString email_qstr = email_edit_->text().trimmed();

    auto* watcher = new QFutureWatcher<LoginTaskResult>(this);
    connect(watcher, &QFutureWatcher<LoginTaskResult>::finished, this, [this, watcher, remember, email_qstr]() {
        const LoginTaskResult result = watcher->result();
        watcher->deleteLater();

        login_in_flight_ = false;
        login_btn_->setEnabled(server_reachable_);
        email_edit_->setEnabled(true);
        password_edit_->setEnabled(true);

        if (result.success) {
            // Save or clear remember-me preference.
            QSettings settings(kOrgName, kAppName);
            if (remember) {
                settings.setValue("login/remember_me", true);
                settings.setValue("login/remembered_email", email_qstr);
            } else {
                settings.remove("login/remember_me");
                settings.remove("login/remembered_email");
            }

            status_label_->clear();
            emit Authenticated();
            return;
        }

        SetStatus(result.error.isEmpty() ? "Login falhou" : result.error);
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, email = std::move(email), password = std::move(password)]() {
        LoginTaskResult result;
        const auto auth_result = auth->Login(email, password);
        result.success = auth_result.success;
        result.error = QString::fromStdString(
            auth_result.error.empty() ? "Login falhou" : auth_result.error);
        return result;
    }));
}

} // namespace remotex::viewer
