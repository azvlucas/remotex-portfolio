#include "viewer/reports_dialog.h"
#include "viewer/connection_settings.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QVBoxLayout>

#include <utility>

namespace remotex::viewer {

namespace {

struct ReportsLoadResult {
    quint64 request_id = 0;
    remotex::core::ReportDashboardData data;
    QString error;
};

remotex::core::MachineClient BuildDialogClient(remotex::core::AuthClient* auth);

ReportsDialog::ReportLoader BuildDefaultReportLoader(remotex::core::AuthClient* auth) {
    return [auth](const QString& df, const QString& dt) {
        auto client = BuildDialogClient(auth);
        return client.GetReportDashboard(df.toStdString(), dt.toStdString());
    };
}

remotex::core::MachineClient BuildDialogClient(remotex::core::AuthClient* auth) {
    remotex::core::MachineClient client;
    client.Configure(ConnectionSettings::Load().ToMachineClientConfig(), auth);
    return client;
}

QString VisibleDateText(QDateEdit* edit) {
    if (!edit) {
        return {};
    }

    if (auto* line_edit = edit->findChild<QLineEdit*>()) {
        return line_edit->text();
    }

    return edit->text();
}

QFrame* MakeSummaryCard(const QString& title, QLabel*& value_label, QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setStyleSheet(
        "QFrame { background: #f8f9fa; border: 1px solid #dee2e6; border-radius: 6px; padding: 12px; }"
    );

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 8, 12, 8);

    auto* title_label = new QLabel(title, frame);
    title_label->setStyleSheet("font-size: 11px; color: #6c757d; border: none; background: transparent;");
    layout->addWidget(title_label);

    value_label = new QLabel("-", frame);
    value_label->setStyleSheet("font-size: 22px; font-weight: bold; color: #212529; border: none; background: transparent;");
    layout->addWidget(value_label);

    return frame;
}

} // namespace

ReportsDialog::ReportsDialog(remotex::core::AuthClient* auth, QWidget* parent)
    : ReportsDialog(auth, BuildDefaultReportLoader(auth), true, parent) {
}

ReportsDialog::ReportsDialog(remotex::core::AuthClient* auth,
                             ReportLoader report_loader,
                             bool auto_load,
                             QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , report_loader_(std::move(report_loader)) {
    if (!report_loader_) {
        report_loader_ = BuildDefaultReportLoader(auth_);
    }

    SetupUi();
    if (auto_load) {
        LoadData();
    }
}

bool ReportsDialog::ResolveDateRange(const QString& date_from_text,
                                     const QString& date_to_text,
                                     const QString& display_format,
                                     QDate* resolved_from,
                                     QDate* resolved_to,
                                     QString* error) {
    const QDate parsed_from = QDate::fromString(date_from_text.trimmed(), display_format);
    if (!parsed_from.isValid()) {
        if (error) {
            *error = "Informe um intervalo de datas valido.";
        }
        return false;
    }

    const QDate parsed_to = QDate::fromString(date_to_text.trimmed(), display_format);
    if (!parsed_to.isValid()) {
        if (error) {
            *error = "Informe um intervalo de datas valido.";
        }
        return false;
    }

    if (parsed_from > parsed_to) {
        if (error) {
            *error = "A data inicial nao pode ser maior que a data final.";
        }
        return false;
    }

    if (resolved_from) {
        *resolved_from = parsed_from;
    }
    if (resolved_to) {
        *resolved_to = parsed_to;
    }
    if (error) {
        error->clear();
    }
    return true;
}

void ReportsDialog::SetupUi() {
    setWindowTitle("Relatorios da empresa");
    resize(900, 580);

    auto* root = new QVBoxLayout(this);

    // ── Filter bar ──────────────────────────────────────────────────
    auto* filter_bar = new QHBoxLayout;
    filter_bar->addWidget(new QLabel("De:", this));

    date_from_ = new QDateEdit(this);
    date_from_->setObjectName("reports_date_from");
    date_from_->setCalendarPopup(true);
    date_from_->setDisplayFormat("dd/MM/yyyy");
    date_from_->setDate(QDate::currentDate().addDays(-30));
    filter_bar->addWidget(date_from_);

    filter_bar->addWidget(new QLabel("Ate:", this));

    date_to_ = new QDateEdit(this);
    date_to_->setObjectName("reports_date_to");
    date_to_->setCalendarPopup(true);
    date_to_->setDisplayFormat("dd/MM/yyyy");
    date_to_->setDate(QDate::currentDate());
    filter_bar->addWidget(date_to_);

    apply_btn_ = new QPushButton("Aplicar", this);
    apply_btn_->setObjectName("reports_apply");
    connect(apply_btn_, &QPushButton::clicked, this, &ReportsDialog::OnApplyFilter);
    filter_bar->addWidget(apply_btn_);

    filter_bar->addStretch();
    root->addLayout(filter_bar);

    // ── Summary cards ───────────────────────────────────────────────
    auto* cards = new QHBoxLayout;
    cards->addWidget(MakeSummaryCard("Horas de suporte", total_hours_value_, this));
    cards->addWidget(MakeSummaryCard("Total de sessoes", total_sessions_value_, this));
    cards->addWidget(MakeSummaryCard("Duracao media", avg_duration_value_, this));
    root->addLayout(cards);

    // ── Tables ──────────────────────────────────────────────────────
    auto* tables = new QHBoxLayout;

    // Left: by machine
    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel("Sessoes por maquina", this));
    machine_table_ = new QTableWidget(0, 3, this);
    machine_table_->setHorizontalHeaderLabels({"Maquina", "Sessoes", "Tempo total"});
    machine_table_->horizontalHeader()->setStretchLastSection(true);
    machine_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    machine_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    machine_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    machine_table_->verticalHeader()->hide();
    left->addWidget(machine_table_);
    tables->addLayout(left);

    // Right: by technician
    auto* right = new QVBoxLayout;
    right->addWidget(new QLabel("Sessoes por tecnico", this));
    technician_table_ = new QTableWidget(0, 3, this);
    technician_table_->setHorizontalHeaderLabels({"Tecnico", "Sessoes", "Tempo total"});
    technician_table_->horizontalHeader()->setStretchLastSection(true);
    technician_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    technician_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    technician_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    technician_table_->verticalHeader()->hide();
    right->addWidget(technician_table_);
    tables->addLayout(right);

    root->addLayout(tables, 1);

    // ── Bottom bar ──────────────────────────────────────────────────
    auto* bottom = new QHBoxLayout;
    status_label_ = new QLabel(this);
    status_label_->setObjectName("reports_status");
    bottom->addWidget(status_label_, 1);

    close_btn_ = new QPushButton("Fechar", this);
    connect(close_btn_, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(close_btn_);
    root->addLayout(bottom);
}

void ReportsDialog::SetStatus(const QString& message, bool is_error) {
    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? "color: red;" : "");
}

void ReportsDialog::UpdateUiState() {
    apply_btn_->setEnabled(!loading_);
    date_from_->setEnabled(!loading_);
    date_to_->setEnabled(!loading_);
}

void ReportsDialog::OnApplyFilter() {
    QDate resolved_from;
    QDate resolved_to;
    QString error;
    if (!ResolveDateRange(VisibleDateText(date_from_), VisibleDateText(date_to_), date_from_->displayFormat(),
                          &resolved_from, &resolved_to, &error)) {
        SetStatus(error, true);
        return;
    }

    date_from_->setDate(resolved_from);
    date_to_->setDate(resolved_to);
    LoadData();
}

void ReportsDialog::LoadData() {
    loading_ = true;
    ++load_request_id_;
    quint64 req_id = load_request_id_;
    SetStatus("Carregando...");
    UpdateUiState();

    QString df = date_from_->date().toString("yyyy-MM-dd");
    QString dt = date_to_->date().toString("yyyy-MM-dd");

    auto* watcher = new QFutureWatcher<ReportsLoadResult>(this);
    connect(watcher, &QFutureWatcher<ReportsLoadResult>::finished, this,
            [this, watcher, req_id]() {
        watcher->deleteLater();
        if (req_id != load_request_id_) return;

        auto result = watcher->result();
        loading_ = false;

        if (!result.error.isEmpty()) {
            SetStatus(result.error, true);
            UpdateUiState();
            return;
        }

        cached_total_sessions_ = result.data.total_sessions;
        cached_total_duration_sec_ = result.data.total_duration_sec;
        cached_avg_duration_sec_ = result.data.avg_duration_sec;
        cached_by_machine_ = std::move(result.data.by_machine);
        cached_by_technician_ = std::move(result.data.by_technician);

        PopulateData();
        SetStatus(QString("%1 sessao(oes) no periodo").arg(cached_total_sessions_));
        UpdateUiState();
    });

    watcher->setFuture(QtConcurrent::run([loader = report_loader_, df, dt, req_id]() {
        ReportsLoadResult result;
        result.request_id = req_id;

        if (!loader) {
            result.error = "falha ao carregar relatorio";
            return result;
        }

        result.data = loader(df, dt);

        if (!result.data.error.empty())
            result.error = QString::fromStdString(result.data.error);

        return result;
    }));
}

void ReportsDialog::PopulateData() {
    // Summary cards
    total_hours_value_->setText(FormatHours(cached_total_duration_sec_));
    total_sessions_value_->setText(QString::number(cached_total_sessions_));
    avg_duration_value_->setText(FormatDuration(cached_avg_duration_sec_));

    // Machine table
    machine_table_->setRowCount(static_cast<int>(cached_by_machine_.size()));
    for (int i = 0; i < static_cast<int>(cached_by_machine_.size()); ++i) {
        const auto& ms = cached_by_machine_[i];
        machine_table_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(ms.machine_name)));

        auto* count_item = new QTableWidgetItem(QString::number(ms.session_count));
        count_item->setTextAlignment(Qt::AlignCenter);
        machine_table_->setItem(i, 1, count_item);

        auto* dur_item = new QTableWidgetItem(FormatDuration(ms.total_duration_sec));
        dur_item->setTextAlignment(Qt::AlignCenter);
        machine_table_->setItem(i, 2, dur_item);
    }

    // Technician table
    technician_table_->setRowCount(static_cast<int>(cached_by_technician_.size()));
    for (int i = 0; i < static_cast<int>(cached_by_technician_.size()); ++i) {
        const auto& ts = cached_by_technician_[i];
        technician_table_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(ts.user_name)));

        auto* count_item = new QTableWidgetItem(QString::number(ts.session_count));
        count_item->setTextAlignment(Qt::AlignCenter);
        technician_table_->setItem(i, 1, count_item);

        auto* dur_item = new QTableWidgetItem(FormatDuration(ts.total_duration_sec));
        dur_item->setTextAlignment(Qt::AlignCenter);
        technician_table_->setItem(i, 2, dur_item);
    }
}

QString ReportsDialog::FormatDuration(int seconds) {
    if (seconds <= 0) return "-";
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) return QString("%1h %2m").arg(h).arg(m, 2, 10, QChar('0'));
    if (m > 0) return QString("%1m %2s").arg(m).arg(s, 2, 10, QChar('0'));
    return QString("%1s").arg(s);
}

QString ReportsDialog::FormatHours(int seconds) {
    if (seconds <= 0) return "0h";
    double hours = static_cast<double>(seconds) / 3600.0;
    if (hours >= 10.0)
        return QString("%1h").arg(static_cast<int>(hours));
    return QString("%1h").arg(hours, 0, 'f', 1);
}

} // namespace remotex::viewer
