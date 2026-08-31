namespace {

struct MachineRefreshResult {
    quint64 request_id = 0;
    std::string previous_selected_id;
    std::vector<remotex::core::MachineInfo> machines;
};

struct AuditLoadResult {
    quint64 request_id = 0;
    std::string machine_id;
    bool append = false;
    std::vector<remotex::core::MachineAuditEvent> events;
    std::vector<remotex::core::RecordingInfo> recordings;
};

struct RecordingDownloadResult {
    quint64 request_id = 0;
    QString download_url;
    QString error;
};

struct DeleteMachineResult {
    quint64 request_id = 0;
    std::string machine_id;
    remotex::core::MachineResult result;
};

QString DocumentTypeLabel(const std::string& document_type) {
    if (document_type == "cpf") {
        return "CPF";
    }
    if (document_type == "cnpj") {
        return "CNPJ";
    }

    return QString();
}

QString FormatDocumentText(const remotex::core::MachineInfo& machine) {
    const QString document_type = DocumentTypeLabel(machine.document_type);
    const QString document_number = QString::fromStdString(machine.document_number);

    if (document_type.isEmpty()) {
        return document_number;
    }
    if (document_number.isEmpty()) {
        return document_type;
    }

    return QString("%1: %2").arg(document_type, document_number);
}

bool MatchesMachineSearch(const remotex::core::MachineInfo& machine, const QString& search_text) {
    if (search_text.isEmpty()) {
        return true;
    }

    const QStringList searchable_fields = {
        QString::fromStdString(machine.name),
        QString::fromStdString(machine.legal_name),
        QString::fromStdString(machine.trade_name),
        FormatDocumentText(machine),
        QString::fromStdString(machine.group_name),
        QString::fromStdString(machine.subgroup_name),
    };

    for (const auto& field : searchable_fields) {
        if (field.contains(search_text, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

QString FormatDurationSeconds(int duration_sec) {
    if (duration_sec <= 0) {
        return "-";
    }

    const int hours = duration_sec / 3600;
    const int minutes = (duration_sec % 3600) / 60;
    const int seconds = duration_sec % 60;

    if (hours > 0) {
        return QString("%1h %2m").arg(hours).arg(minutes, 2, 10, QChar('0'));
    }
    if (minutes > 0) {
        return QString("%1m %2s").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }
    return QString("%1s").arg(seconds);
}

QString FormatAuditTimestamp(const std::string& timestamp) {
    if (timestamp.empty()) {
        return "-";
    }

    const QDateTime dt = ParseIsoTimestampUtc(timestamp);
    if (!dt.isValid()) {
        return QString::fromStdString(timestamp);
    }

    return dt.toLocalTime().toString("dd/MM/yyyy HH:mm:ss");
}

QString FormatAuditType(const remotex::core::MachineAuditEvent& event) {
    if (event.event_type == "session_started") {
        return "Acesso iniciado";
    }
    if (event.event_type == "session_ended") {
        return "Acesso encerrado";
    }
    if (event.event_type == "file_upload_started") {
        return "Envio";
    }
    if (event.event_type == "file_upload_finished") {
        return "Envio concluido";
    }
    if (event.event_type == "file_download_started") {
        return "Recebimento";
    }
    if (event.event_type == "file_download_finished") {
        return "Recebimento concluido";
    }
    if (event.event_type == "file_transfer_failed") {
        return "Falha";
    }
    return QString::fromStdString(event.event_type);
}

QString FormatAuditStatus(const std::string& status) {
    if (status == "started") {
        return "Iniciado";
    }
    if (status == "completed") {
        return "Concluido";
    }
    if (status == "failed") {
        return "Falhou";
    }
    if (status == "cancelled") {
        return "Cancelado";
    }
    return status.empty() ? "-" : QString::fromStdString(status);
}

QString FormatBytes(uint64_t bytes) {
    if (bytes == 0) {
        return "-";
    }

    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit_index = 0;
    while (value >= 1024.0 && unit_index < 4) {
        value /= 1024.0;
        ++unit_index;
    }

    return QString("%1 %2").arg(QString::number(value, 'f', unit_index == 0 ? 0 : 1), units[unit_index]);
}

bool IsSessionAuditEvent(const remotex::core::MachineAuditEvent& event) {
    return event.event_type == "session_started" || event.event_type == "session_ended";
}

bool IsTransferAuditEvent(const remotex::core::MachineAuditEvent& event) {
    return event.event_type.starts_with("file_");
}

QString BuildAuditSummaryText(const remotex::core::MachineAuditEvent& event) {
    QString summary = QString::fromStdString(event.summary);

    if (IsTransferAuditEvent(event)) {
        QStringList details;
        if (event.transfer_total_entries > 0) {
            details.append(QString("%1 itens").arg(event.transfer_total_entries));
        }
        if (event.transfer_total_bytes > 0) {
            details.append(FormatBytes(event.transfer_total_bytes));
        }
        if (!details.isEmpty()) {
            summary += QString(" (%1)").arg(details.join(", "));
        }
    }

    return summary;
}

} // namespace

MainWindow::MainWindow(remotex::core::AuthClient* auth, QWidget* parent)
    : QMainWindow(parent)
    , auth_(auth) {
    ApplyConnectionSettings();
    SetupUI();
    SetupToolbar();

    ViewerUpdateConfig update_config;
    update_config.api_host = connection_settings_.api_host.toStdString();
    update_config.api_port = connection_settings_.api_port;
    update_config.use_tls = connection_settings_.api_use_tls;
    update_config.current_version = REMOTEX_VIEWER_VERSION;
    update_config.check_interval_sec = 3600;
    auto_updater_.SetListener(this);
    auto_updater_.Configure(update_config, auth_);
    auto_updater_.Start();

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(30000);
    connect(refresh_timer_, &QTimer::timeout, this, &MainWindow::RefreshMachines);
    refresh_timer_->start();

    RefreshMachines();
}

MainWindow::~MainWindow() = default;
