void MainWindow::LoadAuditForMachine(const std::string& machine_id, bool append) {
    if (!audit_table_ || !audit_summary_label_ || machine_id.empty()) {
        return;
    }

    if (!append) {
        audit_events_.clear();
        audit_offset_ = 0;
        audit_has_more_ = false;
        current_audit_machine_id_ = machine_id;
        ClearAuditPanel("Carregando historico...");
        if (audit_title_label_) {
            const int machine_index = GetSelectedMachineIndex();
            if (machine_index >= 0) {
                audit_title_label_->setText(
                    QString("Historico - %1").arg(QString::fromStdString(machines_[machine_index].name)));
            }
        }
    }

    audit_load_in_flight_ = true;
    if (audit_load_more_btn_) {
        audit_load_more_btn_->setEnabled(false);
        audit_load_more_btn_->setVisible(true);
    }

    const quint64 request_id = ++audit_request_id_;
    const int offset = append ? audit_offset_ : 0;
    const ConnectionSettings settings = connection_settings_;

    auto* watcher = new QFutureWatcher<AuditLoadResult>(this);
    connect(watcher, &QFutureWatcher<AuditLoadResult>::finished, this, [this, watcher]() {
        AuditLoadResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != audit_request_id_) {
            return;
        }

        audit_load_in_flight_ = false;

        if (!result.append) {
            audit_events_ = std::move(result.events);
            machine_recordings_ = std::move(result.recordings);
        } else if (current_audit_machine_id_ == result.machine_id) {
            audit_events_.insert(audit_events_.end(), result.events.begin(), result.events.end());
        }

        current_audit_machine_id_ = result.machine_id;
        audit_offset_ = static_cast<int>(audit_events_.size());
        audit_has_more_ = result.events.size() == 50;
        PopulateAuditTable();
        UpdateAuditSummary();

        if (audit_load_more_btn_) {
            audit_load_more_btn_->setEnabled(audit_has_more_);
            audit_load_more_btn_->setVisible(!current_audit_machine_id_.empty());
        }
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, settings, request_id, machine_id, append, offset]() {
        remotex::core::MachineClient client;
        client.Configure(settings.ToMachineClientConfig(), auth);

        AuditLoadResult result;
        result.request_id = request_id;
        result.machine_id = machine_id;
        result.append = append;
        result.events = client.ListMachineAudit(machine_id, 50, offset);
        if (!append) {
            result.recordings = client.ListRecordingsByMachine(machine_id, 200, 0);
        }
        return result;
    }));
}

void MainWindow::PopulateAuditTable() {
    if (!audit_table_) {
        return;
    }

    audit_table_->setRowCount(0);

    std::unordered_map<std::string, const remotex::core::RecordingInfo*> session_recording_map;
    for (const auto& rec : machine_recordings_) {
        if (!rec.session_id.empty() && rec.status == "uploaded") {
            session_recording_map[rec.session_id] = &rec;
        }
    }

    const auto consolidated = ConsolidateAuditEvents(audit_events_);
    auto* s = style();

    int row = 0;
    for (const auto& entry : consolidated) {
        const bool is_session = entry.category == ConsolidatedAuditEntry::Category::Session;
        const bool is_transfer = entry.category == ConsolidatedAuditEntry::Category::FileTransfer;

        if (audit_filter_ && audit_filter_->currentIndex() == 1 && !is_session) {
            continue;
        }
        if (audit_filter_ && audit_filter_->currentIndex() == 2 && !is_transfer) {
            continue;
        }

        audit_table_->insertRow(row);

        const std::string& display_time = !entry.started_at.empty() ? entry.started_at : entry.ended_at;
        audit_table_->setItem(row, 0, new QTableWidgetItem(FormatAuditTimestamp(display_time)));
        audit_table_->setItem(row, 1, new QTableWidgetItem(
            entry.user_name.empty() ? "-" : QString::fromStdString(entry.user_name)));

        QString type_text;
        QIcon type_icon;
        if (is_session) {
            type_text = "Acesso remoto";
            type_icon = s->standardIcon(QStyle::SP_ComputerIcon);
        } else {
            type_text = (entry.transfer_direction == "upload") ? "Envio" : "Recebimento";
            type_icon = s->standardIcon(QStyle::SP_DirOpenIcon);
        }
        auto* type_item = new QTableWidgetItem(type_icon, type_text);
        audit_table_->setItem(row, 2, type_item);

        QString duration_text;
        if (entry.has_duration) {
            duration_text = FormatDurationSeconds(entry.duration_sec);
        } else if (entry.status == "started") {
            duration_text = "Em andamento";
        } else {
            duration_text = "-";
        }
        audit_table_->setItem(row, 3, new QTableWidgetItem(duration_text));

        QString details;
        if (is_session) {
            details = entry.summary.empty() ? "-" : QString::fromStdString(entry.summary);
        } else {
            QStringList parts;
            if (entry.transfer_total_entries > 0) {
                parts.append(QString("%1 itens").arg(entry.transfer_total_entries));
            }
            if (entry.transfer_total_bytes > 0) {
                parts.append(FormatBytes(entry.transfer_total_bytes));
            }
            details = parts.isEmpty() ? QString::fromStdString(entry.summary) : parts.join(", ");
        }
        auto* details_item = new QTableWidgetItem(details);
        QStringList tooltip_lines;
        if (!entry.transfer_item.empty()) {
            tooltip_lines.append(QString("Item: %1").arg(QString::fromStdString(entry.transfer_item)));
        }
        if (!entry.error_message.empty()) {
            tooltip_lines.append(QString("Erro: %1").arg(QString::fromStdString(entry.error_message)));
        }
        if (!tooltip_lines.isEmpty()) {
            details_item->setToolTip(tooltip_lines.join('\n'));
        }
        audit_table_->setItem(row, 4, details_item);

        auto* status_item = new QTableWidgetItem(FormatAuditStatus(entry.status));
        if (entry.status == "completed") {
            status_item->setForeground(QColor(0, 160, 0));
        } else if (entry.status == "started") {
            status_item->setForeground(QColor(0, 100, 200));
        } else if (entry.status == "failed" || entry.status == "cancelled") {
            status_item->setForeground(QColor(160, 0, 0));
        }
        audit_table_->setItem(row, 5, status_item);

        auto* rec_item = new QTableWidgetItem;
        if (is_session && !entry.session_id.empty()) {
            auto rec_it = session_recording_map.find(entry.session_id);
            if (rec_it != session_recording_map.end()) {
                rec_item->setText("Disponivel");
                rec_item->setForeground(QColor(0, 120, 200));
                rec_item->setData(Qt::UserRole, QString::fromStdString(rec_it->second->id));
                auto font = rec_item->font();
                font.setUnderline(true);
                rec_item->setFont(font);
            } else {
                rec_item->setText("-");
            }
        } else {
            rec_item->setText("-");
        }
        audit_table_->setItem(row, 6, rec_item);

        ++row;
    }

    if (audit_empty_label_) {
        audit_empty_label_->setVisible(audit_table_->rowCount() == 0);
        if (audit_table_->rowCount() == 0 && !current_audit_machine_id_.empty()) {
            audit_empty_label_->setText("Nenhum evento encontrado para o filtro selecionado.");
        }
    }

    if (audit_download_btn_) {
        audit_download_btn_->setEnabled(false);
    }
}

void MainWindow::UpdateAuditSummary() {
    if (!audit_summary_label_) {
        return;
    }

    if (current_audit_machine_id_.empty()) {
        audit_summary_label_->setText("Selecione uma maquina para ver acessos e transferencias recentes.");
        return;
    }

    const auto consolidated = ConsolidateAuditEvents(audit_events_);

    int session_count = 0;
    int transfer_count = 0;
    int total_duration = 0;
    QString latest_event = "-";

    for (const auto& entry : consolidated) {
        if (latest_event == "-") {
            const std::string& ts = !entry.started_at.empty() ? entry.started_at : entry.ended_at;
            if (!ts.empty()) {
                latest_event = FormatAuditTimestamp(ts);
            }
        }
        if (entry.category == ConsolidatedAuditEntry::Category::Session) {
            ++session_count;
            if (entry.has_duration) {
                total_duration += entry.duration_sec;
            }
        } else {
            ++transfer_count;
        }
    }

    audit_summary_label_->setText(
        QString("Resumo recente: %1 acessos | %2 transferencias | %3 acumulados | Ultimo evento: %4")
            .arg(session_count)
            .arg(transfer_count)
            .arg(FormatDurationSeconds(total_duration))
            .arg(latest_event));
}

void MainWindow::ClearAuditPanel(const QString& message) {
    const QString panel_message = message.isEmpty()
        ? "Selecione uma maquina para ver acessos e transferencias recentes."
        : message;

    if (audit_title_label_) {
        audit_title_label_->setText("Historico da maquina");
    }
    if (audit_summary_label_) {
        audit_summary_label_->setText(panel_message);
    }
    if (audit_empty_label_) {
        audit_empty_label_->setText(panel_message);
        audit_empty_label_->setVisible(true);
    }
    if (audit_table_) {
        audit_table_->setRowCount(0);
    }
    if (audit_load_more_btn_) {
        audit_load_more_btn_->setEnabled(false);
        audit_load_more_btn_->setVisible(false);
    }
    if (audit_download_btn_) {
        audit_download_btn_->setEnabled(false);
    }
    machine_recordings_.clear();
}

bool MainWindow::AuditEventMatchesCurrentFilter(const remotex::core::MachineAuditEvent& event) const {
    if (!audit_filter_) {
        return true;
    }

    if (audit_filter_->currentIndex() == 1) {
        return IsSessionAuditEvent(event);
    }
    if (audit_filter_->currentIndex() == 2) {
        return IsTransferAuditEvent(event);
    }
    return true;
}

void MainWindow::OnAuditSelectionChanged() {
    if (!audit_download_btn_ || !audit_table_) {
        return;
    }

    auto rows = audit_table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        audit_download_btn_->setEnabled(false);
        return;
    }

    const int row = rows.first().row();
    auto* rec_item = audit_table_->item(row, 6);
    const bool has_recording = rec_item && rec_item->data(Qt::UserRole).isValid()
                               && !rec_item->data(Qt::UserRole).toString().isEmpty();
    audit_download_btn_->setEnabled(has_recording && !recording_download_in_flight_);
}

void MainWindow::OnAuditDownloadRecording() {
    if (recording_download_in_flight_ || !audit_table_) {
        return;
    }

    auto rows = audit_table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }

    const int row = rows.first().row();
    auto* rec_item = audit_table_->item(row, 6);
    if (!rec_item) {
        return;
    }

    const QString recording_id = rec_item->data(Qt::UserRole).toString();
    if (recording_id.isEmpty()) {
        return;
    }

    recording_download_in_flight_ = true;
    if (audit_download_btn_) {
        audit_download_btn_->setEnabled(false);
    }
    status_label_->setText("Obtendo link de download...");

    const quint64 request_id = ++recording_download_request_id_;
    const ConnectionSettings settings = connection_settings_;

    auto* watcher = new QFutureWatcher<RecordingDownloadResult>(this);
    connect(watcher, &QFutureWatcher<RecordingDownloadResult>::finished, this, [this, watcher]() {
        const RecordingDownloadResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != recording_download_request_id_) {
            return;
        }

        recording_download_in_flight_ = false;
        OnAuditSelectionChanged();

        if (!result.error.isEmpty()) {
            status_label_->setText(result.error);
            return;
        }

        if (!result.download_url.isEmpty()) {
            QString open_error;
            if (OpenPresignedExternalUrlInBrowser(result.download_url, &open_error)) {
                status_label_->setText("Download da gravacao iniciado no navegador.");
            } else {
                status_label_->setText(open_error);
            }
        }
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, settings, recording_id, request_id]() {
        RecordingDownloadResult result;
        result.request_id = request_id;

        remotex::core::MachineClient client;
        client.Configure(settings.ToMachineClientConfig(), auth);

        const auto download = client.GetRecordingDownloadURL(recording_id.toStdString());
        if (!download.error.empty()) {
            result.error = QString::fromStdString(download.error);
            if (download.http_code > 0) {
                result.error += QString(" (HTTP %1)").arg(download.http_code);
            }
        } else if (download.url.empty()) {
            result.error = "Falha ao obter URL de download da gravacao";
        } else {
            result.download_url = QString::fromStdString(download.url);
        }
        return result;
    }));
}

void MainWindow::OnAuditTableContextMenu(const QPoint& pos) {
    const QModelIndex index = audit_table_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    audit_table_->selectRow(index.row());

    auto* rec_item = audit_table_->item(index.row(), 6);
    const bool has_recording = rec_item && rec_item->data(Qt::UserRole).isValid()
                               && !rec_item->data(Qt::UserRole).toString().isEmpty();

    if (!has_recording) {
        return;
    }

    QMenu menu(this);
    menu.addAction(style()->standardIcon(QStyle::SP_MediaPlay),
                   "Baixar gravacao", this, &MainWindow::OnAuditDownloadRecording);
    menu.exec(audit_table_->viewport()->mapToGlobal(pos));
}

void MainWindow::OnAuditFilterChanged(int /*index*/) {
    PopulateAuditTable();
}

void MainWindow::OnAuditLoadMore() {
    if (!audit_load_in_flight_ && !current_audit_machine_id_.empty() && audit_has_more_) {
        LoadAuditForMachine(current_audit_machine_id_, true);
    }
}
