void MainWindow::OnEditMachine() {
    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) {
        QMessageBox::information(this, "Editar", "Selecione uma maquina.");
        return;
    }

    MachineEditDialog dlg(auth_, machines_[machine_index], this);
    if (dlg.exec() == QDialog::Accepted) {
        RefreshMachines();
    }
}

void MainWindow::OnDeleteMachine() {
    if (delete_in_flight_) {
        return;
    }

    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) {
        QMessageBox::information(this, "Excluir", "Selecione uma maquina.");
        return;
    }

    if (!CurrentUserIsManager()) {
        QMessageBox::warning(this, "Excluir", "Apenas gerentes podem excluir maquinas.");
        return;
    }

    const auto id = machines_[machine_index].id;
    const auto name = QString::fromStdString(machines_[machine_index].name);

    auto reply = QMessageBox::question(this, "Confirmar Exclusao",
        QString("Excluir a maquina \"%1\"?").arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    delete_in_flight_ = true;
    if (act_delete_) {
        act_delete_->setEnabled(false);
    }
    UpdateMachineActionState();
    status_label_->setText("Excluindo maquina...");

    const quint64 request_id = ++delete_request_id_;
    const ConnectionSettings settings = connection_settings_;

    auto* watcher = new QFutureWatcher<DeleteMachineResult>(this);
    connect(watcher, &QFutureWatcher<DeleteMachineResult>::finished, this, [this, watcher]() {
        const DeleteMachineResult delete_result = watcher->result();
        watcher->deleteLater();

        if (delete_result.request_id != delete_request_id_) {
            return;
        }

        delete_in_flight_ = false;
        if (act_delete_) {
            act_delete_->setEnabled(true);
        }
        UpdateMachineActionState();

        if (delete_result.result.success) {
            RefreshMachines();
            return;
        }

        status_label_->setText("Erro ao excluir maquina");
        QMessageBox::warning(this, "Erro",
            QString::fromStdString(delete_result.result.error.empty()
                ? "Falha ao excluir"
                : delete_result.result.error));
    });

    watcher->setFuture(QtConcurrent::run([auth = auth_, settings, request_id, id]() {
        remotex::core::MachineClient client;
        client.Configure(settings.ToMachineClientConfig(), auth);

        DeleteMachineResult result;
        result.request_id = request_id;
        result.machine_id = id;
        result.result = client.DeleteMachine(id);
        return result;
    }));
}

void MainWindow::OnConnect() {
    OpenSelectedMachine(remotex::core::SessionAccessMode::Interactive);
}

void MainWindow::OnWatchOnly() {
    OpenSelectedMachine(remotex::core::SessionAccessMode::WatchOnly);
}

void MainWindow::OnChat() {
    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) {
        QMessageBox::information(this, "Chat", "Selecione uma maquina.");
        return;
    }

    if (!machines_[machine_index].is_online) {
        QMessageBox::information(this, "Chat", "A maquina esta offline.");
        return;
    }

    auto* dlg = new StandaloneChatDialog(
        auth_,
        QString::fromStdString(machines_[machine_index].id),
        QString::fromStdString(machines_[machine_index].name),
        this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::OpenSelectedMachine(remotex::core::SessionAccessMode access_mode) {
    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) {
        QMessageBox::information(this, "Conectar", "Selecione uma maquina.");
        return;
    }

    const auto& machine = machines_[machine_index];
    const QString denied_message = MachineSessionAccessDeniedMessage(machine, CurrentUserIsManager());
    if (!denied_message.isEmpty()) {
        if (IsMachineBlocked(machine)) {
            QMessageBox::warning(this, "Conectar", denied_message);
        } else if (!machine.is_online) {
            QMessageBox::information(this, "Conectar", denied_message);
        } else {
            QMessageBox::warning(this, "Conectar", denied_message);
        }
        return;
    }

    const auto name = QString::fromStdString(machine.name);
    auto* session = new RemoteSessionWindow(
        auth_,
        QString::fromStdString(machine.id),
        name,
        access_mode);
    session->setAttribute(Qt::WA_DeleteOnClose);
    session->show();
}

void MainWindow::OnSettings() {
    SettingsDialog dlg(auth_, QString::fromStdString(GetSelectedMachineId()), this);
    if (dlg.exec() == QDialog::Accepted) {
        ApplyConnectionSettings();
        RefreshMachines();
    }
}

void MainWindow::OnManageGroups() {
    GroupManagementDialog dlg(auth_, this);
    dlg.exec();
    if (dlg.HasChanges()) {
        RefreshMachines();
    }
}

void MainWindow::OnManageUsers() {
    UserManagementDialog dlg(auth_, this);
    dlg.exec();
}

void MainWindow::OnReports() {
    ReportsDialog dlg(auth_, this);
    dlg.exec();
}

void MainWindow::OnTableContextMenu(const QPoint& pos) {
    const QModelIndex index = table_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    table_->selectRow(index.row());

    auto* s = style();
    QMenu menu(this);
    const int machine_index = GetSelectedMachineIndex();
    const bool has_machine = machine_index >= 0;
    const bool can_access = has_machine
        && CanStartMachineSession(machines_[machine_index], CurrentUserIsManager());
    auto* connect_action = menu.addAction(
        s->standardIcon(QStyle::SP_MediaPlay), "Controlar", this, &MainWindow::OnConnect);
    connect_action->setEnabled(can_access);
    auto* watch_action = menu.addAction(
        s->standardIcon(QStyle::SP_FileDialogContentsView), "Apenas assistir", this, &MainWindow::OnWatchOnly);
    watch_action->setEnabled(can_access);
    auto* chat_action = menu.addAction(
        s->standardIcon(QStyle::SP_MessageBoxInformation), "Chat", this, &MainWindow::OnChat);
    chat_action->setEnabled(has_machine && machines_[machine_index].is_online);
    menu.addSeparator();
    auto* edit_action = menu.addAction(
        s->standardIcon(QStyle::SP_FileDialogDetailedView), "Editar", this, &MainWindow::OnEditMachine);
    edit_action->setEnabled(has_machine);
    auto* delete_action = menu.addAction(
        s->standardIcon(QStyle::SP_DialogDiscardButton), "Excluir", this, &MainWindow::OnDeleteMachine);
    delete_action->setEnabled(has_machine && CurrentUserIsManager() && !delete_in_flight_);
    menu.addSeparator();
    auto* force_update_action = menu.addAction(
        s->standardIcon(QStyle::SP_BrowserReload), "Atualizar versao do Host", this, &MainWindow::OnForceHostUpdate);
    force_update_action->setEnabled(has_machine && machines_[machine_index].is_online);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::OnLogout() {
    auto reply = QMessageBox::question(this, "Sair",
        "Deseja realmente sair?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    refresh_timer_->stop();
    emit LoggedOut();
    close();
}

void MainWindow::OnUpdateNow() {
    if (auto_updater_.PendingVersion().empty()) {
        statusBar()->showMessage("Nenhuma atualizacao pendente", 3000);
        return;
    }
    if (update_download_in_flight_) return;
    update_download_in_flight_ = true;

    update_btn_->setEnabled(false);
    update_btn_->setText("Baixando...");
    if (update_label_) update_label_->setText("Baixando atualizacao...");

    struct UpdateResult { bool staged = false; bool applied = false; };

    auto* watcher = new QFutureWatcher<UpdateResult>(this);
    connect(watcher, &QFutureWatcher<UpdateResult>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        auto result = watcher->result();
        update_download_in_flight_ = false;

        if (!result.staged) {
            update_btn_->setEnabled(true);
            update_btn_->setText("Atualizar agora");
            if (update_label_) {
                auto ver = QString::fromStdString(auto_updater_.PendingVersion());
                update_label_->setText(QString("Atualizacao disponivel (v%1) - Falha ao baixar").arg(ver));
            }
            statusBar()->showMessage("Falha ao baixar atualizacao", 5000);
            return;
        }

        if (update_label_) update_label_->setText("Aplicando atualizacao...");
        if (auto_updater_.ApplyAndRelaunch()) {
            QApplication::quit();
        } else {
            update_btn_->setEnabled(true);
            update_btn_->setText("Atualizar agora");
            statusBar()->showMessage("Falha ao aplicar atualizacao", 5000);
        }
    });

    watcher->setFuture(QtConcurrent::run([this]() -> UpdateResult {
        UpdateResult r;
        r.staged = auto_updater_.DownloadAndStage();
        return r;
    }));
}

void MainWindow::OnForceHostUpdate() {
    const int machine_index = GetSelectedMachineIndex();
    if (machine_index < 0) return;

    const auto& machine = machines_[machine_index];
    auto machine_name = QString::fromStdString(machine.name);
    auto machine_id = machine.id;

    auto answer = QMessageBox::question(this, "Atualizar Host",
        QString("Solicitar atualizacao do host \"%1\"?\n\n"
                "O host sera atualizado na proxima verificacao (ate 30 segundos).")
            .arg(machine_name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes) return;

    statusBar()->showMessage("Solicitando atualizacao do host...");

    auto* watcher = new QFutureWatcher<remotex::core::MachineResult>(this);
    connect(watcher, &QFutureWatcher<remotex::core::MachineResult>::finished, this, [this, watcher, machine_name]() {
        watcher->deleteLater();
        auto result = watcher->result();
        if (result.success) {
            statusBar()->showMessage(
                QString("Atualizacao solicitada para \"%1\"").arg(machine_name), 5000);
        } else {
            statusBar()->showMessage(
                QString("Falha ao solicitar atualizacao: %1").arg(QString::fromStdString(result.error)), 5000);
        }
    });

    auto settings = connection_settings_;
    watcher->setFuture(QtConcurrent::run([auth = auth_, settings, machine_id]() {
        remotex::core::MachineClient client;
        client.Configure({settings.api_host.toStdString(), settings.api_port, settings.api_use_tls}, auth);
        return client.RequestForceUpdate(machine_id);
    }));
}

void MainWindow::OnUpdateAvailable(const std::string& version) {
    QString v = QString::fromStdString(version);
    QMetaObject::invokeMethod(this, [this, v]() { ShowUpdateBar(v); },
                              Qt::QueuedConnection);
}

void MainWindow::OnDownloadProgress(int64_t downloaded, int64_t total) {
    QMetaObject::invokeMethod(this, [this, downloaded, total]() {
        if (!update_label_ || !update_download_in_flight_) return;
        if (total > 0) {
            int pct = static_cast<int>(downloaded * 100 / total);
            double mb_down = downloaded / (1024.0 * 1024.0);
            double mb_total = total / (1024.0 * 1024.0);
            update_label_->setText(QString("Baixando atualizacao... %1% (%2 / %3 MB)")
                .arg(pct).arg(mb_down, 0, 'f', 1).arg(mb_total, 0, 'f', 1));
        } else {
            double mb_down = downloaded / (1024.0 * 1024.0);
            update_label_->setText(QString("Baixando atualizacao... %1 MB").arg(mb_down, 0, 'f', 1));
        }
    }, Qt::QueuedConnection);
}

void MainWindow::OnUpdateReady(const std::string& version) {
    QString v = QString::fromStdString(version);
    QMetaObject::invokeMethod(this, [this, v]() {
        if (update_label_) update_label_->setText(QString("Atualizacao v%1 pronta para instalar").arg(v));
    }, Qt::QueuedConnection);
}

void MainWindow::OnUpdateError(const std::string& error) {
    QString msg = QString::fromStdString(error);
    QMetaObject::invokeMethod(this, [this, msg]() {
        statusBar()->showMessage(QString("Auto-update: %1").arg(msg), 5000);
    }, Qt::QueuedConnection);
}

void MainWindow::ShowUpdateBar(const QString& version) {
    if (update_bar_) {
        if (update_label_) update_label_->setText(QString("Atualizacao disponivel (v%1)").arg(version));
        update_bar_->setVisible(true);
    }
}
