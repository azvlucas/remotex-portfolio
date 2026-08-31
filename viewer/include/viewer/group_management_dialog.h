#pragma once

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include <optional>
#include <vector>

namespace remotex::viewer {

class GroupManagementDialog : public QDialog {
    Q_OBJECT

public:
    explicit GroupManagementDialog(remotex::core::AuthClient* auth,
                                   QWidget* parent = nullptr);

    bool HasChanges() const;

private slots:
    void OnGroupSelectionChanged();
    void OnAddGroup();
    void OnEditGroup();
    void OnDeleteGroup();
    void OnAddSubgroup();
    void OnEditSubgroup();
    void OnDeleteSubgroup();

private:
    void SetupUi();
    void SetStatus(const QString& message, bool is_error = false);
    void UpdateUiState();
    void ReloadGroups(const QString& selected_group_id = {});
    void ReloadSubgroups(const QString& selected_subgroup_id = {});
    std::optional<remotex::core::MachineGroupInfo> SelectedGroup() const;
    std::optional<remotex::core::MachineSubgroupInfo> SelectedSubgroup() const;

    remotex::core::AuthClient*                     auth_ = nullptr;
    std::vector<remotex::core::MachineGroupInfo>   groups_;
    std::vector<remotex::core::MachineSubgroupInfo> subgroups_;
    bool                                           dirty_ = false;
    quint64                                        groups_request_id_ = 0;
    quint64                                        subgroups_request_id_ = 0;
    quint64                                        action_request_id_ = 0;
    bool                                           groups_loading_ = false;
    bool                                           subgroups_loading_ = false;
    bool                                           action_in_flight_ = false;

    QListWidget* group_list_ = nullptr;
    QListWidget* subgroup_list_ = nullptr;
    QPushButton* add_group_button_ = nullptr;
    QPushButton* edit_group_button_ = nullptr;
    QPushButton* delete_group_button_ = nullptr;
    QPushButton* add_subgroup_button_ = nullptr;
    QPushButton* edit_subgroup_button_ = nullptr;
    QPushButton* delete_subgroup_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QDialogButtonBox* button_box_ = nullptr;
};

} // namespace remotex::viewer
