#pragma once

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>

#include <vector>

namespace remotex::viewer {

class MachineEditDialog : public QDialog {
    Q_OBJECT

public:
    MachineEditDialog(remotex::core::AuthClient* auth,
                      const remotex::core::MachineInfo& machine,
                      QWidget* parent = nullptr);

private slots:
    void OnGroupChanged(int index);
    void OnSave();

private:
    void SetupUi();
    void SetStatus(const QString& message, bool is_error = false);
    void UpdateUiState();
    void LoadGroups();
    void LoadSubgroups(const QString& selected_group_id, const QString& selected_subgroup_id = {});
    QString SelectedGroupId() const;
    QString SelectedSubgroupId() const;

    remotex::core::AuthClient*                    auth_ = nullptr;
    remotex::core::MachineInfo                    machine_;
    std::vector<remotex::core::MachineGroupInfo>  groups_;
    std::vector<remotex::core::MachineSubgroupInfo> subgroups_;
    quint64                                       groups_request_id_ = 0;
    quint64                                       subgroups_request_id_ = 0;
    quint64                                       save_request_id_ = 0;
    bool                                          groups_loading_ = false;
    bool                                          subgroups_loading_ = false;
    bool                                          save_in_flight_ = false;

    QLineEdit* name_edit_ = nullptr;
    QComboBox* group_combo_ = nullptr;
    QComboBox* subgroup_combo_ = nullptr;
    QLineEdit* os_info_edit_ = nullptr;
    QLineEdit* mac_edit_ = nullptr;
    QLineEdit* legal_name_edit_ = nullptr;
    QLineEdit* trade_name_edit_ = nullptr;
    QComboBox* document_type_combo_ = nullptr;
    QLineEdit* document_number_edit_ = nullptr;
    QCheckBox* managers_only_access_checkbox_ = nullptr;
    QCheckBox* require_client_consent_checkbox_ = nullptr;
    QLabel* status_label_ = nullptr;
    QDialogButtonBox* button_box_ = nullptr;
};

} // namespace remotex::viewer
