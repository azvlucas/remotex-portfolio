void MainWindow::RefreshMachines() {
    if (machine_refresh_in_flight_) {
        return;
    }

    machine_refresh_in_flight_ = true;
    if (act_refresh_) {
        act_refresh_->setEnabled(false);
    }
    status_label_->setText("Atualizando...");
    const quint64 request_id = ++machine_request_id_;
    const std::string previous_selected_id = GetSelectedMachineId();
    const ConnectionSettings settings = connection_settings_;

    auto* watcher = new QFutureWatcher<MachineRefreshResult>(this);
    connect(watcher, &QFutureWatcher<MachineRefreshResult>::finished, this, [this, watcher]() {
        const MachineRefreshResult result = watcher->result();
        watcher->deleteLater();

        if (result.request_id != machine_request_id_) {
            return;
        }

        machine_refresh_in_flight_ = false;
        if (act_refresh_) {
            act_refresh_->setEnabled(true);
        }

        machines_ = result.machines;
        ApplyFilters();
        RestoreSelectionByMachineId(result.previous_selected_id);
        UpdateSummary();
        UpdateMachineActionState();

        if (GetSelectedMachineId().empty()) {
            ++audit_request_id_;
            current_audit_machine_id_.clear();
            audit_events_.clear();
            ClearAuditPanel();
        }

        status_label_->setText("Atualizado");
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, settings, request_id, previous_selected_id]() {
        remotex::core::MachineClient client;
        client.Configure(settings.ToMachineClientConfig(), auth);

        MachineRefreshResult result;
        result.request_id = request_id;
        result.previous_selected_id = previous_selected_id;
        result.machines = client.ListMachines();
        return result;
    }));
}

void MainWindow::ApplyFilters() {
    visible_machine_indices_.clear();

    const QString search_text = search_input_ ? search_input_->text().trimmed() : QString();
    const int status_filter = status_filter_ ? status_filter_->currentIndex() : 0;

    for (int i = 0; i < static_cast<int>(machines_.size()); ++i) {
        const auto& machine = machines_[i];
        const bool matches_search = MatchesMachineSearch(machine, search_text);

        bool matches_status = true;
        if (status_filter == 1) {
            matches_status = machine.is_online;
        } else if (status_filter == 2) {
            matches_status = !machine.is_online;
        }

        if (matches_search && matches_status) {
            visible_machine_indices_.push_back(i);
        }
    }

    const QDateTime now_utc = QDateTime::currentDateTimeUtc();
    std::sort(visible_machine_indices_.begin(), visible_machine_indices_.end(),
              [this, now_utc](int lhs_index, int rhs_index) {
                  return CompareMachinesForDisplay(machines_[lhs_index], machines_[rhs_index], now_utc);
              });

    PopulateMachineTable();
}

void MainWindow::PopulateMachineTable() {
    table_->setRowCount(static_cast<int>(visible_machine_indices_.size()));
    const QDateTime now_utc = QDateTime::currentDateTimeUtc();

    for (int row = 0; row < static_cast<int>(visible_machine_indices_.size()); ++row) {
        const auto& machine = machines_[visible_machine_indices_[row]];
        const bool is_new_machine = IsMachineNew(machine, now_utc);
        const QString base_name = QString::fromStdString(machine.name);
        const QString display_name = is_new_machine
            ? QString("Nova | %1").arg(base_name)
            : base_name;

        auto* status_item = new QTableWidgetItem(MachineStatusText(machine));
        const bool is_blocked_machine = IsMachineBlocked(machine);
        status_item->setForeground(
            is_blocked_machine || !machine.is_online ? QColor(160, 0, 0) : QColor(0, 160, 0));
        if (const QString status_tooltip = MachineStatusTooltipText(machine); !status_tooltip.isEmpty()) {
            status_item->setToolTip(status_tooltip);
        }
        auto status_font = status_item->font();
        status_font.setBold(true);
        status_item->setFont(status_font);
        table_->setItem(row, 0, status_item);

        auto* name_item = new QTableWidgetItem(display_name);
        QStringList tooltip_lines;
        if (is_new_machine) {
            const QDateTime created_at = ParseIsoTimestampUtc(machine.created_at);
            const QString machine_age = FormatMachineAgeText(machine.created_at, now_utc);
            if (!machine_age.isEmpty()) {
                tooltip_lines.append(QString("Nova maquina cadastrada ha %1").arg(machine_age));
            }
            if (created_at.isValid()) {
                tooltip_lines.append(
                    QString("Cadastrada em: %1")
                        .arg(created_at.toLocalTime().toString("dd/MM/yyyy HH:mm:ss")));
            }
            auto font = name_item->font();
            font.setBold(true);
            name_item->setFont(font);
            name_item->setForeground(QColor(184, 120, 0));
        }
        if (!machine.legal_name.empty()) {
            tooltip_lines.append(
                QString("Razao social: %1").arg(QString::fromStdString(machine.legal_name)));
        }
        if (!machine.trade_name.empty()) {
            tooltip_lines.append(
                QString("Nome fantasia: %1").arg(QString::fromStdString(machine.trade_name)));
        }
        if (!machine.group_name.empty()) {
            tooltip_lines.append(
                QString("Grupo: %1").arg(QString::fromStdString(machine.group_name)));
        }
        if (!machine.subgroup_name.empty()) {
            tooltip_lines.append(
                QString("Subgrupo: %1").arg(QString::fromStdString(machine.subgroup_name)));
        }
        if (!tooltip_lines.isEmpty()) {
            name_item->setToolTip(tooltip_lines.join('\n'));
        }
        table_->setItem(row, 1, name_item);

        table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(machine.legal_name)));
        table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(machine.trade_name)));
        table_->setItem(row, 4, new QTableWidgetItem(FormatDocumentText(machine)));
        table_->setItem(row, 5, new QTableWidgetItem(QString::fromStdString(machine.group_name)));
        table_->setItem(row, 6, new QTableWidgetItem(QString::fromStdString(machine.subgroup_name)));

        auto* version_item = new QTableWidgetItem(QString::fromStdString(machine.agent_version));
        table_->setItem(row, 7, version_item);

        auto* hb_item = new QTableWidgetItem(FormatLastSeenText(machine.last_heartbeat));
        if (!machine.last_heartbeat.empty()) {
            hb_item->setToolTip(QString::fromStdString(machine.last_heartbeat));
        }
        table_->setItem(row, 8, hb_item);

        if (!machine.is_online) {
            const QColor muted(140, 140, 140);
            for (int col = 1; col < table_->columnCount(); ++col) {
                if (auto* item = table_->item(row, col)) {
                    item->setForeground(muted);
                }
            }
        }
    }
}

void MainWindow::RestoreSelectionByMachineId(const std::string& machine_id) {
    if (!table_) {
        return;
    }

    if (machine_id.empty()) {
        table_->clearSelection();
        return;
    }

    for (int row = 0; row < static_cast<int>(visible_machine_indices_.size()); ++row) {
        const auto& machine = machines_[visible_machine_indices_[row]];
        if (machine.id == machine_id) {
            table_->selectRow(row);
            return;
        }
    }

    table_->clearSelection();
}

void MainWindow::UpdateSummary() const {
    if (!summary_label_) {
        return;
    }

    int online_count = 0;
    int new_count = 0;
    const QDateTime now_utc = QDateTime::currentDateTimeUtc();
    for (const auto& machine : machines_) {
        if (machine.is_online) {
            ++online_count;
        }
        if (IsMachineNew(machine, now_utc)) {
            ++new_count;
        }
    }

    summary_label_->setText(
        QString("Total: %1 | Online: %2 | Novas: %3")
            .arg(machines_.size())
            .arg(online_count)
            .arg(new_count));
}

int MainWindow::GetSelectedMachineIndex() const {
    auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return -1;

    const int visible_row = rows.first().row();
    if (visible_row < 0 || visible_row >= static_cast<int>(visible_machine_indices_.size())) {
        return -1;
    }

    const int machine_index = visible_machine_indices_[visible_row];
    if (machine_index < 0 || machine_index >= static_cast<int>(machines_.size())) {
        return -1;
    }

    return machine_index;
}

std::string MainWindow::GetSelectedMachineId() const {
    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) return "";
    return machines_[machine_index].id;
}

void MainWindow::OnTableDoubleClicked(int row, int /*column*/) {
    if (row < 0 || row >= static_cast<int>(visible_machine_indices_.size())) return;
    if (!machines_[visible_machine_indices_[row]].is_online) return;
    OnConnect();
}

void MainWindow::OnSearchTextChanged(const QString& /*text*/) {
    ApplyFilters();
}

void MainWindow::OnStatusFilterChanged(int /*index*/) {
    ApplyFilters();
}

void MainWindow::OnMachineSelectionChanged() {
    const int machine_index = GetSelectedMachineIndex();
    UpdateMachineActionState();
    if (machine_index < 0) {
        ++audit_request_id_;
        current_audit_machine_id_.clear();
        audit_events_.clear();
        ClearAuditPanel();
        return;
    }

    const auto& machine = machines_[machine_index];
    if (audit_title_label_) {
        audit_title_label_->setText(
            QString("Historico - %1").arg(QString::fromStdString(machine.name)));
    }

    if (current_audit_machine_id_ == machine.id && !audit_events_.empty()) {
        return;
    }

    LoadAuditForMachine(machine.id, false);
}

void MainWindow::ApplyConnectionSettings() {
    connection_settings_ = ConnectionSettings::Load();
    auth_->Configure(connection_settings_.ToAuthConfig());
    machine_client_.Configure(connection_settings_.ToMachineClientConfig(), auth_);
}

bool MainWindow::CurrentUserIsManager() const {
    if (!auth_) {
        return false;
    }
    const auto role = auth_->GetRole();
    return role == "company_manager" || role == "super_admin";
}

void MainWindow::UpdateMachineActionState() {
    const int machine_index = GetSelectedMachineIndex();
    const bool has_machine = machine_index >= 0;
    const bool can_delete = has_machine && CurrentUserIsManager() && !delete_in_flight_;
    const bool can_access = has_machine
        && CanStartMachineSession(machines_[machine_index], CurrentUserIsManager());

    if (act_edit_) {
        act_edit_->setEnabled(has_machine);
    }
    if (act_delete_) {
        act_delete_->setEnabled(can_delete);
    }
    if (act_connect_) {
        act_connect_->setEnabled(can_access);
    }
    if (act_watch_only_) {
        act_watch_only_->setEnabled(can_access);
    }
    if (connect_button_) {
        connect_button_->setEnabled(can_access);
    }
    if (act_chat_) {
        act_chat_->setEnabled(has_machine && machine_index >= 0 && machines_[machine_index].is_online);
    }
}
