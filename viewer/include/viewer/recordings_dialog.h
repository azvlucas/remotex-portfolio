#pragma once

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <vector>

namespace remotex::viewer {

class RecordingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit RecordingsDialog(remotex::core::AuthClient* auth,
                               QWidget* parent = nullptr);

private slots:
    void OnMachineFilterChanged(int index);
    void OnDownload();
    void OnDelete();

private:
    void SetupUi();
    void SetStatus(const QString& message, bool is_error = false);
    void UpdateUiState();
    void ReloadData();
    void PopulateTable();
    bool IsManager() const;
    int GetSelectedRow() const;

    remotex::core::AuthClient* auth_ = nullptr;
    std::vector<remotex::core::MachineInfo> machines_;
    std::vector<remotex::core::RecordingInfo> recordings_;
    quint64 load_request_id_ = 0;
    quint64 action_request_id_ = 0;
    bool loading_ = false;
    bool action_in_flight_ = false;

    QComboBox*    machine_filter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton*  download_btn_ = nullptr;
    QPushButton*  delete_btn_ = nullptr;
    QPushButton*  close_btn_ = nullptr;
    QLabel*       status_label_ = nullptr;
};

} // namespace remotex::viewer
