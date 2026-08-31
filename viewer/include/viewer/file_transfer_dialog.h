#pragma once
// RemoteX - viewer/file_transfer_dialog.h

#include "viewer/remote_session_window.h"

#include <QDialog>

#include <functional>
#include <optional>
#include <QSet>

class QCloseEvent;
class QFileSystemModel;
class QLineEdit;
class QProgressBar;
class QTableWidget;
class QToolButton;
class QTreeView;

namespace remotex::viewer {

class FileTransferDialog : public QDialog {
    Q_OBJECT

public:
    struct DispatchHooks {
        std::function<void()> request_remote_roots;
        std::function<void(const QString&)> request_remote_listing;
        std::function<void(const QStringList&, const QString&)> send_paths;
        std::function<void(const QStringList&, const QString&)> receive_paths;
        std::function<void(const QString&, bool)> resolve_conflict;
        std::function<std::optional<bool>(const QString&, const QString&)> prompt_conflict_decision;
    };

    explicit FileTransferDialog(NetworkWorker* worker, QWidget* parent = nullptr);
    FileTransferDialog(NetworkWorker* worker, DispatchHooks dispatch_hooks, QWidget* parent = nullptr);
    ~FileTransferDialog() override;

private slots:
    void OnLocalActivated(const QModelIndex& index);
    void OnRemoteActivated(int row, int column);
    void OnLocalSelectionChanged();
    void OnRemoteSelectionChanged();
    void NavigateLocalUp();
    void RefreshLocal();
    void SetLocalRoot(const QString& path);
    void NavigateRemoteUp();
    void RefreshRemote();
    void SendSelection();
    void SendCurrentTree();
    void ReceiveSelection();
    void ReceiveCurrentTree();
    void ApplyRemoteListing(const remotex::viewer::RemoteListingResult& listing);
    void ApplyTransferConflictPreview(const QString& job_id, bool receiving, const QString& summary);
    void ApplyTransferConflictFailure(const QString& job_id, const QString& outcome, const QString& message);
    void ApplyTransferStarted(const QString& job_id, const QString& destination_root,
                              quint64 total_bytes, int total_entries, bool receiving);
    void ApplyTransferProgress(const QString& job_id, const QString& current_item,
                               quint64 bytes_done, quint64 total_bytes,
                               int entries_done, int total_entries);
    void ApplyTransferFinished(const QString& job_id, bool success,
                               const QString& destination_root, const QString& message);

private:
    static DispatchHooks BuildDispatchHooks(NetworkWorker* worker);

    void closeEvent(QCloseEvent* event) override;
    void SetupUi();
    void RebindLocalSelectionModel();
    void SetLocalRoots();
    void ApplyRemotePathState(bool at_roots, const QString& path);
    void RequestRemoteRoots();
    void RequestRemoteListing(const QString& path);
    void PopulateRemoteTable(const remotex::viewer::RemoteListingResult& listing);
    QString CurrentLocalDirectory() const;
    QStringList SelectedLocalPaths() const;
    QStringList SelectedRemotePaths() const;
    QStringList ListedLocalChildPaths() const;
    QStringList ListedRemoteChildPaths() const;
    QStringList CurrentLocalTreeTransferPaths() const;
    QStringList CurrentRemoteTreeTransferPaths() const;
    bool StartSendTransfer(const QStringList& source_paths,
                           const QString& remote_destination_root,
                           const QString& status_text);
    bool StartReceiveTransfer(const QStringList& remote_paths,
                              const QString& local_destination_root,
                              const QString& status_text);
    static bool IsDriveRootPath(const QString& path);
    void BeginConflictPreviewRequest(const QString& status_text);
    void ClearConflictPreviewState();
    void ResolveConflictPreview(bool allow_overwrite, const QString& status_text);
    void CancelPendingConflictPreview();
    std::optional<bool> PromptConflictDecision(const QString& title, const QString& message);
    QString DescribeTransferFailure(const QString& message, bool approved_conflict) const;
    void UpdateActionState();
    QString FormatSize(quint64 bytes) const;
    QString FormatTimestamp(qint64 unix_sec) const;

    NetworkWorker* worker_ = nullptr;
    DispatchHooks   dispatch_hooks_;

    QFileSystemModel* local_model_ = nullptr;
    QTreeView*        local_view_ = nullptr;
    QLineEdit*        local_path_edit_ = nullptr;
    QToolButton*      local_up_btn_ = nullptr;
    QToolButton*      local_refresh_btn_ = nullptr;
    QString           local_current_path_;
    bool              local_at_roots_ = false;

    QTableWidget* remote_table_ = nullptr;
    QLineEdit*    remote_path_edit_ = nullptr;
    QToolButton*  remote_up_btn_ = nullptr;
    QToolButton*  remote_refresh_btn_ = nullptr;
    QString       remote_current_path_;
    bool          remote_at_roots_ = true;

    QPushButton*  send_btn_ = nullptr;
    QPushButton*  send_tree_btn_ = nullptr;
    QPushButton*  receive_btn_ = nullptr;
    QPushButton*  receive_tree_btn_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QLabel*       progress_label_ = nullptr;
    QString       active_job_id_;
    QString       pending_conflict_job_id_;
    QString       approved_conflict_job_id_;
    QSet<QString> ignored_job_ids_;
    bool          pending_conflict_decision_ = false;
    bool          pending_conflict_receiving_ = false;
    bool          prompt_open_ = false;
};

} // namespace remotex::viewer
