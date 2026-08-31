#include "viewer/machine_edit_dialog.h"

#include "viewer/connection_settings.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace remotex::viewer {

namespace {

struct GroupLoadResult {
    quint64 request_id = 0;
    QString selected_group_id;
    QString selected_subgroup_id;
    std::vector<remotex::core::MachineGroupInfo> groups;
    QString error;
};

struct SubgroupLoadResult {
    quint64 request_id = 0;
    QString selected_group_id;
    QString selected_subgroup_id;
    std::vector<remotex::core::MachineSubgroupInfo> subgroups;
    QString error;
};

struct SaveMachineResult {
    quint64 request_id = 0;
    remotex::core::MachineInfoResult result;
    QString error;
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

MachineEditDialog::MachineEditDialog(remotex::core::AuthClient* auth,
                                     const remotex::core::MachineInfo& machine,
                                     QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , machine_(machine) {
    SetupUi();
    LoadGroups();
}

void MachineEditDialog::SetupUi() {
    setWindowTitle("Editar maquina");
    resize(520, 0);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    name_edit_ = new QLineEdit(QString::fromStdString(machine_.name));
    form->addRow("Nome:", name_edit_);

    group_combo_ = new QComboBox;
    group_combo_->addItem("Carregando...", "");
    form->addRow("Grupo:", group_combo_);

    subgroup_combo_ = new QComboBox;
    subgroup_combo_->addItem("Sem subgrupo", "");
    form->addRow("Subgrupo:", subgroup_combo_);

    os_info_edit_ = new QLineEdit(QString::fromStdString(machine_.os_info));
    form->addRow("Sistema operacional:", os_info_edit_);

    mac_edit_ = new QLineEdit(QString::fromStdString(machine_.mac_address));
    mac_edit_->setPlaceholderText("AA:BB:CC:DD:EE:FF");
    form->addRow("MAC address:", mac_edit_);

    legal_name_edit_ = new QLineEdit(QString::fromStdString(machine_.legal_name));
    form->addRow("Razao social:", legal_name_edit_);

    trade_name_edit_ = new QLineEdit(QString::fromStdString(machine_.trade_name));
    form->addRow("Nome fantasia:", trade_name_edit_);

    document_type_combo_ = new QComboBox;
    document_type_combo_->addItem("Sem documento", "");
    document_type_combo_->addItem("CPF", "cpf");
    document_type_combo_->addItem("CNPJ", "cnpj");
    const QString current_document_type = QString::fromStdString(machine_.document_type);
    const int document_type_index = document_type_combo_->findData(current_document_type);
    document_type_combo_->setCurrentIndex(document_type_index >= 0 ? document_type_index : 0);
    form->addRow("Tipo documento:", document_type_combo_);

    document_number_edit_ = new QLineEdit(QString::fromStdString(machine_.document_number));
    document_number_edit_->setPlaceholderText("Somente numeros ou mascara livre");
    form->addRow("Numero documento:", document_number_edit_);

    managers_only_access_checkbox_ = new QCheckBox("Apenas gerentes podem acessar");
    managers_only_access_checkbox_->setChecked(machine_.managers_only_access);
    form->addRow("Permissao:", managers_only_access_checkbox_);

    require_client_consent_checkbox_ = new QCheckBox("Solicitar acesso ao cliente antes de acessar");
    require_client_consent_checkbox_->setChecked(machine_.require_client_consent);
    form->addRow("", require_client_consent_checkbox_);

    layout->addLayout(form);

    status_label_ = new QLabel;
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    button_box_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(button_box_, &QDialogButtonBox::accepted, this, &MachineEditDialog::OnSave);
    connect(button_box_, &QDialogButtonBox::rejected, this, &MachineEditDialog::reject);
    connect(group_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MachineEditDialog::OnGroupChanged);

    layout->addWidget(button_box_);
    UpdateUiState();
}

void MachineEditDialog::SetStatus(const QString& message, bool is_error) {
    if (!status_label_) {
        return;
    }

    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? "color: #b3261e;" : "color: #666666;");
}

void MachineEditDialog::UpdateUiState() {
    const bool group_ready = !groups_loading_ && !save_in_flight_;
    const bool subgroup_ready = !groups_loading_
        && !subgroups_loading_
        && !save_in_flight_
        && !SelectedGroupId().isEmpty();

    if (name_edit_) {
        name_edit_->setEnabled(!save_in_flight_);
    }
    if (group_combo_) {
        group_combo_->setEnabled(group_ready);
    }
    if (subgroup_combo_) {
        subgroup_combo_->setEnabled(subgroup_ready);
    }
    if (os_info_edit_) {
        os_info_edit_->setEnabled(!save_in_flight_);
    }
    if (mac_edit_) {
        mac_edit_->setEnabled(!save_in_flight_);
    }
    if (legal_name_edit_) {
        legal_name_edit_->setEnabled(!save_in_flight_);
    }
    if (trade_name_edit_) {
        trade_name_edit_->setEnabled(!save_in_flight_);
    }
    if (document_type_combo_) {
        document_type_combo_->setEnabled(!save_in_flight_);
    }
    if (document_number_edit_) {
        document_number_edit_->setEnabled(!save_in_flight_);
    }
    if (managers_only_access_checkbox_) {
        managers_only_access_checkbox_->setEnabled(!save_in_flight_);
    }
    if (require_client_consent_checkbox_) {
        require_client_consent_checkbox_->setEnabled(!save_in_flight_);
    }
    if (button_box_) {
        if (auto* save_button = button_box_->button(QDialogButtonBox::Save)) {
            save_button->setEnabled(!groups_loading_ && !subgroups_loading_ && !save_in_flight_);
        }
        if (auto* cancel_button = button_box_->button(QDialogButtonBox::Cancel)) {
            cancel_button->setEnabled(!save_in_flight_);
        }
    }
}

void MachineEditDialog::LoadGroups() {
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

        group_combo_->blockSignals(true);
        group_combo_->clear();
        group_combo_->addItem("Sem grupo", "");
        for (const auto& group : groups_) {
            group_combo_->addItem(QString::fromStdString(group.name), QString::fromStdString(group.id));
        }

        const int group_index = group_combo_->findData(result.selected_group_id);
        group_combo_->setCurrentIndex(group_index >= 0 ? group_index : 0);
        group_combo_->blockSignals(false);

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
        }

        UpdateUiState();
        LoadSubgroups(SelectedGroupId(), result.selected_subgroup_id);
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id,
                                          selected_group_id = QString::fromStdString(machine_.group_id),
                                          selected_subgroup_id = QString::fromStdString(machine_.subgroup_id)]() {
        GroupLoadResult result;
        result.request_id = request_id;
        result.selected_group_id = selected_group_id;
        result.selected_subgroup_id = selected_subgroup_id;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        result.groups = client.ListGroups();
        return result;
    }));
}

void MachineEditDialog::LoadSubgroups(const QString& selected_group_id, const QString& selected_subgroup_id) {
    if (selected_group_id.isEmpty()) {
        ++subgroups_request_id_;
        subgroups_loading_ = false;
        subgroups_.clear();
        subgroup_combo_->blockSignals(true);
        subgroup_combo_->clear();
        subgroup_combo_->addItem("Sem subgrupo", "");
        subgroup_combo_->setCurrentIndex(0);
        subgroup_combo_->blockSignals(false);
        UpdateUiState();
        return;
    }

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

        subgroup_combo_->blockSignals(true);
        subgroup_combo_->clear();
        subgroup_combo_->addItem("Sem subgrupo", "");
        for (const auto& subgroup : subgroups_) {
            subgroup_combo_->addItem(QString::fromStdString(subgroup.name), QString::fromStdString(subgroup.id));
        }

        const int subgroup_index = subgroup_combo_->findData(result.selected_subgroup_id);
        subgroup_combo_->setCurrentIndex(subgroup_index >= 0 ? subgroup_index : 0);
        subgroup_combo_->blockSignals(false);

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
        } else if (!save_in_flight_) {
            SetStatus({});
        }

        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id, selected_group_id, selected_subgroup_id]() {
        SubgroupLoadResult result;
        result.request_id = request_id;
        result.selected_group_id = selected_group_id;
        result.selected_subgroup_id = selected_subgroup_id;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        result.subgroups = client.ListSubgroups(selected_group_id.toStdString());
        return result;
    }));
}

QString MachineEditDialog::SelectedGroupId() const {
    return group_combo_ ? group_combo_->currentData().toString() : QString{};
}

QString MachineEditDialog::SelectedSubgroupId() const {
    return subgroup_combo_ ? subgroup_combo_->currentData().toString() : QString{};
}

void MachineEditDialog::OnGroupChanged(int /*index*/) {
    LoadSubgroups(SelectedGroupId());
}

void MachineEditDialog::OnSave() {
    if (save_in_flight_) {
        return;
    }

    remotex::core::MachineUpdateRequest request;
    request.name = name_edit_->text().trimmed().toStdString();
    request.os_info = os_info_edit_->text().trimmed().toStdString();
    request.mac_address = mac_edit_->text().trimmed().toStdString();
    request.group_id = SelectedGroupId().toStdString();
    request.subgroup_id = SelectedSubgroupId().toStdString();
    request.legal_name = legal_name_edit_->text().trimmed().toStdString();
    request.trade_name = trade_name_edit_->text().trimmed().toStdString();
    request.document_type = document_type_combo_->currentData().toString().toStdString();
    request.document_number = document_number_edit_->text().trimmed().toStdString();
    request.managers_only_access = managers_only_access_checkbox_ && managers_only_access_checkbox_->isChecked();
    request.require_client_consent = require_client_consent_checkbox_ && require_client_consent_checkbox_->isChecked();

    const quint64 request_id = ++save_request_id_;
    save_in_flight_ = true;
    SetStatus("Salvando maquina...");
    UpdateUiState();

    auto* watcher = new QFutureWatcher<SaveMachineResult>(this);
    connect(watcher, &QFutureWatcher<SaveMachineResult>::finished, this, [this, watcher]() {
        const SaveMachineResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != save_request_id_) {
            return;
        }

        save_in_flight_ = false;
        UpdateUiState();

        if (!result.error.isEmpty() || !result.result.machine.has_value()) {
            const QString error = !result.error.isEmpty()
                ? result.error
                : ErrorOrFallback(result.result.error, "Falha ao atualizar maquina.");
            SetStatus(error, true);
            QMessageBox::warning(this, "Erro", error);
            return;
        }

        accept();
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, request_id,
                                          machine_id = machine_.id,
                                          request = std::move(request)]() mutable {
        SaveMachineResult result;
        result.request_id = request_id;
        if (!auth) {
            result.error = "Cliente de autenticacao indisponivel.";
            return result;
        }

        auto client = BuildDialogClient(auth);
        result.result = client.UpdateMachineDetails(machine_id, request);
        return result;
    }));
}

} // namespace remotex::viewer
