#include "viewer/recordings_dialog.h"
#include "viewer/connection_settings.h"
#include "viewer/presigned_url_utils.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QUrl>
#include <QVBoxLayout>

#include <unordered_map>

namespace remotex::viewer {

namespace {

struct RecordingsLoadResult {
    quint64 request_id = 0;
    std::vector<remotex::core::MachineInfo> machines;
    std::vector<remotex::core::RecordingInfo> recordings;
    QString error;
};

struct RecordingActionResult {
    quint64 request_id = 0;
    QString error;
    QString download_url;  // set only for download action
    bool deleted = false;
};

remotex::core::MachineClient BuildDialogClient(remotex::core::AuthClient* auth) {
    remotex::core::MachineClient client;
    client.Configure(ConnectionSettings::Load().ToMachineClientConfig(), auth);
    return client;
}

QString FormatFileSize(int64_t bytes) {
    if (bytes <= 0) return "-";
    static constexpr const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int idx = 0;
    while (value >= 1024.0 && idx < 3) { value /= 1024.0; ++idx; }
    return QString("%1 %2").arg(QString::number(value, 'f', idx == 0 ? 0 : 1), units[idx]);
}

QString FormatDuration(int seconds) {
    if (seconds <= 0) return "-";
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) return QString("%1h %2m").arg(h).arg(m, 2, 10, QChar('0'));
    if (m > 0) return QString("%1m %2s").arg(m).arg(s, 2, 10, QChar('0'));
    return QString("%1s").arg(s);
}

QString FormatTimestamp(const std::string& ts) {
    if (ts.empty()) return "-";
    QDateTime dt = QDateTime::fromString(QString::fromStdString(ts), Qt::ISODate);
    if (!dt.isValid()) return QString::fromStdString(ts);
    return dt.toLocalTime().toString("dd/MM/yyyy HH:mm");
}

} // namespace

RecordingsDialog::RecordingsDialog(remotex::core::AuthClient* auth, QWidget* parent)
    : QDialog(parent)
    , auth_(auth) {
    SetupUi();
    ReloadData();
}

void RecordingsDialog::SetupUi() {
    setWindowTitle("Gravacoes de sessao");
    resize(820, 480);

    auto* root = new QVBoxLayout(this);

    // Filter bar
    auto* filter_bar = new QHBoxLayout;
    filter_bar->addWidget(new QLabel("Maquina:"));
    machine_filter_ = new QComboBox;
    machine_filter_->addItem("Todas");
    filter_bar->addWidget(machine_filter_, 1);
    root->addLayout(filter_bar);

    // Table
    table_ = new QTableWidget;
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"Maquina", "Data", "Duracao", "Tamanho", "Status"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    root->addWidget(table_, 1);

    // Bottom bar
    auto* bottom = new QHBoxLayout;
    status_label_ = new QLabel;
    bottom->addWidget(status_label_, 1);

    download_btn_ = new QPushButton("Download");
    delete_btn_ = new QPushButton("Excluir");
    close_btn_ = new QPushButton("Fechar");

    bottom->addWidget(download_btn_);
    bottom->addWidget(delete_btn_);
    bottom->addWidget(close_btn_);
    root->addLayout(bottom);

    delete_btn_->setVisible(IsManager());

    connect(machine_filter_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &RecordingsDialog::OnMachineFilterChanged);
    connect(download_btn_, &QPushButton::clicked, this, &RecordingsDialog::OnDownload);
    connect(delete_btn_, &QPushButton::clicked, this, &RecordingsDialog::OnDelete);
    connect(close_btn_, &QPushButton::clicked, this, &QDialog::accept);

    UpdateUiState();
}

bool RecordingsDialog::IsManager() const {
    if (!auth_) return false;
    auto role = auth_->GetRole();
    return role == "company_manager" || role == "super_admin";
}

int RecordingsDialog::GetSelectedRow() const {
    auto items = table_->selectionModel()->selectedRows();
    if (items.isEmpty()) return -1;
    return items.first().row();
}

void RecordingsDialog::SetStatus(const QString& message, bool is_error) {
    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? "color: red;" : "");
}

void RecordingsDialog::UpdateUiState() {
    bool has_selection = GetSelectedRow() >= 0;
    download_btn_->setEnabled(!loading_ && !action_in_flight_ && has_selection);
    delete_btn_->setEnabled(!loading_ && !action_in_flight_ && has_selection);
    delete_btn_->setVisible(IsManager());
    machine_filter_->setEnabled(!loading_);
}

void RecordingsDialog::ReloadData() {
    loading_ = true;
    ++load_request_id_;
    quint64 req_id = load_request_id_;
    SetStatus("Carregando...");
    UpdateUiState();

    auto* auth = auth_;
    QString machine_id;
    int idx = machine_filter_->currentIndex();
    if (idx > 0 && idx - 1 < static_cast<int>(machines_.size())) {
        machine_id = QString::fromStdString(machines_[static_cast<size_t>(idx - 1)].id);
    }

    auto* watcher = new QFutureWatcher<RecordingsLoadResult>(this);
    connect(watcher, &QFutureWatcher<RecordingsLoadResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != load_request_id_) return;

        auto result = watcher->result();
        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            loading_ = false;
            UpdateUiState();
            return;
        }

        // Update machines combo only on first load or if empty
        if (machines_.empty() && !result.machines.empty()) {
            machines_ = std::move(result.machines);
            machine_filter_->blockSignals(true);
            for (const auto& m : machines_) {
                machine_filter_->addItem(QString::fromStdString(m.name));
            }
            machine_filter_->blockSignals(false);
        }

        recordings_ = std::move(result.recordings);
        PopulateTable();
        SetStatus(QString("%1 gravacao(oes)").arg(recordings_.size()));
        loading_ = false;
        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth, machine_id, req_id]() -> RecordingsLoadResult {
        RecordingsLoadResult result;
        result.request_id = req_id;

        auto client = BuildDialogClient(auth);
        result.machines = client.ListMachines();

        if (machine_id.isEmpty()) {
            result.recordings = client.ListRecordings(200, 0);
        } else {
            result.recordings = client.ListRecordingsByMachine(
                machine_id.toStdString(), 200, 0);
        }

        return result;
    }));
}

void RecordingsDialog::PopulateTable() {
    table_->setRowCount(0);
    table_->setRowCount(static_cast<int>(recordings_.size()));

    // Build machine name map
    std::unordered_map<std::string, std::string> machine_names;
    for (const auto& m : machines_) {
        machine_names[m.id] = m.name;
    }

    for (size_t i = 0; i < recordings_.size(); ++i) {
        const auto& rec = recordings_[i];
        int row = static_cast<int>(i);

        auto it = machine_names.find(rec.machine_id);
        QString machine_name = (it != machine_names.end())
            ? QString::fromStdString(it->second)
            : QString::fromStdString(rec.machine_id).left(8);

        table_->setItem(row, 0, new QTableWidgetItem(machine_name));
        table_->setItem(row, 1, new QTableWidgetItem(FormatTimestamp(rec.created_at)));
        table_->setItem(row, 2, new QTableWidgetItem(FormatDuration(rec.duration_sec)));
        table_->setItem(row, 3, new QTableWidgetItem(FormatFileSize(rec.file_size)));
        table_->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(rec.status)));
    }
}

void RecordingsDialog::OnMachineFilterChanged(int /*index*/) {
    ReloadData();
}

void RecordingsDialog::OnDownload() {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(recordings_.size())) return;

    action_in_flight_ = true;
    ++action_request_id_;
    quint64 req_id = action_request_id_;
    std::string rec_id = recordings_[static_cast<size_t>(row)].id;
    SetStatus("Obtendo link de download...");
    UpdateUiState();

    auto* auth = auth_;
    auto* watcher = new QFutureWatcher<RecordingActionResult>(this);
    connect(watcher, &QFutureWatcher<RecordingActionResult>::finished, this,
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

        if (!result.download_url.isEmpty()) {
            QString open_error;
            if (OpenPresignedExternalUrlInBrowser(result.download_url, &open_error)) {
                SetStatus("Download iniciado no navegador.");
            } else {
                SetStatus(open_error, true);
            }
        }
        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([auth, rec_id, req_id]() -> RecordingActionResult {
        RecordingActionResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        const auto download = client.GetRecordingDownloadURL(rec_id);
        if (!download.error.empty()) {
            result.error = QString::fromStdString(download.error);
            if (download.http_code > 0) {
                result.error += QString(" (HTTP %1)").arg(download.http_code);
            }
        } else if (download.url.empty()) {
            result.error = "Falha ao obter URL de download";
        } else {
            result.download_url = QString::fromStdString(download.url);
        }
        return result;
    }));
}

void RecordingsDialog::OnDelete() {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(recordings_.size())) return;

    auto reply = QMessageBox::question(this, "Excluir gravacao",
        "Tem certeza que deseja excluir esta gravacao?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    action_in_flight_ = true;
    ++action_request_id_;
    quint64 req_id = action_request_id_;
    std::string rec_id = recordings_[static_cast<size_t>(row)].id;
    SetStatus("Excluindo...");
    UpdateUiState();

    auto* auth = auth_;
    auto* watcher = new QFutureWatcher<RecordingActionResult>(this);
    connect(watcher, &QFutureWatcher<RecordingActionResult>::finished, this,
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

        SetStatus("Gravacao excluida.");
        ReloadData();
    });

    watcher->setFuture(QtConcurrent::run([auth, rec_id, req_id]() -> RecordingActionResult {
        RecordingActionResult result;
        result.request_id = req_id;
        auto client = BuildDialogClient(auth);
        auto r = client.DeleteRecording(rec_id);
        if (!r.success) {
            result.error = r.error.empty()
                ? QString("Falha ao excluir")
                : QString::fromStdString(r.error);
        } else {
            result.deleted = true;
        }
        return result;
    }));
}

} // namespace remotex::viewer
