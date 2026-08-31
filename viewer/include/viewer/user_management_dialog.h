#pragma once

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <vector>

namespace remotex::viewer {

class UserManagementDialog : public QDialog {
    Q_OBJECT

public:
    explicit UserManagementDialog(remotex::core::AuthClient* auth,
                                   QWidget* parent = nullptr);

private slots:
    void OnAddUser();
    void OnToggleActive();
    void OnResetPassword();

private:
    void SetupUi();
    void SetStatus(const QString& message, bool is_error = false);
    void UpdateUiState();
    void ReloadUsers();
    void PopulateTable();
    int GetSelectedRow() const;

    remotex::core::AuthClient* auth_ = nullptr;
    std::vector<remotex::core::CompanyUserInfo> users_;
    quint64 load_request_id_ = 0;
    quint64 action_request_id_ = 0;
    bool loading_ = false;
    bool action_in_flight_ = false;

    QTableWidget* table_ = nullptr;
    QPushButton*  add_btn_ = nullptr;
    QPushButton*  toggle_btn_ = nullptr;
    QPushButton*  reset_pwd_btn_ = nullptr;
    QPushButton*  close_btn_ = nullptr;
    QLabel*       status_label_ = nullptr;
};

} // namespace remotex::viewer
