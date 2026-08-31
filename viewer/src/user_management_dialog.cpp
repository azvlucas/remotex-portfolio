#include "viewer/user_management_dialog.h"
#include "viewer/connection_settings.h"

#include <QtConcurrent>
#include <QDialog>
#include <QFutureWatcher>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace remotex::viewer {

namespace {

struct UsersLoadResult {
    quint64 request_id = 0;
    std::vector<remotex::core::CompanyUserInfo> users;
    QString error;
};

struct UserActionResult {
    quint64 request_id = 0;
    QString error;
};

remotex::core::MachineClient BuildDialogClient(remotex::core::AuthClient* auth) {
    remotex::core::MachineClient client;
    client.Configure(ConnectionSettings::Load().ToMachineClientConfig(), auth);
    return client;
}

QString FormatTimestamp(const std::string& ts) {
    if (ts.empty()) return "-";
    QDateTime dt = QDateTime::fromString(QString::fromStdString(ts), Qt::ISODate);
    if (!dt.isValid()) return QString::fromStdString(ts);
    return dt.toLocalTime().toString("dd/MM/yyyy HH:mm");
}

} // namespace

UserManagementDialog::UserManagementDialog(remotex::core::AuthClient* auth,
                                             QWidget* parent)
    : QDialog(parent)
    , auth_(auth) {
    SetupUi();
    ReloadUsers();
}

void UserManagementDialog::SetupUi() {
    setWindowTitle("Gerenciamento de usuarios");
    resize(720, 420);

    auto* root = new QVBoxLayout(this);

    table_ = new QTableWidget;
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"Nome", "E-mail", "Cargo", "Status", "Criado em"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    root->addWidget(table_, 1);

    auto* bottom = new QHBoxLayout;
    status_label_ = new QLabel;
    bottom->addWidget(status_label_, 1);

    add_btn_ = new QPushButton("Adicionar usuario");
    toggle_btn_ = new QPushButton("Ativar/Bloquear");
    reset_pwd_btn_ = new QPushButton("Resetar senha");
    close_btn_ = new QPushButton("Fechar");

    bottom->addWidget(add_btn_);
    bottom->addWidget(toggle_btn_);
    bottom->addWidget(reset_pwd_btn_);
    bottom->addWidget(close_btn_);
    root->addLayout(bottom);

    connect(add_btn_, &QPushButton::clicked, this, &UserManagementDialog::OnAddUser);
    connect(toggle_btn_, &QPushButton::clicked, this, &UserManagementDialog::OnToggleActive);
    connect(reset_pwd_btn_, &QPushButton::clicked, this, &UserManagementDialog::OnResetPassword);
    connect(close_btn_, &QPushButton::clicked, this, &QDialog::accept);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { UpdateUiState(); });

    UpdateUiState();
}

int UserManagementDialog::GetSelectedRow() const {
    auto items = table_->selectionModel()->selectedRows();
    if (items.isEmpty()) return -1;
    return items.first().row();
}

void UserManagementDialog::SetStatus(const QString& message, bool is_error) {
    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? "color: red;" : "");
}

void UserManagementDialog::UpdateUiState() {
    bool has_selection = GetSelectedRow() >= 0;
    add_btn_->setEnabled(!loading_ && !action_in_flight_);
    toggle_btn_->setEnabled(!loading_ && !action_in_flight_ && has_selection);
    reset_pwd_btn_->setEnabled(!loading_ && !action_in_flight_ && has_selection);
}

void UserManagementDialog::ReloadUsers() {
    loading_ = true;
    ++load_request_id_;
    quint64 req_id = load_request_id_;
    SetStatus("Carregando...");
    UpdateUiState();

    auto* auth = auth_;
    auto* watcher = new QFutureWatcher<UsersLoadResult>(this);
    connect(watcher, &QFutureWatcher<UsersLoadResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != load_request_id_) return;

        auto result = watcher->result();
        loading_ = false;

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            UpdateUiState();
            return;
        }

        users_ = std::move(result.users);
        PopulateTable();
        SetStatus(QString("%1 usuario(s)").arg(users_.size()));
        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth, req_id]() -> UsersLoadResult {
        UsersLoadResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        result.users = client.ListCompanyUsers();
        return result;
    }));
}

void UserManagementDialog::PopulateTable() {
    table_->setRowCount(0);
    table_->setRowCount(static_cast<int>(users_.size()));

    for (size_t i = 0; i < users_.size(); ++i) {
        const auto& u = users_[i];
        int row = static_cast<int>(i);

        table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(u.name)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(u.email)));

        QString role_label = (u.role == "company_manager") ? "Gerente" : "Usuario";
        table_->setItem(row, 2, new QTableWidgetItem(role_label));

        QString status_label = u.is_active ? "Ativo" : "Bloqueado";
        auto* status_item = new QTableWidgetItem(status_label);
        status_item->setForeground(u.is_active ? QColor("#2b8a3e") : QColor("#c92a2a"));
        table_->setItem(row, 3, status_item);

        table_->setItem(row, 4, new QTableWidgetItem(FormatTimestamp(u.created_at)));
    }
}

void UserManagementDialog::OnAddUser() {
    QDialog dlg(this);
    dlg.setWindowTitle("Adicionar usuario");
    dlg.setMinimumWidth(360);

    auto* form = new QFormLayout(&dlg);
    auto* name_input = new QLineEdit;
    auto* email_input = new QLineEdit;
    auto* password_input = new QLineEdit;
    password_input->setEchoMode(QLineEdit::Password);

    form->addRow("Nome:", name_input);
    form->addRow("E-mail:", email_input);
    form->addRow("Senha:", password_input);

    auto* buttons = new QHBoxLayout;
    auto* ok_btn = new QPushButton("Criar");
    auto* cancel_btn = new QPushButton("Cancelar");
    buttons->addStretch();
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);
    form->addRow(buttons);

    connect(ok_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString name = name_input->text().trimmed();
    QString email = email_input->text().trimmed();
    QString password = password_input->text();

    if (name.isEmpty() || email.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "Erro", "Preencha todos os campos.");
        return;
    }
    if (password.size() < 8) {
        QMessageBox::warning(this, "Erro", "A senha deve ter pelo menos 8 caracteres.");
        return;
    }

    action_in_flight_ = true;
    ++action_request_id_;
    quint64 req_id = action_request_id_;
    SetStatus("Criando usuario...");
    UpdateUiState();

    auto* auth = auth_;
    std::string n = name.toStdString();
    std::string e = email.toStdString();
    std::string p = password.toStdString();

    auto* watcher = new QFutureWatcher<UserActionResult>(this);
    connect(watcher, &QFutureWatcher<UserActionResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != action_request_id_) return;
        action_in_flight_ = false;

        auto result = watcher->result();
        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            UpdateUiState();
            return;
        }

        SetStatus("Usuario criado com sucesso.");
        ReloadUsers();
    });

    watcher->setFuture(QtConcurrent::run([auth, n, e, p, req_id]() -> UserActionResult {
        UserActionResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        auto user = client.CreateCompanyUser(n, e, p);
        if (!user.has_value()) {
            result.error = "Falha ao criar usuario";
        }
        return result;
    }));
}

void UserManagementDialog::OnToggleActive() {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(users_.size())) return;

    const auto& user = users_[static_cast<size_t>(row)];
    bool currently_active = user.is_active;
    std::string user_id = user.id;

    QString action = currently_active ? "bloquear" : "ativar";
    auto reply = QMessageBox::question(this, "Confirmar",
        QString("Deseja %1 o usuario %2?").arg(action, QString::fromStdString(user.name)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    action_in_flight_ = true;
    ++action_request_id_;
    quint64 req_id = action_request_id_;
    SetStatus(currently_active ? "Bloqueando..." : "Ativando...");
    UpdateUiState();

    auto* auth = auth_;
    auto* watcher = new QFutureWatcher<UserActionResult>(this);
    connect(watcher, &QFutureWatcher<UserActionResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != action_request_id_) return;
        action_in_flight_ = false;

        auto result = watcher->result();
        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            UpdateUiState();
            return;
        }

        SetStatus("Usuario atualizado.");
        ReloadUsers();
    });

    watcher->setFuture(QtConcurrent::run([auth, user_id, currently_active, req_id]() -> UserActionResult {
        UserActionResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        auto r = currently_active
            ? client.DeactivateCompanyUser(user_id)
            : client.ActivateCompanyUser(user_id);
        if (!r.success) {
            result.error = r.error.empty()
                ? QString("Falha na operacao")
                : QString::fromStdString(r.error);
        }
        return result;
    }));
}

void UserManagementDialog::OnResetPassword() {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(users_.size())) return;

    const auto& user = users_[static_cast<size_t>(row)];
    std::string user_id = user.id;

    QDialog dlg(this);
    dlg.setWindowTitle("Resetar senha");
    dlg.setMinimumWidth(360);

    auto* form = new QFormLayout(&dlg);
    auto* password_input = new QLineEdit;
    auto* confirm_input = new QLineEdit;
    password_input->setEchoMode(QLineEdit::Password);
    confirm_input->setEchoMode(QLineEdit::Password);

    form->addRow("Nova senha:", password_input);
    form->addRow("Confirmar:", confirm_input);

    auto* buttons = new QHBoxLayout;
    auto* ok_btn = new QPushButton("Resetar");
    auto* cancel_btn = new QPushButton("Cancelar");
    buttons->addStretch();
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);
    form->addRow(buttons);

    connect(ok_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString password = password_input->text();
    QString confirm = confirm_input->text();

    if (password.isEmpty()) {
        QMessageBox::warning(this, "Erro", "Informe a nova senha.");
        return;
    }
    if (password.size() < 8) {
        QMessageBox::warning(this, "Erro", "A senha deve ter pelo menos 8 caracteres.");
        return;
    }
    if (password != confirm) {
        QMessageBox::warning(this, "Erro", "As senhas nao conferem.");
        return;
    }

    action_in_flight_ = true;
    ++action_request_id_;
    quint64 req_id = action_request_id_;
    SetStatus("Resetando senha...");
    UpdateUiState();

    auto* auth = auth_;
    std::string pwd = password.toStdString();

    auto* watcher = new QFutureWatcher<UserActionResult>(this);
    connect(watcher, &QFutureWatcher<UserActionResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != action_request_id_) return;
        action_in_flight_ = false;

        auto result = watcher->result();
        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            UpdateUiState();
            return;
        }

        SetStatus("Senha resetada com sucesso.");
        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth, user_id, pwd, req_id]() -> UserActionResult {
        UserActionResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        auto r = client.ResetCompanyUserPassword(user_id, pwd);
        if (!r.success) {
            result.error = r.error.empty()
                ? QString("Falha ao resetar senha")
                : QString::fromStdString(r.error);
        }
        return result;
    }));
}

} // namespace remotex::viewer
