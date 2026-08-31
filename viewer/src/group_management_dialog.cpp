#include "viewer/group_management_dialog.h"

#include "viewer/connection_settings.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QVBoxLayout>

namespace remotex::viewer {

namespace {

struct GroupLoadResult {
    quint64 request_id = 0;
    QString selected_group_id;
    std::vector<remotex::core::MachineGroupInfo> groups;
    QString error;
};

struct SubgroupLoadResult {
    quint64 request_id = 0;
    QString group_id;
    QString selected_subgroup_id;
    std::vector<remotex::core::MachineSubgroupInfo> subgroups;
    QString error;
};

struct GroupActionResult {
    quint64 request_id = 0;
    QString selected_group_id;
    QString selected_subgroup_id;
    QString error;
    bool reload_groups = false;
    bool reload_subgroups = false;
};

remotex::core::MachineClient BuildDialogClient(remotex::core::AuthClient* auth) {
    remotex::core::MachineClient client;
    client.Configure(ConnectionSettings::Load().ToMachineClientConfig(), auth);
    return client;
}

QString ErrorOrFallback(const std::string& error, const QString& fallback) {
    return error.empty() ? fallback : QString::fromStdString(error);
}

} // namespace

GroupManagementDialog::GroupManagementDialog(remotex::core::AuthClient* auth,
                                             QWidget* parent)
    : QDialog(parent)
    , auth_(auth) {
    SetupUi();
    ReloadGroups();
}

bool GroupManagementDialog::HasChanges() const {
    return dirty_;
}

void GroupManagementDialog::SetupUi() {
    setWindowTitle("Grupos e subgrupos");
    resize(760, 420);

    auto* root = new QVBoxLayout(this);
    auto* content = new QHBoxLayout;

    auto* groups_layout = new QVBoxLayout;
    groups_layout->addWidget(new QLabel("Grupos"));
    group_list_ = new QListWidget;
    groups_layout->addWidget(group_list_, 1);

    auto* group_buttons = new QHBoxLayout;
    add_group_button_ = new QPushButton("Adicionar");
    edit_group_button_ = new QPushButton("Editar");
    delete_group_button_ = new QPushButton("Excluir");
    group_buttons->addWidget(add_group_button_);
    group_buttons->addWidget(edit_group_button_);
    group_buttons->addWidget(delete_group_button_);
    groups_layout->addLayout(group_buttons);

    auto* subgroups_layout = new QVBoxLayout;
    subgroups_layout->addWidget(new QLabel("Subgrupos"));
    subgroup_list_ = new QListWidget;
    subgroups_layout->addWidget(subgroup_list_, 1);

    auto* subgroup_buttons = new QHBoxLayout;
    add_subgroup_button_ = new QPushButton("Adicionar");
    edit_subgroup_button_ = new QPushButton("Editar");
    delete_subgroup_button_ = new QPushButton("Excluir");
    subgroup_buttons->addWidget(add_subgroup_button_);
    subgroup_buttons->addWidget(edit_subgroup_button_);
    subgroup_buttons->addWidget(delete_subgroup_button_);
    subgroups_layout->addLayout(subgroup_buttons);

    content->addLayout(groups_layout, 1);
    content->addLayout(subgroups_layout, 1);
    root->addLayout(content, 1);

    status_label_ = new QLabel;
    status_label_->setWordWrap(true);
    root->addWidget(status_label_);

    button_box_ = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(button_box_, &QDialogButtonBox::rejected, this, &GroupManagementDialog::reject);
    connect(button_box_, &QDialogButtonBox::accepted, this, &GroupManagementDialog::accept);
    root->addWidget(button_box_);

    connect(group_list_, &QListWidget::itemSelectionChanged,
            this, &GroupManagementDialog::OnGroupSelectionChanged);
    connect(subgroup_list_, &QListWidget::itemSelectionChanged, this, [this]() {
        UpdateUiState();
    });
    connect(add_group_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnAddGroup);
    connect(edit_group_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnEditGroup);
    connect(delete_group_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnDeleteGroup);
    connect(add_subgroup_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnAddSubgroup);
    connect(edit_subgroup_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnEditSubgroup);
    connect(delete_subgroup_button_, &QPushButton::clicked, this, &GroupManagementDialog::OnDeleteSubgroup);

    UpdateUiState();
}

void GroupManagementDialog::SetStatus(const QString& message, bool is_error) {
    if (!status_label_) {
        return;
    }

    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? "color: #b3261e;" : "color: #666666;");
}

void GroupManagementDialog::UpdateUiState() {
    const bool has_group = SelectedGroup().has_value();
    const bool has_subgroup = SelectedSubgroup().has_value();

    if (group_list_) {
        group_list_->setEnabled(!groups_loading_ && !action_in_flight_);
    }
    if (subgroup_list_) {
        subgroup_list_->setEnabled(!subgroups_loading_ && !action_in_flight_ && has_group);
    }
    if (add_group_button_) {
        add_group_button_->setEnabled(!groups_loading_ && !action_in_flight_);
    }
    if (edit_group_button_) {
        edit_group_button_->setEnabled(!groups_loading_ && !action_in_flight_ && has_group);
    }
    if (delete_group_button_) {
        delete_group_button_->setEnabled(!groups_loading_ && !action_in_flight_ && has_group);
    }
    if (add_subgroup_button_) {
        add_subgroup_button_->setEnabled(!groups_loading_ && !subgroups_loading_ && !action_in_flight_ && has_group);
    }
    if (edit_subgroup_button_) {
        edit_subgroup_button_->setEnabled(!groups_loading_ && !subgroups_loading_ && !action_in_flight_ && has_subgroup);
    }
    if (delete_subgroup_button_) {
        delete_subgroup_button_->setEnabled(!groups_loading_ && !subgroups_loading_ && !action_in_flight_ && has_subgroup);
    }
    if (button_box_) {
        if (auto* close_button = button_box_->button(QDialogButtonBox::Close)) {
            close_button->setEnabled(!action_in_flight_);
        }
    }
}

void GroupManagementDialog::ReloadGroups(const QString& selected_group_id) {
    const quint64 request_id = ++groups_request_id_;
    groups_loading_ = true;
    SetStatus("Carregando grupos...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupLoadResult>(this);
    connect(watcher, &QFutureWatcher<GroupLoadResult>::finished, this, [this, watcher]() {
        const GroupLoadResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != groups_request_id_) {
            return;
        }

        groups_loading_ = false;
        groups_ = result.groups;

        group_list_->blockSignals(true);
        group_list_->clear();

        int selected_row = -1;
        for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
            const auto& group = groups_[i];
            auto* item = new QListWidgetItem(QString::fromStdString(group.name));
            item->setData(Qt::UserRole, QString::fromStdString(group.id));
            group_list_->addItem(item);
            if (!result.selected_group_id.isEmpty()
                    && result.selected_group_id == item->data(Qt::UserRole).toString()) {
                selected_row = i;
            }
        }

        if (selected_row >= 0) {
            group_list_->setCurrentRow(selected_row);
        } else if (group_list_->count() > 0) {
            group_list_->setCurrentRow(0);
        }
        group_list_->blockSignals(false);

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
        }

        UpdateUiState();
        ReloadSubgroups();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, selected_group_id]() {
        GroupLoadResult result;
        result.request_id = request_id;
        result.selected_group_id = selected_group_id;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        result.groups = client.ListGroups();
        return result;
    }));
}

void GroupManagementDialog::ReloadSubgroups(const QString& selected_subgroup_id) {
    auto group = SelectedGroup();
    if (!group.has_value()) {
        ++subgroups_request_id_;
        subgroups_loading_ = false;
        subgroups_.clear();
        subgroup_list_->clear();
        if (!action_in_flight_ && !groups_loading_) {
            SetStatus({});
        }
        UpdateUiState();
        return;
    }

    const QString group_id = QString::fromStdString(group->id);
    const quint64 request_id = ++subgroups_request_id_;
    subgroups_loading_ = true;
    SetStatus("Carregando subgrupos...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<SubgroupLoadResult>(this);
    connect(watcher, &QFutureWatcher<SubgroupLoadResult>::finished, this, [this, watcher]() {
        const SubgroupLoadResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != subgroups_request_id_) {
            return;
        }

        subgroups_loading_ = false;
        subgroups_ = result.subgroups;

        subgroup_list_->blockSignals(true);
        subgroup_list_->clear();

        int selected_row = -1;
        for (int i = 0; i < static_cast<int>(subgroups_.size()); ++i) {
            const auto& subgroup = subgroups_[i];
            auto* item = new QListWidgetItem(QString::fromStdString(subgroup.name));
            item->setData(Qt::UserRole, QString::fromStdString(subgroup.id));
            subgroup_list_->addItem(item);
            if (!result.selected_subgroup_id.isEmpty()
                    && result.selected_subgroup_id == item->data(Qt::UserRole).toString()) {
                selected_row = i;
            }
        }

        if (selected_row >= 0) {
            subgroup_list_->setCurrentRow(selected_row);
        } else if (subgroup_list_->count() > 0) {
            subgroup_list_->setCurrentRow(0);
        }
        subgroup_list_->blockSignals(false);

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
        } else if (!action_in_flight_) {
            SetStatus({});
        }

        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, group_id, selected_subgroup_id]() {
        SubgroupLoadResult result;
        result.request_id = request_id;
        result.group_id = group_id;
        result.selected_subgroup_id = selected_subgroup_id;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        result.subgroups = client.ListSubgroups(group_id.toStdString());
        return result;
    }));
}

std::optional<remotex::core::MachineGroupInfo> GroupManagementDialog::SelectedGroup() const {
    const int row = group_list_ ? group_list_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(groups_.size())) {
        return std::nullopt;
    }
    return groups_[row];
}

std::optional<remotex::core::MachineSubgroupInfo> GroupManagementDialog::SelectedSubgroup() const {
    const int row = subgroup_list_ ? subgroup_list_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(subgroups_.size())) {
        return std::nullopt;
    }
    return subgroups_[row];
}

void GroupManagementDialog::OnGroupSelectionChanged() {
    ReloadSubgroups();
}

void GroupManagementDialog::OnAddGroup() {
    if (action_in_flight_) {
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, "Novo grupo", "Nome do grupo:",
        QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Criando grupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        if (result.reload_groups) {
            ReloadGroups(result.selected_group_id);
        } else if (result.reload_subgroups) {
            ReloadSubgroups(result.selected_subgroup_id);
        }
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, name]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_groups = true;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto create_result = client.CreateGroup(name.toStdString());
        if (!create_result.group.has_value()) {
            result.error = ErrorOrFallback(create_result.error, "Falha ao criar grupo.");
            return result;
        }

        result.selected_group_id = QString::fromStdString(create_result.group->id);
        return result;
    }));
}

void GroupManagementDialog::OnEditGroup() {
    if (action_in_flight_) {
        return;
    }

    auto group = SelectedGroup();
    if (!group.has_value()) {
        QMessageBox::information(this, "Editar grupo", "Selecione um grupo.");
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, "Editar grupo", "Nome do grupo:",
        QLineEdit::Normal, QString::fromStdString(group->name), &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Atualizando grupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        ReloadGroups(result.selected_group_id);
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, group_id = group->id, name]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_groups = true;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto update_result = client.UpdateGroup(group_id, name.toStdString());
        if (!update_result.group.has_value()) {
            result.error = ErrorOrFallback(update_result.error, "Falha ao atualizar grupo.");
            return result;
        }

        result.selected_group_id = QString::fromStdString(update_result.group->id);
        return result;
    }));
}

void GroupManagementDialog::OnDeleteGroup() {
    if (action_in_flight_) {
        return;
    }

    auto group = SelectedGroup();
    if (!group.has_value()) {
        QMessageBox::information(this, "Excluir grupo", "Selecione um grupo.");
        return;
    }

    if (QMessageBox::question(this, "Excluir grupo",
            QString("Excluir o grupo \"%1\"?").arg(QString::fromStdString(group->name)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Excluindo grupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        ReloadGroups();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, group_id = group->id]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_groups = true;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto delete_result = client.DeleteGroup(group_id);
        if (!delete_result.success) {
            result.error = ErrorOrFallback(delete_result.error, "Falha ao excluir grupo.");
        }
        return result;
    }));
}

void GroupManagementDialog::OnAddSubgroup() {
    if (action_in_flight_) {
        return;
    }

    auto group = SelectedGroup();
    if (!group.has_value()) {
        QMessageBox::information(this, "Novo subgrupo", "Selecione um grupo.");
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, "Novo subgrupo", "Nome do subgrupo:",
        QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Criando subgrupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        ReloadSubgroups(result.selected_subgroup_id);
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id,
                                          group_id = group->id,
                                          name]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_subgroups = true;
        result.selected_group_id = QString::fromStdString(group_id);
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto create_result = client.CreateSubgroup(group_id, name.toStdString());
        if (!create_result.subgroup.has_value()) {
            result.error = ErrorOrFallback(create_result.error, "Falha ao criar subgrupo.");
            return result;
        }

        result.selected_subgroup_id = QString::fromStdString(create_result.subgroup->id);
        return result;
    }));
}

void GroupManagementDialog::OnEditSubgroup() {
    if (action_in_flight_) {
        return;
    }

    auto subgroup = SelectedSubgroup();
    if (!subgroup.has_value()) {
        QMessageBox::information(this, "Editar subgrupo", "Selecione um subgrupo.");
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, "Editar subgrupo", "Nome do subgrupo:",
        QLineEdit::Normal, QString::fromStdString(subgroup->name), &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Atualizando subgrupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        ReloadSubgroups(result.selected_subgroup_id);
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id,
                                          subgroup_id = subgroup->id,
                                          name]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_subgroups = true;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto update_result = client.UpdateSubgroup(subgroup_id, name.toStdString());
        if (!update_result.subgroup.has_value()) {
            result.error = ErrorOrFallback(update_result.error, "Falha ao atualizar subgrupo.");
            return result;
        }

        result.selected_subgroup_id = QString::fromStdString(update_result.subgroup->id);
        return result;
    }));
}

void GroupManagementDialog::OnDeleteSubgroup() {
    if (action_in_flight_) {
        return;
    }

    auto subgroup = SelectedSubgroup();
    if (!subgroup.has_value()) {
        QMessageBox::information(this, "Excluir subgrupo", "Selecione um subgrupo.");
        return;
    }

    if (QMessageBox::question(this, "Excluir subgrupo",
            QString("Excluir o subgrupo \"%1\"?").arg(QString::fromStdString(subgroup->name)),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const quint64 request_id = ++action_request_id_;
    action_in_flight_ = true;
    SetStatus("Excluindo subgrupo...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<GroupActionResult>(this);
    connect(watcher, &QFutureWatcher<GroupActionResult>::finished, this, [this, watcher]() {
        const GroupActionResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != action_request_id_) {
            return;
        }

        action_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            QMessageBox::warning(this, "Erro", result.error);
            return;
        }

        dirty_ = true;
        ReloadSubgroups();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, subgroup_id = subgroup->id]() {
        GroupActionResult result;
        result.request_id = request_id;
        result.reload_subgroups = true;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        const auto delete_result = client.DeleteSubgroup(subgroup_id);
        if (!delete_result.success) {
            result.error = ErrorOrFallback(delete_result.error, "Falha ao excluir subgrupo.");
        }
        return result;
    }));
}

} // namespace remotex::viewer
