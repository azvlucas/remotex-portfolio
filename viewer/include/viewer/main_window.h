#pragma once
// RemoteX - viewer/main_window.h
// Tela principal: lista de maquinas, toolbar e refresh automatico.

#include "viewer/auto_updater.h"
#include "viewer/connection_settings.h"
#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <vector>

namespace remotex::viewer {

class MainWindow : public QMainWindow, public ViewerAutoUpdaterListener {
    Q_OBJECT

public:
    explicit MainWindow(remotex::core::AuthClient* auth, QWidget* parent = nullptr);
    ~MainWindow() override;

    // ViewerAutoUpdaterListener
    void OnUpdateAvailable(const std::string& version) override;
    void OnDownloadProgress(int64_t downloaded, int64_t total) override;
    void OnUpdateReady(const std::string& version) override;
    void OnUpdateError(const std::string& error) override;

signals:
    void LoggedOut();

private slots:
    void RefreshMachines();
    void OnEditMachine();
    void OnDeleteMachine();
    void OnConnect();
    void OnWatchOnly();
    void OnChat();
    void OnSettings();
    void OnManageGroups();
    void OnManageUsers();
    void OnReports();
    void OnLogout();
    void OnTableDoubleClicked(int row, int column);
    void OnSearchTextChanged(const QString& text);
    void OnStatusFilterChanged(int index);
    void OnMachineSelectionChanged();
    void OnAuditFilterChanged(int index);
    void OnAuditLoadMore();
    void OnAuditDownloadRecording();
    void OnAuditSelectionChanged();
    void OnAuditTableContextMenu(const QPoint& pos);
    void OnTableContextMenu(const QPoint& pos);
    void OnUpdateNow();
    void OnForceHostUpdate();

private:
    void SetupUI();
    void SetupToolbar();
    void ApplyConnectionSettings();
    void ApplyFilters();
    void PopulateMachineTable();
    void LoadAuditForMachine(const std::string& machine_id, bool append);
    void PopulateAuditTable();
    void UpdateAuditSummary();
    void ClearAuditPanel(const QString& message = QString());
    void RestoreSelectionByMachineId(const std::string& machine_id);
    void OpenSelectedMachine(remotex::core::SessionAccessMode access_mode);
    bool AuditEventMatchesCurrentFilter(const remotex::core::MachineAuditEvent& event) const;
    void ShowUpdateBar(const QString& version);
    void UpdateSummary() const;
    void UpdateMachineActionState();
    int GetSelectedMachineIndex() const;
    std::string GetSelectedMachineId() const;
    bool CurrentUserIsManager() const;

    remotex::core::AuthClient*   auth_ = nullptr;
    remotex::core::MachineClient machine_client_;
    ConnectionSettings           connection_settings_;
    ViewerAutoUpdater            auto_updater_;

    QFrame*       update_bar_ = nullptr;
    QLabel*       update_label_ = nullptr;
    QPushButton*  update_btn_ = nullptr;
    bool          update_download_in_flight_ = false;
    QTableWidget* table_ = nullptr;
    QToolBar*     toolbar_ = nullptr;
    QLineEdit*    search_input_ = nullptr;
    QComboBox*    status_filter_ = nullptr;
    QLabel*       summary_label_ = nullptr;
    QLabel*       status_label_ = nullptr;
    QLabel*       audit_title_label_ = nullptr;
    QLabel*       audit_summary_label_ = nullptr;
    QLabel*       audit_empty_label_ = nullptr;
    QComboBox*    audit_filter_ = nullptr;
    QTableWidget* audit_table_ = nullptr;
    QPushButton*  audit_load_more_btn_ = nullptr;
    QPushButton*  audit_download_btn_ = nullptr;
    QTimer*       refresh_timer_ = nullptr;

    QAction* act_edit_ = nullptr;
    QAction* act_delete_ = nullptr;
    QAction* act_connect_ = nullptr;
    QAction* act_watch_only_ = nullptr;
    QAction* act_chat_ = nullptr;
    QAction* act_settings_ = nullptr;
    QAction* act_groups_ = nullptr;
    QAction* act_users_ = nullptr;
    QAction* act_reports_ = nullptr;
    QAction* act_logout_ = nullptr;
    QAction* act_refresh_ = nullptr;
    QToolButton* connect_button_ = nullptr;

    std::vector<remotex::core::MachineInfo> machines_;
    std::vector<int> visible_machine_indices_;
    std::vector<remotex::core::MachineAuditEvent> audit_events_;
    std::vector<remotex::core::RecordingInfo> machine_recordings_;
    std::string current_audit_machine_id_;
    int audit_offset_ = 0;
    bool audit_has_more_ = false;
    quint64 machine_request_id_ = 0;
    quint64 audit_request_id_ = 0;
    quint64 delete_request_id_ = 0;
    quint64 recording_download_request_id_ = 0;
    bool machine_refresh_in_flight_ = false;
    bool audit_load_in_flight_ = false;
    bool delete_in_flight_ = false;
    bool recording_download_in_flight_ = false;
};

} // namespace remotex::viewer
