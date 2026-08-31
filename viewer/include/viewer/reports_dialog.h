#pragma once
// RemoteX - viewer/reports_dialog.h
// Dialog de relatorios: horas de suporte, sessoes por maquina/tecnico.

#include "core/auth_client.h"
#include "core/machine_client.h"

#include <QDateEdit>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <functional>

namespace remotex::viewer {

class ReportsDialog : public QDialog {
    Q_OBJECT

public:
    using ReportLoader = std::function<remotex::core::ReportDashboardData(const QString&, const QString&)>;

    explicit ReportsDialog(remotex::core::AuthClient* auth, QWidget* parent = nullptr);
    ReportsDialog(remotex::core::AuthClient* auth,
                  ReportLoader report_loader,
                  bool auto_load,
                  QWidget* parent = nullptr);
    static bool ResolveDateRange(const QString& date_from_text,
                                 const QString& date_to_text,
                                 const QString& display_format,
                                 QDate* resolved_from,
                                 QDate* resolved_to,
                                 QString* error = nullptr);

private slots:
    void OnApplyFilter();

private:
    void SetupUi();
    void SetStatus(const QString& message, bool is_error = false);
    void UpdateUiState();
    void LoadData();
    void PopulateData();
    static QString FormatDuration(int seconds);
    static QString FormatHours(int seconds);

    remotex::core::AuthClient* auth_ = nullptr;
    ReportLoader report_loader_;
    quint64 load_request_id_ = 0;
    bool loading_ = false;

    QDateEdit*    date_from_ = nullptr;
    QDateEdit*    date_to_ = nullptr;
    QPushButton*  apply_btn_ = nullptr;

    // Summary cards
    QLabel* total_hours_value_ = nullptr;
    QLabel* total_sessions_value_ = nullptr;
    QLabel* avg_duration_value_ = nullptr;

    // Tables
    QTableWidget* machine_table_ = nullptr;
    QTableWidget* technician_table_ = nullptr;

    QPushButton* close_btn_ = nullptr;
    QLabel*      status_label_ = nullptr;

    // Cached data from last load
    int cached_total_sessions_ = 0;
    int cached_total_duration_sec_ = 0;
    int cached_avg_duration_sec_ = 0;
    std::vector<remotex::core::ReportMachineStats>    cached_by_machine_;
    std::vector<remotex::core::ReportTechnicianStats> cached_by_technician_;
};

} // namespace remotex::viewer
