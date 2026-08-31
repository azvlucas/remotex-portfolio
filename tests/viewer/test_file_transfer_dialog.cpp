#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QModelIndex>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QWidget>

#include "viewer/file_transfer_dialog.h"

namespace {

class FileTransferDialogTest : public ::testing::Test {
protected:
    static QApplication* app_;
    static bool owns_app_;

    static void SetUpTestSuite() {
        if (QApplication::instance() == nullptr) {
            int argc = 1;
            char arg0[] = "test_file_transfer_dialog";
            char* argv[] = {arg0, nullptr};
            app_ = new QApplication(argc, argv);
            owns_app_ = true;
        } else {
            app_ = qobject_cast<QApplication*>(QApplication::instance());
            owns_app_ = false;
        }
    }

    static void TearDownTestSuite() {
        if (owns_app_) {
            delete app_;
            app_ = nullptr;
        }
    }
};

QApplication* FileTransferDialogTest::app_ = nullptr;
bool FileTransferDialogTest::owns_app_ = false;

constexpr int kRoleAbsolutePath = Qt::UserRole + 1;
constexpr int kRoleIsDirectory = Qt::UserRole + 2;

template <typename T>
T* FindWidget(remotex::viewer::FileTransferDialog& dialog, const char* object_name) {
    auto* widget = dialog.findChild<T*>(object_name);
    EXPECT_NE(widget, nullptr) << object_name;
    return widget;
}

remotex::viewer::RemoteFileItem MakeRemoteItem(const QString& name,
                                               const QString& absolute_path,
                                               bool is_directory,
                                               quint64 size_bytes = 0,
                                               qint64 modified_unix_sec = 1710000000) {
    remotex::viewer::RemoteFileItem entry;
    entry.name = name;
    entry.absolute_path = absolute_path;
    entry.is_directory = is_directory;
    entry.size_bytes = size_bytes;
    entry.modified_unix_sec = modified_unix_sec;
    return entry;
}

remotex::viewer::RemoteListingResult MakeListing(const QString& path,
                                                 bool is_root_listing,
                                                 std::initializer_list<remotex::viewer::RemoteFileItem> entries) {
    remotex::viewer::RemoteListingResult listing;
    listing.success = true;
    listing.path = path;
    listing.is_root_listing = is_root_listing;
    listing.entries = QList<remotex::viewer::RemoteFileItem>(entries.begin(), entries.end());
    return listing;
}

void ApplyListing(remotex::viewer::FileTransferDialog& dialog,
                  const remotex::viewer::RemoteListingResult& listing) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "ApplyRemoteListing",
                                                   Qt::DirectConnection,
                                                   Q_ARG(remotex::viewer::RemoteListingResult, listing));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

void InvokeSlot(remotex::viewer::FileTransferDialog& dialog, const char* slot_name) {
    const bool invoked = QMetaObject::invokeMethod(&dialog, slot_name, Qt::DirectConnection);
    ASSERT_TRUE(invoked) << slot_name;
    QCoreApplication::processEvents();
}

void OpenLocalDirectory(remotex::viewer::FileTransferDialog& dialog, const QString& path) {
    const QString normalized_path = QDir::cleanPath(path);
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "SetLocalRoot",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, normalized_path));
    ASSERT_TRUE(invoked) << normalized_path.toStdString();
    QCoreApplication::processEvents();
}

void ApplyConflictPreview(remotex::viewer::FileTransferDialog& dialog,
                          const QString& job_id,
                          bool receiving,
                          const QString& summary) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "ApplyTransferConflictPreview",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, job_id),
                                                   Q_ARG(bool, receiving),
                                                   Q_ARG(QString, summary));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

void ApplyConflictFailure(remotex::viewer::FileTransferDialog& dialog,
                          const QString& job_id,
                          const QString& outcome,
                          const QString& message) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "ApplyTransferConflictFailure",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, job_id),
                                                   Q_ARG(QString, outcome),
                                                   Q_ARG(QString, message));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

void ApplyTransferStarted(remotex::viewer::FileTransferDialog& dialog,
                          const QString& job_id,
                          const QString& destination_root,
                          quint64 total_bytes,
                          int total_entries,
                          bool receiving) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "ApplyTransferStarted",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, job_id),
                                                   Q_ARG(QString, destination_root),
                                                   Q_ARG(quint64, total_bytes),
                                                   Q_ARG(int, total_entries),
                                                   Q_ARG(bool, receiving));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

void ApplyTransferFinished(remotex::viewer::FileTransferDialog& dialog,
                           const QString& job_id,
                           bool success,
                           const QString& destination_root,
                           const QString& message) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "ApplyTransferFinished",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, job_id),
                                                   Q_ARG(bool, success),
                                                   Q_ARG(QString, destination_root),
                                                   Q_ARG(QString, message));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

void CreateFile(const QString& path, const QByteArray& payload = QByteArrayLiteral("payload")) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
    ASSERT_EQ(file.write(payload), payload.size()) << path.toStdString();
    file.close();
}

void DrainLocalModel(QFileSystemModel* model, const QModelIndex& root_index) {
    ASSERT_NE(model, nullptr);

    int guard = 0;
    while (model->canFetchMore(root_index) && guard++ < 64) {
        model->fetchMore(root_index);
        QCoreApplication::processEvents();
    }

    QCoreApplication::processEvents();
}

QStringList VisibleLocalPaths(remotex::viewer::FileTransferDialog& dialog) {
    auto* local_view = FindWidget<QTreeView>(dialog, "file_transfer_local_view");
    if (!local_view) {
        return {};
    }

    auto* local_model = qobject_cast<QFileSystemModel*>(local_view->model());
    EXPECT_NE(local_model, nullptr);
    if (!local_model) {
        return {};
    }

    const QModelIndex root_index = local_view->rootIndex();
    DrainLocalModel(local_model, root_index);

    QStringList paths;
    for (int row = 0; row < local_model->rowCount(root_index); ++row) {
        const QModelIndex child_index = local_model->index(row, 0, root_index);
        EXPECT_TRUE(child_index.isValid());
        if (!child_index.isValid()) {
            return {};
        }

        const QString path = QDir::cleanPath(local_model->filePath(child_index));
        EXPECT_FALSE(path.isEmpty());
        if (path.isEmpty()) {
            return {};
        }

        if (!paths.contains(path)) {
            paths.push_back(path);
        }
    }

    return paths;
}

QString VolumeRootForPath(const QString& path) {
    QDir directory(QDir::cleanPath(path));
    while (directory.cdUp()) {
    }
    return QDir::cleanPath(directory.absolutePath());
}

qint64 TimestampUtc(int year, int month, int day, int hour = 0, int minute = 0) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute), Qt::UTC).toSecsSinceEpoch();
}

struct ExpectedRemoteRow {
    QString name;
    QString absolute_path;
    bool is_directory;
};

void SortRemoteTable(QTableWidget* remote_table, int column, Qt::SortOrder order) {
    ASSERT_NE(remote_table, nullptr);
    remote_table->sortItems(column, order);
    QCoreApplication::processEvents();
}

void ExpectRemoteRows(QTableWidget* remote_table,
                      std::initializer_list<ExpectedRemoteRow> expected_rows) {
    ASSERT_NE(remote_table, nullptr);
    ASSERT_EQ(remote_table->rowCount(), static_cast<int>(expected_rows.size()));

    int row = 0;
    for (const auto& expected_row : expected_rows) {
        SCOPED_TRACE(::testing::Message() << "row " << row);
        auto* name_item = remote_table->item(row, 0);
        ASSERT_NE(name_item, nullptr);
        EXPECT_EQ(name_item->text(), expected_row.name);
        EXPECT_EQ(QDir::cleanPath(name_item->data(kRoleAbsolutePath).toString()),
                  QDir::cleanPath(expected_row.absolute_path));
        EXPECT_EQ(name_item->data(kRoleIsDirectory).toBool(), expected_row.is_directory);
        ++row;
    }
}

void ActivateRemoteRow(remotex::viewer::FileTransferDialog& dialog, int row) {
    const bool invoked = QMetaObject::invokeMethod(&dialog,
                                                   "OnRemoteActivated",
                                                   Qt::DirectConnection,
                                                   Q_ARG(int, row),
                                                   Q_ARG(int, 0));
    ASSERT_TRUE(invoked);
    QCoreApplication::processEvents();
}

QString ConfigureLocalSelection(remotex::viewer::FileTransferDialog& dialog, QTemporaryDir& temp_dir) {
    const QString local_directory = QDir::cleanPath(temp_dir.path());
    const QString local_file_path = temp_dir.filePath("upload.txt");
    CreateFile(local_file_path);

    OpenLocalDirectory(dialog, local_directory);

    auto* local_view = FindWidget<QTreeView>(dialog, "file_transfer_local_view");
    auto* local_path = FindWidget<QLineEdit>(dialog, "file_transfer_local_path");
    EXPECT_NE(local_view, nullptr);
    EXPECT_NE(local_path, nullptr);
    if (!local_view || !local_path) {
        return {};
    }

    auto* local_model = qobject_cast<QFileSystemModel*>(local_view->model());
    EXPECT_NE(local_model, nullptr);
    if (!local_model) {
        return {};
    }

    EXPECT_EQ(local_path->text(), local_directory);

    const QModelIndex file_index = local_model->index(local_file_path);
    EXPECT_TRUE(file_index.isValid());
    EXPECT_NE(local_view->selectionModel(), nullptr);
    if (!file_index.isValid() || !local_view->selectionModel()) {
        return {};
    }

    local_view->selectionModel()->select(file_index,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    local_view->selectionModel()->setCurrentIndex(file_index,
        QItemSelectionModel::NoUpdate);
    QCoreApplication::processEvents();

    return local_file_path;
}

QString ConfigureRemoteSelection(remotex::viewer::FileTransferDialog& dialog,
                                 const QString& remote_directory,
                                 const QString& remote_file_path) {
    ApplyListing(dialog, MakeListing(remote_directory,
                                     false,
                                     {{
                                         .name = QStringLiteral("remote.txt"),
                                         .absolute_path = remote_file_path,
                                         .is_directory = false,
                                         .size_bytes = 42,
                                         .modified_unix_sec = 1710000000,
                                     }}));

    auto* remote_table = FindWidget<QTableWidget>(dialog, "file_transfer_remote_table");
    if (!remote_table) {
        return {};
    }

    EXPECT_EQ(remote_table->rowCount(), 1);
    if (remote_table->rowCount() != 1) {
        return {};
    }

    remote_table->selectRow(0);
    QCoreApplication::processEvents();

    return remote_file_path;
}

TEST_F(FileTransferDialogTest, RendersCenteredTransferControlsAndInitialRootsState) {
    int request_remote_roots_calls = 0;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = [&request_remote_roots_calls]() {
        ++request_remote_roots_calls;
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    auto* local_panel = FindWidget<QWidget>(dialog, "file_transfer_local_panel");
    auto* action_column = FindWidget<QWidget>(dialog, "file_transfer_action_column");
    auto* remote_panel = FindWidget<QWidget>(dialog, "file_transfer_remote_panel");
    auto* local_path = FindWidget<QLineEdit>(dialog, "file_transfer_local_path");
    auto* remote_path = FindWidget<QLineEdit>(dialog, "file_transfer_remote_path");
    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");

    ASSERT_NE(local_panel, nullptr);
    ASSERT_NE(action_column, nullptr);
    ASSERT_NE(remote_panel, nullptr);
    ASSERT_NE(local_path, nullptr);
    ASSERT_NE(remote_path, nullptr);
    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(send_tree_button, nullptr);
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(receive_tree_button, nullptr);

    EXPECT_LT(local_panel->geometry().center().x(), action_column->geometry().center().x());
    EXPECT_LT(action_column->geometry().center().x(), remote_panel->geometry().center().x());
    EXPECT_EQ(send_button->parentWidget(), action_column);
    EXPECT_EQ(send_tree_button->parentWidget(), action_column);
    EXPECT_EQ(receive_button->parentWidget(), action_column);
    EXPECT_EQ(receive_tree_button->parentWidget(), action_column);

    EXPECT_FALSE(local_path->text().isEmpty());
    EXPECT_EQ(remote_path->text(), QStringLiteral("Raizes"));
    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(send_tree_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());
    EXPECT_FALSE(receive_tree_button->isEnabled());
    EXPECT_EQ(request_remote_roots_calls, 1);
}

TEST_F(FileTransferDialogTest, KeepsTransferActionsDisabledWhenDispatchHooksAreUnbound) {
    remotex::viewer::FileTransferDialog dialog(nullptr, remotex::viewer::FileTransferDialog::DispatchHooks{}, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString local_file_path = ConfigureLocalSelection(dialog, temp_dir);
    const QString remote_file_path = ConfigureRemoteSelection(dialog,
                                                             QStringLiteral("D:/Target"),
                                                             QStringLiteral("D:/Target/remote.txt"));
    ASSERT_FALSE(local_file_path.isEmpty());
    ASSERT_FALSE(remote_file_path.isEmpty());

    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");
    auto* remote_refresh = FindWidget<QToolButton>(dialog, "file_transfer_remote_refresh");
    auto* remote_up = FindWidget<QToolButton>(dialog, "file_transfer_remote_up");

    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(send_tree_button, nullptr);
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(receive_tree_button, nullptr);
    ASSERT_NE(remote_refresh, nullptr);
    ASSERT_NE(remote_up, nullptr);

    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(send_tree_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());
    EXPECT_FALSE(receive_tree_button->isEnabled());
    EXPECT_FALSE(remote_refresh->isEnabled());
    EXPECT_FALSE(remote_up->isEnabled());
}

TEST_F(FileTransferDialogTest, KeepsTransferActionsDisabledWhenConflictResolutionHookIsUnbound) {
    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [](const QStringList&, const QString&) {};
    hooks.receive_paths = [](const QStringList&, const QString&) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    ASSERT_FALSE(ConfigureLocalSelection(dialog, temp_dir).isEmpty());
    ASSERT_FALSE(ConfigureRemoteSelection(dialog,
                                         QStringLiteral("D:/Target"),
                                         QStringLiteral("D:/Target/remote.txt")).isEmpty());

    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");

    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(send_tree_button, nullptr);
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(receive_tree_button, nullptr);

    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(send_tree_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());
    EXPECT_FALSE(receive_tree_button->isEnabled());
}

TEST_F(FileTransferDialogTest, ConflictPromptSummarizesPendingSendAndRestoresActionsAfterDecline) {
    QStringList sent_local_paths;
    QString sent_remote_destination;
    QList<QPair<QString, bool>> resolved_previews;
    int prompt_calls = 0;
    QString prompt_title;
    QString prompt_message;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [&](const QStringList& local_paths, const QString& remote_destination_root) {
        sent_local_paths = local_paths;
        sent_remote_destination = remote_destination_root;
    };
    hooks.receive_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [&](const QString& job_id, bool allow_overwrite) {
        resolved_previews.push_back({job_id, allow_overwrite});
    };
    hooks.prompt_conflict_decision = [&](const QString& title, const QString& message) -> std::optional<bool> {
        ++prompt_calls;
        prompt_title = title;
        prompt_message = message;
        return false;
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString local_file_path = ConfigureLocalSelection(dialog, temp_dir);
    const QString remote_file_path = ConfigureRemoteSelection(dialog,
                                                             QStringLiteral("D:/Target"),
                                                             QStringLiteral("D:/Target/remote.txt"));
    ASSERT_FALSE(local_file_path.isEmpty());
    ASSERT_FALSE(remote_file_path.isEmpty());

    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* progress_label = FindWidget<QLabel>(dialog, "file_transfer_progress_label");

    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(progress_label, nullptr);

    send_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(sent_local_paths, QStringList{local_file_path});
    EXPECT_EQ(sent_remote_destination, QStringLiteral("D:/Target"));
    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());
    EXPECT_TRUE(progress_label->text().contains(QStringLiteral("Verificando conflitos")));

    const QString conflict_summary = QStringLiteral(
        "O envio encontrou conflitos em 2 de 4 itens (3.0 KB total).\n"
        "Confirme a sobrescrita apenas se estes caminhos estiverem corretos:\n\n"
        "- docs/report.txt\n"
        "- images/logo.png");
    ApplyConflictPreview(dialog, QStringLiteral("job-send-conflict"), false, conflict_summary);

    ASSERT_EQ(prompt_calls, 1);
    EXPECT_EQ(prompt_title, QStringLiteral("Confirmar sobrescrita no envio"));
    EXPECT_EQ(prompt_message, conflict_summary);
    EXPECT_FALSE(prompt_message.contains(QStringLiteral("D:/Target")));
    EXPECT_FALSE(prompt_message.contains(QDir::cleanPath(temp_dir.path())));
    ASSERT_EQ(resolved_previews.size(), 1);
    EXPECT_EQ(resolved_previews.front().first, QStringLiteral("job-send-conflict"));
    EXPECT_FALSE(resolved_previews.front().second);
    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-send-conflict"),
                         QStringLiteral("preview-declined"),
                         QStringLiteral("Envio cancelado: sobrescrita nao aprovada."));

    EXPECT_TRUE(send_button->isEnabled());
    EXPECT_TRUE(receive_button->isEnabled());
    EXPECT_EQ(progress_label->text(), QStringLiteral("Envio cancelado: sobrescrita nao aprovada."));
}

TEST_F(FileTransferDialogTest, MissingConflictSummaryFailsClosedWithoutApprovingOverwrite) {
    QList<QPair<QString, bool>> resolved_previews;
    int prompt_calls = 0;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [](const QStringList&, const QString&) {};
    hooks.receive_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [&](const QString& job_id, bool allow_overwrite) {
        resolved_previews.push_back({job_id, allow_overwrite});
    };
    hooks.prompt_conflict_decision = [&](const QString&, const QString&) -> std::optional<bool> {
        ++prompt_calls;
        return true;
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    ASSERT_FALSE(ConfigureLocalSelection(dialog, temp_dir).isEmpty());
    ASSERT_FALSE(ConfigureRemoteSelection(dialog,
                                         QStringLiteral("D:/Target"),
                                         QStringLiteral("D:/Target/remote.txt")).isEmpty());

    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* progress_label = FindWidget<QLabel>(dialog, "file_transfer_progress_label");
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(progress_label, nullptr);

    receive_button->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictPreview(dialog, QStringLiteral("job-missing-summary"), true, QString());

    EXPECT_EQ(prompt_calls, 0);
    ASSERT_EQ(resolved_previews.size(), 1);
    EXPECT_EQ(resolved_previews.front().first, QStringLiteral("job-missing-summary"));
    EXPECT_FALSE(resolved_previews.front().second);
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-missing-summary"),
                         QStringLiteral("preview-malformed"),
                         QStringLiteral("Recebimento cancelado: o resumo de conflitos retornou dados invalidos."));

    EXPECT_TRUE(receive_button->isEnabled());
    EXPECT_EQ(progress_label->text(),
              QStringLiteral("Recebimento cancelado: o resumo de conflitos retornou dados invalidos."));
}

TEST_F(FileTransferDialogTest, ApprovedConflictWaitsForTransferStartAndMarksPostApprovalFailures) {
    QList<QPair<QString, bool>> resolved_previews;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [](const QStringList&, const QString&) {};
    hooks.receive_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [&](const QString& job_id, bool allow_overwrite) {
        resolved_previews.push_back({job_id, allow_overwrite});
    };
    hooks.prompt_conflict_decision = [](const QString&, const QString&) -> std::optional<bool> {
        return true;
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    ASSERT_FALSE(ConfigureLocalSelection(dialog, temp_dir).isEmpty());
    ASSERT_FALSE(ConfigureRemoteSelection(dialog,
                                         QStringLiteral("D:/Target"),
                                         QStringLiteral("D:/Target/remote.txt")).isEmpty());

    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* progress_label = FindWidget<QLabel>(dialog, "file_transfer_progress_label");
    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(progress_label, nullptr);

    send_button->click();
    QCoreApplication::processEvents();
    ApplyConflictPreview(dialog,
                         QStringLiteral("job-approved"),
                         false,
                         QStringLiteral("- docs/report.txt"));

    ASSERT_EQ(resolved_previews.size(), 1);
    EXPECT_EQ(resolved_previews.front().first, QStringLiteral("job-approved"));
    EXPECT_TRUE(resolved_previews.front().second);
    EXPECT_FALSE(send_button->isEnabled());

    ApplyTransferStarted(dialog,
                         QStringLiteral("job-approved"),
                         QStringLiteral("D:/Target"),
                         4096,
                         2,
                         false);

    EXPECT_TRUE(send_button->isEnabled());
    EXPECT_TRUE(progress_label->text().contains(QStringLiteral("sobrescrita aprovada")));

    ApplyTransferFinished(dialog,
                          QStringLiteral("job-approved"),
                          false,
                          QStringLiteral("D:/Target"),
                          QStringLiteral("late-conflict: file already exists"));

    EXPECT_TRUE(progress_label->text().contains(QStringLiteral("Sobrescrita aprovada, mas houve conflito tardio")));
}

TEST_F(FileTransferDialogTest, PreviewTimeoutAndUnsupportedFailuresRestoreActionStateWithExplicitText) {
    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [](const QStringList&, const QString&) {};
    hooks.receive_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    ASSERT_FALSE(ConfigureLocalSelection(dialog, temp_dir).isEmpty());
    ASSERT_FALSE(ConfigureRemoteSelection(dialog,
                                         QStringLiteral("D:/Target"),
                                         QStringLiteral("D:/Target/remote.txt")).isEmpty());

    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");
    auto* progress_label = FindWidget<QLabel>(dialog, "file_transfer_progress_label");
    ASSERT_NE(receive_button, nullptr);
    ASSERT_NE(progress_label, nullptr);

    receive_button->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-timeout"),
                         QStringLiteral("preview-timeout"),
                         QStringLiteral("Recebimento cancelado: o host nao respondeu ao resumo de conflitos."));

    EXPECT_TRUE(receive_button->isEnabled());
    EXPECT_EQ(progress_label->text(),
              QStringLiteral("Recebimento cancelado: o host nao respondeu ao resumo de conflitos."));

    receive_button->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-unsupported"),
                         QStringLiteral("preview-unsupported"),
                         QStringLiteral("Recebimento cancelado: o host nao conseguiu validar conflitos antes da transferencia."));

    EXPECT_TRUE(receive_button->isEnabled());
    EXPECT_EQ(progress_label->text(),
              QStringLiteral("Recebimento cancelado: o host nao conseguiu validar conflitos antes da transferencia."));
}

TEST_F(FileTransferDialogTest, EnablesAndDispatchesSingleArrowTransfersThroughInjectedHooks) {
    int request_remote_roots_calls = 0;
    QStringList sent_local_paths;
    QString sent_remote_destination;
    QStringList received_remote_paths;
    QString received_local_destination;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = [&request_remote_roots_calls]() {
        ++request_remote_roots_calls;
    };
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [&](const QStringList& local_paths, const QString& remote_destination_root) {
        sent_local_paths = local_paths;
        sent_remote_destination = remote_destination_root;
    };
    hooks.receive_paths = [&](const QStringList& remote_paths, const QString& local_destination_root) {
        received_remote_paths = remote_paths;
        received_local_destination = local_destination_root;
    };
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    const QString local_file_path = ConfigureLocalSelection(dialog, temp_dir);
    const QString remote_file_path = ConfigureRemoteSelection(dialog,
                                                             QStringLiteral("D:/Target"),
                                                             QStringLiteral("D:/Target/remote.txt"));
    ASSERT_FALSE(local_file_path.isEmpty());
    ASSERT_FALSE(remote_file_path.isEmpty());

    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");

    ASSERT_NE(send_button, nullptr);
    ASSERT_NE(receive_button, nullptr);
    EXPECT_TRUE(send_button->isEnabled());
    EXPECT_TRUE(receive_button->isEnabled());
    EXPECT_EQ(request_remote_roots_calls, 1);

    send_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(sent_local_paths, QStringList{local_file_path});
    EXPECT_EQ(sent_remote_destination, QStringLiteral("D:/Target"));
    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-single-send"),
                         QStringLiteral("preview-timeout"),
                         QStringLiteral("Envio cancelado: o host nao respondeu ao resumo de conflitos."));

    EXPECT_TRUE(send_button->isEnabled());
    EXPECT_TRUE(receive_button->isEnabled());

    receive_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(received_remote_paths, QStringList{remote_file_path});
    EXPECT_EQ(received_local_destination, QDir::cleanPath(temp_dir.path()));
}

TEST_F(FileTransferDialogTest, BulkCurrentTreeButtonsDispatchNormalDirectoriesThroughInjectedHooks) {
    QStringList sent_local_paths;
    QString sent_remote_destination;
    QStringList received_remote_paths;
    QString received_local_destination;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [&](const QStringList& local_paths, const QString& remote_destination_root) {
        sent_local_paths = local_paths;
        sent_remote_destination = remote_destination_root;
    };
    hooks.receive_paths = [&](const QStringList& remote_paths, const QString& local_destination_root) {
        received_remote_paths = remote_paths;
        received_local_destination = local_destination_root;
    };
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    const QString local_tree_directory = temp_dir.filePath("tree");
    const QString nested_directory = QDir(local_tree_directory).filePath(QStringLiteral("nested"));
    ASSERT_TRUE(QDir().mkpath(nested_directory));
    CreateFile(QDir(nested_directory).filePath(QStringLiteral("upload.txt")));

    OpenLocalDirectory(dialog, local_tree_directory);
    ApplyListing(dialog, MakeListing(QStringLiteral("D:/RemoteTree"),
                                     false,
                                     {{
                                         .name = QStringLiteral("subdir"),
                                         .absolute_path = QStringLiteral("D:/RemoteTree/subdir"),
                                         .is_directory = true,
                                     }}));

    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");

    ASSERT_NE(send_tree_button, nullptr);
    ASSERT_NE(receive_tree_button, nullptr);
    EXPECT_TRUE(send_tree_button->isEnabled());
    EXPECT_TRUE(receive_tree_button->isEnabled());

    send_tree_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(sent_local_paths, QStringList{QDir::cleanPath(local_tree_directory)});
    EXPECT_EQ(sent_remote_destination, QStringLiteral("D:/RemoteTree"));
    EXPECT_FALSE(send_tree_button->isEnabled());
    EXPECT_FALSE(receive_tree_button->isEnabled());

    ApplyConflictFailure(dialog,
                         QStringLiteral("job-tree-send"),
                         QStringLiteral("preview-unsupported"),
                         QStringLiteral("Envio cancelado: o host nao conseguiu validar conflitos antes da transferencia."));

    EXPECT_TRUE(send_tree_button->isEnabled());
    EXPECT_TRUE(receive_tree_button->isEnabled());

    receive_tree_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(received_remote_paths, QStringList{QStringLiteral("D:/RemoteTree")});
    EXPECT_EQ(received_local_destination, QDir::cleanPath(local_tree_directory));
}

TEST_F(FileTransferDialogTest, BulkCurrentTreeButtonsStayDisabledAtRootsAndOnEmptyOrMalformedListings) {
    QStringList sent_local_paths;
    QStringList received_remote_paths;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [&](const QStringList& local_paths, const QString&) {
        sent_local_paths = local_paths;
    };
    hooks.receive_paths = [&](const QStringList& remote_paths, const QString&) {
        received_remote_paths = remote_paths;
    };
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    const QString drive_root = VolumeRootForPath(temp_dir.path());
    const QString non_empty_local_directory = temp_dir.filePath("local-tree");
    ASSERT_TRUE(QDir().mkpath(non_empty_local_directory));
    CreateFile(QDir(non_empty_local_directory).filePath(QStringLiteral("keep.txt")));

    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");

    ASSERT_NE(send_tree_button, nullptr);
    ASSERT_NE(receive_tree_button, nullptr);

    OpenLocalDirectory(dialog, drive_root);
    ApplyListing(dialog, MakeListing(QStringLiteral("D:/Target"),
                                     false,
                                     {{
                                         .name = QStringLiteral("remote.txt"),
                                         .absolute_path = QStringLiteral("D:/Target/remote.txt"),
                                         .is_directory = false,
                                     }}));
    InvokeSlot(dialog, "NavigateLocalUp");

    EXPECT_FALSE(send_tree_button->isEnabled());
    send_tree_button->click();
    EXPECT_TRUE(sent_local_paths.isEmpty());

    OpenLocalDirectory(dialog, non_empty_local_directory);
    ApplyListing(dialog, MakeListing(QString{},
                                     true,
                                     {{
                                         .name = QStringLiteral("Drive D"),
                                         .absolute_path = QStringLiteral("D:/"),
                                         .is_directory = true,
                                     }}));

    EXPECT_FALSE(receive_tree_button->isEnabled());
    receive_tree_button->click();
    EXPECT_TRUE(received_remote_paths.isEmpty());

    const QString empty_local_directory = temp_dir.filePath("empty-local");
    ASSERT_TRUE(QDir().mkpath(empty_local_directory));
    OpenLocalDirectory(dialog, empty_local_directory);
    ApplyListing(dialog, MakeListing(QStringLiteral("D:/Target"),
                                     false,
                                     {{
                                         .name = QStringLiteral("remote.txt"),
                                         .absolute_path = QStringLiteral("D:/Target/remote.txt"),
                                         .is_directory = false,
                                     }}));

    EXPECT_FALSE(send_tree_button->isEnabled());
    send_tree_button->click();
    EXPECT_TRUE(sent_local_paths.isEmpty());

    OpenLocalDirectory(dialog, non_empty_local_directory);
    ApplyListing(dialog, MakeListing(QStringLiteral("D:/EmptyDir"), false, {}));
    EXPECT_FALSE(receive_tree_button->isEnabled());

    ApplyListing(dialog, MakeListing(QStringLiteral("D:/Malformed"),
                                     false,
                                     {
                                         MakeRemoteItem(QStringLiteral("good.txt"), QStringLiteral("D:/Malformed/good.txt"), false, 12),
                                         MakeRemoteItem(QStringLiteral("broken"), QString{}, true)
                                     }));
    EXPECT_FALSE(receive_tree_button->isEnabled());
    receive_tree_button->click();
    EXPECT_TRUE(received_remote_paths.isEmpty());
}

TEST_F(FileTransferDialogTest, BulkReceiveAtDriveRootDispatchesChildrenInsteadOfRawDrivePath) {
    QStringList received_remote_paths;
    QString received_local_destination;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.receive_paths = [&](const QStringList& remote_paths, const QString& local_destination_root) {
        received_remote_paths = remote_paths;
        received_local_destination = local_destination_root;
    };
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    OpenLocalDirectory(dialog, temp_dir.path());

    const QString remote_drive_root = QStringLiteral("D:/");
    const QString remote_docs_path = QDir(remote_drive_root).filePath(QStringLiteral("Docs"));
    const QString remote_file_path = QDir(remote_drive_root).filePath(QStringLiteral("readme.txt"));
    ApplyListing(dialog, MakeListing(remote_drive_root,
                                     false,
                                     {
                                         MakeRemoteItem(QStringLiteral("Docs"), remote_docs_path, true),
                                         MakeRemoteItem(QStringLiteral("readme.txt"), remote_file_path, false, 64)
                                     }));

    auto* receive_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_tree");
    ASSERT_NE(receive_tree_button, nullptr);
    EXPECT_TRUE(receive_tree_button->isEnabled());

    receive_tree_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(received_remote_paths,
              (QStringList{QDir::cleanPath(remote_docs_path), QDir::cleanPath(remote_file_path)}));
    EXPECT_FALSE(received_remote_paths.contains(QDir::cleanPath(remote_drive_root)));
    EXPECT_EQ(received_local_destination, QDir::cleanPath(temp_dir.path()));
}

TEST_F(FileTransferDialogTest, BulkSendAtDriveRootDispatchesVisibleChildrenInsteadOfRawDrivePath) {
    QStringList sent_local_paths;
    QString sent_remote_destination;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.send_paths = [&](const QStringList& local_paths, const QString& remote_destination_root) {
        sent_local_paths = local_paths;
        sent_remote_destination = remote_destination_root;
    };
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    ApplyListing(dialog, MakeListing(QStringLiteral("D:/Target"),
                                     false,
                                     {{
                                         .name = QStringLiteral("remote.txt"),
                                         .absolute_path = QStringLiteral("D:/Target/remote.txt"),
                                         .is_directory = false,
                                     }}));

    const QString drive_root = VolumeRootForPath(temp_dir.path());
    OpenLocalDirectory(dialog, drive_root);
    const QStringList visible_paths = VisibleLocalPaths(dialog);
    ASSERT_FALSE(visible_paths.isEmpty());

    auto* send_tree_button = FindWidget<QPushButton>(dialog, "file_transfer_send_tree");
    ASSERT_NE(send_tree_button, nullptr);
    EXPECT_TRUE(send_tree_button->isEnabled());

    send_tree_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(sent_local_paths, visible_paths);
    EXPECT_FALSE(sent_local_paths.contains(QDir::cleanPath(drive_root)));
    EXPECT_EQ(sent_remote_destination, QStringLiteral("D:/Target"));
}

TEST_F(FileTransferDialogTest, RemoteSortTogglesUseTypedValuesAndPreserveRowMetadata) {
    QStringList requested_remote_paths;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [&requested_remote_paths](const QString& path) {
        requested_remote_paths.push_back(path);
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    auto* remote_table = FindWidget<QTableWidget>(dialog, "file_transfer_remote_table");
    ASSERT_NE(remote_table, nullptr);
    ASSERT_TRUE(remote_table->isSortingEnabled());

    const QString remote_directory = QStringLiteral("D:/Sorts");
    const QString alpha_directory_path = QDir(remote_directory).filePath(QStringLiteral("Alpha"));
    const QString legacy_file_path = QDir(remote_directory).filePath(QStringLiteral("legacy.bin"));
    const QString mid_file_path = QDir(remote_directory).filePath(QStringLiteral("mid.bin"));
    const QString modern_file_path = QDir(remote_directory).filePath(QStringLiteral("modern.bin"));
    ApplyListing(dialog, MakeListing(remote_directory,
                                     false,
                                     {
                                         MakeRemoteItem(QStringLiteral("legacy.bin"),
                                                        legacy_file_path,
                                                        false,
                                                        12288,
                                                        TimestampUtc(2024, 2, 15, 8, 0)),
                                         MakeRemoteItem(QStringLiteral("Alpha"),
                                                        alpha_directory_path,
                                                        true,
                                                        0,
                                                        TimestampUtc(2024, 4, 1, 9, 0)),
                                         MakeRemoteItem(QStringLiteral("modern.bin"),
                                                        modern_file_path,
                                                        false,
                                                        1024,
                                                        TimestampUtc(2025, 1, 1, 7, 30)),
                                         MakeRemoteItem(QStringLiteral("mid.bin"),
                                                        mid_file_path,
                                                        false,
                                                        950,
                                                        TimestampUtc(2024, 3, 2, 11, 45))
                                     }));

    SortRemoteTable(remote_table, 2, Qt::AscendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                         {QStringLiteral("mid.bin"), mid_file_path, false},
                         {QStringLiteral("modern.bin"), modern_file_path, false},
                         {QStringLiteral("legacy.bin"), legacy_file_path, false},
                     });

    SortRemoteTable(remote_table, 2, Qt::DescendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("legacy.bin"), legacy_file_path, false},
                         {QStringLiteral("modern.bin"), modern_file_path, false},
                         {QStringLiteral("mid.bin"), mid_file_path, false},
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                     });

    SortRemoteTable(remote_table, 3, Qt::AscendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("legacy.bin"), legacy_file_path, false},
                         {QStringLiteral("mid.bin"), mid_file_path, false},
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                         {QStringLiteral("modern.bin"), modern_file_path, false},
                     });

    SortRemoteTable(remote_table, 3, Qt::DescendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("modern.bin"), modern_file_path, false},
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                         {QStringLiteral("mid.bin"), mid_file_path, false},
                         {QStringLiteral("legacy.bin"), legacy_file_path, false},
                     });

    SortRemoteTable(remote_table, 3, Qt::AscendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("legacy.bin"), legacy_file_path, false},
                         {QStringLiteral("mid.bin"), mid_file_path, false},
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                         {QStringLiteral("modern.bin"), modern_file_path, false},
                     });

    ActivateRemoteRow(dialog, 2);
    EXPECT_EQ(requested_remote_paths, QStringList{QDir::cleanPath(alpha_directory_path)});
}

TEST_F(FileTransferDialogTest, RepopulatingSortedRemoteListingsPreservesSortStateAndNavigationMetadata) {
    QStringList requested_remote_paths;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [&requested_remote_paths](const QString& path) {
        requested_remote_paths.push_back(path);
    };

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    auto* remote_table = FindWidget<QTableWidget>(dialog, "file_transfer_remote_table");
    auto* remote_path = FindWidget<QLineEdit>(dialog, "file_transfer_remote_path");
    ASSERT_NE(remote_table, nullptr);
    ASSERT_NE(remote_path, nullptr);

    const QString remote_directory = QStringLiteral("D:/Sorted");
    const QString alpha_directory_path = QDir(remote_directory).filePath(QStringLiteral("Alpha"));
    const QString small_file_path = QDir(remote_directory).filePath(QStringLiteral("small.bin"));
    const QString medium_file_path = QDir(remote_directory).filePath(QStringLiteral("medium.bin"));
    ApplyListing(dialog, MakeListing(remote_directory,
                                     false,
                                     {
                                         MakeRemoteItem(QStringLiteral("medium.bin"), medium_file_path, false, 4096, TimestampUtc(2024, 2, 15, 8, 0)),
                                         MakeRemoteItem(QStringLiteral("Alpha"), alpha_directory_path, true, 0, TimestampUtc(2024, 4, 1, 9, 0)),
                                         MakeRemoteItem(QStringLiteral("small.bin"), small_file_path, false, 5, TimestampUtc(2024, 1, 10, 7, 30))
                                     }));

    SortRemoteTable(remote_table, 2, Qt::DescendingOrder);
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("medium.bin"), medium_file_path, false},
                         {QStringLiteral("small.bin"), small_file_path, false},
                         {QStringLiteral("Alpha"), alpha_directory_path, true},
                     });

    const QString beta_directory_path = QDir(remote_directory).filePath(QStringLiteral("Beta"));
    const QString newer_small_path = QDir(remote_directory).filePath(QStringLiteral("newer-small.bin"));
    const QString huge_file_path = QDir(remote_directory).filePath(QStringLiteral("huge.bin"));
    ApplyListing(dialog, MakeListing(remote_directory,
                                     false,
                                     {
                                         MakeRemoteItem(QStringLiteral("newer-small.bin"), newer_small_path, false, 15, TimestampUtc(2024, 6, 3, 7, 30)),
                                         MakeRemoteItem(QStringLiteral("Beta"), beta_directory_path, true, 0, TimestampUtc(2024, 6, 4, 9, 0)),
                                         MakeRemoteItem(QStringLiteral("huge.bin"), huge_file_path, false, 65536, TimestampUtc(2024, 6, 5, 8, 15))
                                     }));

    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    ExpectRemoteRows(remote_table,
                     {
                         {QStringLiteral("huge.bin"), huge_file_path, false},
                         {QStringLiteral("newer-small.bin"), newer_small_path, false},
                         {QStringLiteral("Beta"), beta_directory_path, true},
                     });
    EXPECT_EQ(remote_path->text(), remote_directory);

    ActivateRemoteRow(dialog, 2);
    EXPECT_EQ(requested_remote_paths, QStringList{QDir::cleanPath(beta_directory_path)});

    ApplyListing(dialog, MakeListing(remote_directory, false, {}));
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(remote_table->rowCount(), 0);
    EXPECT_EQ(remote_path->text(), remote_directory);

    ApplyListing(dialog, MakeListing(QString{},
                                     true,
                                     {
                                         MakeRemoteItem(QStringLiteral("Drive D"), QStringLiteral("D:/"), true),
                                         MakeRemoteItem(QStringLiteral("Drive C"), QStringLiteral("C:/"), true)
                                     }));
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(remote_table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(remote_table->rowCount(), 2);
    EXPECT_EQ(remote_path->text(), QStringLiteral("Raizes"));
    for (int row = 0; row < remote_table->rowCount(); ++row) {
        auto* name_item = remote_table->item(row, 0);
        ASSERT_NE(name_item, nullptr);
        EXPECT_FALSE(name_item->data(kRoleAbsolutePath).toString().isEmpty());
        EXPECT_TRUE(name_item->data(kRoleIsDirectory).toBool());
    }
}

TEST_F(FileTransferDialogTest, LocalPaneReturnsFromDriveRootToRootsAndRefreshPreservesRootsState) {
    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = []() {};
    hooks.request_remote_listing = [](const QString&) {};
    hooks.receive_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString remote_file_path = ConfigureRemoteSelection(dialog,
                                                             QStringLiteral("D:/Target"),
                                                             QStringLiteral("D:/Target/remote.txt"));
    ASSERT_FALSE(remote_file_path.isEmpty());

    const QString drive_root = VolumeRootForPath(temp_dir.path());
    OpenLocalDirectory(dialog, drive_root);

    auto* local_path = FindWidget<QLineEdit>(dialog, "file_transfer_local_path");
    auto* local_up = FindWidget<QToolButton>(dialog, "file_transfer_local_up");
    auto* local_refresh = FindWidget<QToolButton>(dialog, "file_transfer_local_refresh");
    auto* receive_button = FindWidget<QPushButton>(dialog, "file_transfer_receive_selection");

    ASSERT_NE(local_path, nullptr);
    ASSERT_NE(local_up, nullptr);
    ASSERT_NE(local_refresh, nullptr);
    ASSERT_NE(receive_button, nullptr);

    EXPECT_EQ(local_path->text(), drive_root);
    EXPECT_TRUE(local_up->isEnabled());
    EXPECT_TRUE(local_refresh->isEnabled());
    EXPECT_TRUE(receive_button->isEnabled());

    InvokeSlot(dialog, "NavigateLocalUp");

    EXPECT_EQ(local_path->text(), QStringLiteral("Raizes"));
    EXPECT_FALSE(local_up->isEnabled());
    EXPECT_TRUE(local_refresh->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());

    InvokeSlot(dialog, "RefreshLocal");

    EXPECT_EQ(local_path->text(), QStringLiteral("Raizes"));
    EXPECT_FALSE(local_up->isEnabled());
    EXPECT_TRUE(local_refresh->isEnabled());
    EXPECT_FALSE(receive_button->isEnabled());

    OpenLocalDirectory(dialog, drive_root);
    EXPECT_EQ(local_path->text(), drive_root);
    EXPECT_TRUE(local_up->isEnabled());
}

TEST_F(FileTransferDialogTest, RemotePaneReturnsFromDriveRootToRootsAndTreatsEmptyPathListingAsRoots) {
    int request_remote_roots_calls = 0;
    QStringList requested_remote_paths;

    remotex::viewer::FileTransferDialog::DispatchHooks hooks;
    hooks.request_remote_roots = [&request_remote_roots_calls]() {
        ++request_remote_roots_calls;
    };
    hooks.request_remote_listing = [&requested_remote_paths](const QString& path) {
        requested_remote_paths.push_back(path);
    };
    hooks.send_paths = [](const QStringList&, const QString&) {};
    hooks.resolve_conflict = [](const QString&, bool) {};

    remotex::viewer::FileTransferDialog dialog(nullptr, hooks, nullptr);
    dialog.show();
    QCoreApplication::processEvents();

    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString local_file_path = ConfigureLocalSelection(dialog, temp_dir);
    ASSERT_FALSE(local_file_path.isEmpty());

    const QString remote_drive_root = QStringLiteral("D:/");
    const QString normalized_remote_drive_root = QDir::cleanPath(remote_drive_root);
    const QString remote_file_path = QDir(remote_drive_root).filePath(QStringLiteral("remote.txt"));
    ConfigureRemoteSelection(dialog, remote_drive_root, remote_file_path);

    auto* remote_path = FindWidget<QLineEdit>(dialog, "file_transfer_remote_path");
    auto* remote_up = FindWidget<QToolButton>(dialog, "file_transfer_remote_up");
    auto* remote_refresh = FindWidget<QToolButton>(dialog, "file_transfer_remote_refresh");
    auto* send_button = FindWidget<QPushButton>(dialog, "file_transfer_send_selection");

    ASSERT_NE(remote_path, nullptr);
    ASSERT_NE(remote_up, nullptr);
    ASSERT_NE(remote_refresh, nullptr);
    ASSERT_NE(send_button, nullptr);

    EXPECT_EQ(remote_path->text(), normalized_remote_drive_root);
    EXPECT_TRUE(remote_up->isEnabled());
    EXPECT_TRUE(remote_refresh->isEnabled());
    EXPECT_TRUE(send_button->isEnabled());
    EXPECT_EQ(request_remote_roots_calls, 1);

    InvokeSlot(dialog, "NavigateRemoteUp");

    EXPECT_EQ(remote_path->text(), QStringLiteral("Raizes"));
    EXPECT_FALSE(remote_up->isEnabled());
    EXPECT_TRUE(remote_refresh->isEnabled());
    EXPECT_FALSE(send_button->isEnabled());
    EXPECT_EQ(request_remote_roots_calls, 2);
    EXPECT_TRUE(requested_remote_paths.isEmpty());

    InvokeSlot(dialog, "RefreshRemote");

    EXPECT_EQ(remote_path->text(), QStringLiteral("Raizes"));
    EXPECT_EQ(request_remote_roots_calls, 3);
    EXPECT_TRUE(requested_remote_paths.isEmpty());

    ApplyListing(dialog, MakeListing(QString{},
                                     false,
                                     {{
                                         .name = QStringLiteral("Drive D"),
                                         .absolute_path = QStringLiteral("D:/"),
                                         .is_directory = true,
                                     }}));

    EXPECT_EQ(remote_path->text(), QStringLiteral("Raizes"));
    EXPECT_FALSE(remote_up->isEnabled());
    EXPECT_TRUE(remote_refresh->isEnabled());
    EXPECT_FALSE(send_button->isEnabled());
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
