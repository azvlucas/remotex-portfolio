void RemoteSessionWindow::SetupToolbar() {
    toolbar_ = addToolBar("Sessao");
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto* s = style();
    auto set_action_hints = [](QAction* action, const QString& tooltip, const QString& status_tip = QString()) {
        if (!action) {
            return;
        }

        action->setToolTip(tooltip);
        action->setStatusTip(status_tip.isEmpty() ? tooltip : status_tip);
    };

    act_transfer_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DirOpenIcon), "Transferir Arquivos", this, &RemoteSessionWindow::OnTransferFile);
    set_action_hints(act_transfer_,
                     "Transferir arquivos",
                     "Abre a transferencia de arquivos com a maquina remota");
    act_transfer_->setEnabled(false);
    act_chat_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_MessageBoxInformation), "Chat", this, [this]() {
            if (chat_panel_) {
                chat_panel_->setVisible(!chat_panel_->isVisible());
            }
        });
    set_action_hints(act_chat_,
                     "Abrir chat",
                     "Mostra ou oculta o painel de chat da sessao remota");
    act_cad_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_MessageBoxWarning), "Ctrl+Alt+Del", this, &RemoteSessionWindow::OnSendCtrlAltDel);
    set_action_hints(act_cad_,
                     "Enviar Ctrl+Alt+Del seguro",
                     "Solicita Ctrl+Alt+Del seguro ao Host remoto");
    act_reboot_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_BrowserReload), "Reiniciar PC", this, &RemoteSessionWindow::OnRebootClicked);
    set_action_hints(act_reboot_,
                     "Reiniciar computador remoto",
                     "Solicita o reinicio do computador remoto");
    act_reboot_->setEnabled(false);

    act_privacy_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_ComputerIcon), "Tela Preta", this, &RemoteSessionWindow::OnTogglePrivacyMode);
    set_action_hints(act_privacy_,
                     "Alternar tela preta",
                     "Ativa ou desativa a tela preta no Host remoto");
    act_privacy_->setCheckable(true);
    act_privacy_->setEnabled(false);

    act_block_input_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DialogNoButton), "Bloquear Entrada", this, &RemoteSessionWindow::OnToggleBlockInput);
    set_action_hints(act_block_input_,
                     "Alternar bloqueio de entrada",
                     "Bloqueia ou libera mouse e teclado locais no Host remoto");
    act_block_input_->setCheckable(true);
    act_block_input_->setEnabled(false);

    toolbar_->addSeparator();
    monitor_combo_ = new QComboBox();
    monitor_combo_->setToolTip("Selecionar monitor remoto");
    monitor_combo_->setStatusTip("Seleciona qual monitor remoto sera exibido nesta sessao");
    monitor_combo_->setMinimumWidth(160);
    monitor_combo_->setEnabled(false);
    toolbar_->addWidget(monitor_combo_);
    toolbar_->addSeparator();

    act_fullscreen_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_TitleBarMaxButton), "Tela Cheia", this, &RemoteSessionWindow::OnToggleFullscreen);
    UpdateFullscreenActionHints();
    toolbar_->addSeparator();
    act_disconnect_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DialogCancelButton), "Desconectar", this, &RemoteSessionWindow::OnDisconnectClicked);
    set_action_hints(act_disconnect_,
                     "Desconectar sessao",
                     "Encerra a sessao remota atual");

    if (!IsInteractiveSession()) {
        act_transfer_->setVisible(false);
        act_cad_->setVisible(false);
        act_reboot_->setVisible(false);
        act_privacy_->setVisible(false);
        act_block_input_->setVisible(false);
    }
}

void RemoteSessionWindow::UpdateFullscreenActionHints() {
    if (!act_fullscreen_) {
        return;
    }

    if (is_fullscreen_) {
        act_fullscreen_->setToolTip("Sair da tela cheia");
        act_fullscreen_->setStatusTip("Sai da sessao em tela cheia e restaura a barra de ferramentas");
    } else {
        act_fullscreen_->setToolTip("Entrar em tela cheia");
        act_fullscreen_->setStatusTip("Expande a sessao remota para tela cheia e oculta a barra de ferramentas");
    }
}

void RemoteSessionWindow::OnConnected() {
    retry_timer_->stop();
    retry_btn_->hide();
    received_video_frame_ = false;
    status_label_->setText(IsInteractiveSession()
                               ? "Host conectado - aguardando video..."
                               : "Host conectado - somente visualizacao, aguardando video...");
    UpdateFullscreenOverlayTexts();
    if (video_wait_timer_) {
        video_wait_timer_->start();
    }
    if (IsInteractiveSession() && act_transfer_) {
        act_transfer_->setEnabled(true);
    }
    if (IsInteractiveSession() && act_reboot_) {
        act_reboot_->setEnabled(true);
    }
    if (IsInteractiveSession() && act_cad_) {
        act_cad_->setEnabled(!secure_attention_request_pending_);
    }
    if (IsInteractiveSession() && act_privacy_) {
        act_privacy_->setEnabled(true);
        act_privacy_->setChecked(privacy_mode_active_);
        privacy_mode_request_pending_ = false;
    }
    if (IsInteractiveSession() && act_block_input_) {
        act_block_input_->setEnabled(true);
        act_block_input_->setChecked(block_input_active_);
        block_input_request_pending_ = false;
    }

    if (IsInteractiveSession() && input_capture_) {
        input_capture_->SetEnabled(true);
    }
    if (IsInteractiveSession() && network_worker_ && pending_num_lock_policy_.has_value()) {
        QMetaObject::invokeMethod(network_worker_, [this]() {
            network_worker_->SendNumLockPolicy(*pending_num_lock_policy_);
        }, Qt::QueuedConnection);
    }
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnDisconnected(const QString& reason) {
    if (video_wait_timer_) {
        video_wait_timer_->stop();
    }
    if (transfer_toast_timer_) {
        transfer_toast_timer_->stop();
    }
    HideTransferToast();
    status_label_->setText(QString("Desconectado: %1").arg(reason));
    received_video_frame_ = false;
    UpdateFullscreenOverlayTexts();
    if (act_transfer_) {
        act_transfer_->setEnabled(false);
    }
    if (act_reboot_) {
        act_reboot_->setEnabled(false);
    }
    if (act_cad_) {
        act_cad_->setEnabled(false);
        secure_attention_request_pending_ = false;
    }
    if (act_privacy_) {
        act_privacy_->setEnabled(false);
        act_privacy_->setChecked(false);
        privacy_mode_active_ = false;
    }
    if (act_block_input_) {
        act_block_input_->setEnabled(false);
        act_block_input_->setChecked(false);
        block_input_active_ = false;
    }
    if (input_capture_) {
        input_capture_->SetEnabled(false);
    }
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnVideoFrame(const remotex::viewer::VideoFramePacketPtr& frame) {
    if (!frame) {
        return;
    }

    if (!received_video_frame_) {
        received_video_frame_ = true;
        if (video_wait_timer_) {
            video_wait_timer_->stop();
        }
        status_label_->setText(IsInteractiveSession()
                                    ? "Conectado"
                                    : "Somente visualizacao - video ativo");
        UpdateFullscreenOverlayTexts();
    }
    renderer_->SubmitFrame(frame);
    if (is_fullscreen_) {
        UpdateFullscreenOverlayGeometry();
    }
}

void RemoteSessionWindow::OnTransferFile() {
    if (!IsInteractiveSession()) {
        return;
    }
    if (!transfer_dialog_) {
        transfer_dialog_ = new FileTransferDialog(network_worker_, this);
        transfer_dialog_->setAttribute(Qt::WA_DeleteOnClose);
        connect(transfer_dialog_, &QObject::destroyed, this, [this]() {
            transfer_dialog_ = nullptr;
        });
    }

    transfer_dialog_->show();
    transfer_dialog_->raise();
    transfer_dialog_->activateWindow();
}

void RemoteSessionWindow::OnClipboardFilePasteShortcut() {
    if (!IsInteractiveSession() || !network_worker_) {
        return;
    }

    QMetaObject::invokeMethod(network_worker_, "TriggerClipboardPaste", Qt::QueuedConnection);
}

void RemoteSessionWindow::OnSendCtrlAltDel() {
    if (!IsInteractiveSession() || !network_worker_ || !act_cad_ || secure_attention_request_pending_) {
        return;
    }

    secure_attention_request_pending_ = true;
    act_cad_->setEnabled(false);
    statusBar()->showMessage("Solicitando Ctrl+Alt+Del seguro ao host...", 3000);
    status_label_->setText("Aguardando confirmacao do Host para Ctrl+Alt+Del...");
    UpdateFullscreenOverlayTexts();
    ShowFullscreenOverlayTemporarily();

    QMetaObject::invokeMethod(network_worker_, "RequestSecureAttention", Qt::QueuedConnection);
}

void RemoteSessionWindow::OnDisconnectClicked() {
    close();
}

void RemoteSessionWindow::OnRebootClicked() {
    auto result = QMessageBox::warning(this,
        "Reiniciar computador remoto",
        "Tem certeza que deseja reiniciar o computador remoto?\n\n"
        "Todos os programas abertos serao fechados.\n"
        "A reconexao sera feita automaticamente apos o reinicio.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (result == QMessageBox::Yes) {
        if (act_reboot_) act_reboot_->setEnabled(false);
        status_label_->setText("Solicitando reinicio...");
        UpdateFullscreenOverlayTexts();
        QMetaObject::invokeMethod(network_worker_, "SendRebootRequest", Qt::QueuedConnection);
    }
}

void RemoteSessionWindow::OnRebootAcknowledged() {
    status_label_->setText("Host reiniciando - aguardando reconexao...");
    UpdateFullscreenOverlayTexts();
    ShowFullscreenOverlayTemporarily();
    if (act_transfer_) act_transfer_->setEnabled(false);
    if (act_cad_) act_cad_->setEnabled(false);
    if (act_reboot_) act_reboot_->setEnabled(false);
    if (retry_timer_) {
        retry_timer_->stop();
        retry_btn_->hide();
    }
}

void RemoteSessionWindow::OnRebootFailed(const QString& error) {
    if (act_reboot_) act_reboot_->setEnabled(true);
    QMessageBox::warning(this, "Falha ao reiniciar",
        QString("Nao foi possivel reiniciar o computador remoto.\n\nErro: %1").arg(error));
}

void RemoteSessionWindow::OnSecureAttentionStatusChanged(const remotex::viewer::SecureAttentionStatus& status) {
    secure_attention_request_pending_ = false;
    if (act_cad_) {
        act_cad_->setEnabled(IsInteractiveSession());
    }

    QString message;
    QString detail;
    bool show_dialog = false;
    if (!status.error.isEmpty()) {
        if (status.error == "watch_only_session") {
            message = "Ctrl+Alt+Del indisponivel em sessao somente visualizacao.";
        } else if (status.error == "request_in_progress") {
            message = "Host ainda esta processando a solicitacao anterior de Ctrl+Alt+Del seguro.";
        } else if (status.error == "unsupported_runtime_context") {
            message = "Host nao pode executar Ctrl+Alt+Del seguro fora do servico do Windows.";
            detail = "O Host recebeu a solicitacao, mas a sessao atual nao esta no contexto privilegiado exigido pelo Windows para SAS.";
            show_dialog = true;
        } else if (status.error == "no_active_console_session") {
            message = "Host nao encontrou uma sessao local ativa para entregar Ctrl+Alt+Del seguro.";
            detail = "Verifique se o Windows remoto possui uma sessao de console ativa e desbloqueada.";
            show_dialog = true;
        } else if (status.error == "session_query_failed") {
            message = "Host nao conseguiu validar o contexto da sessao antes de executar Ctrl+Alt+Del seguro.";
            detail = "Consulte os logs do Host para o erro detalhado da consulta de sessao.";
            show_dialog = true;
        } else if (status.error == "sas_api_unavailable") {
            message = "Host nao oferece a API do Windows necessaria para Ctrl+Alt+Del seguro nesta execucao.";
            detail = "O ambiente remoto nao disponibilizou sas.dll/SendSAS para o servico do Host.";
            show_dialog = true;
        } else if (status.error == "sas_api_failed") {
            message = "Host tentou executar Ctrl+Alt+Del seguro, mas o Windows recusou a chamada privilegiada.";
            detail = "Consulte os logs do Host para o codigo de erro do Windows retornado por SendSAS.";
            show_dialog = true;
        } else {
            message = "Host falhou ao executar Ctrl+Alt+Del seguro.";
            detail = QString("O Host retornou um erro nao reconhecido: %1").arg(status.error);
            show_dialog = true;
        }

        status_label_->setText(message);
        statusBar()->showMessage(message, 7000);
        if (show_dialog) {
            QMessageBox::warning(this,
                                 "Ctrl+Alt+Del seguro indisponivel",
                                 detail.isEmpty() ? message : QString("%1\n\n%2").arg(message, detail));
        }
    } else if (status.acknowledged) {
        message = "Host confirmou a solicitacao de Ctrl+Alt+Del seguro.";
        status_label_->setText(message);
        statusBar()->showMessage(message, 5000);
        if (network_worker_) {
            QMetaObject::invokeMethod(network_worker_, "SendRefreshRequest", Qt::QueuedConnection);
        }
    } else {
        message = "Host nao confirmou a solicitacao de Ctrl+Alt+Del seguro.";
        detail = "A confirmacao do Host nao chegou ou veio incompleta. Verifique a conectividade da sessao e os logs do Host.";
        status_label_->setText(message);
        statusBar()->showMessage(message, 7000);
        QMessageBox::warning(this,
                             "Ctrl+Alt+Del seguro sem confirmacao",
                             QString("%1\n\n%2").arg(message, detail));
    }

    UpdateFullscreenOverlayTexts();
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnTogglePrivacyMode() {
    if (!IsInteractiveSession() || !network_worker_ || !act_privacy_ || privacy_mode_request_pending_) {
        return;
    }

    const bool requested_enabled = !privacy_mode_active_;
    privacy_mode_request_pending_ = true;
    act_privacy_->setEnabled(false);
    act_privacy_->setChecked(requested_enabled);
    statusBar()->showMessage(
        requested_enabled ? "Solicitando tela preta ao host..." : "Solicitando desativacao da tela preta...",
        3000);
    ShowFullscreenOverlayTemporarily();

    QMetaObject::invokeMethod(network_worker_, [this, requested_enabled]() {
        network_worker_->SetPrivacyMode(requested_enabled);
    }, Qt::QueuedConnection);
}

void RemoteSessionWindow::OnLocalNumLockStateChanged(bool enabled) {
    pending_num_lock_policy_ = enabled;
    if (!IsInteractiveSession() || !network_worker_) {
        return;
    }

    QMetaObject::invokeMethod(network_worker_, [this, enabled]() {
        network_worker_->SendNumLockPolicy(enabled);
    }, Qt::QueuedConnection);
}

void RemoteSessionWindow::OnPrivacyModeStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status) {
    privacy_mode_request_pending_ = false;
    privacy_mode_active_ = status.enabled;
    if (act_privacy_) {
        act_privacy_->setEnabled(true);
        act_privacy_->setChecked(status.enabled);
    }

    QString message;
    if (!status.error.isEmpty()) {
        if (status.error == "watch_only_session") {
            message = "Tela preta indisponivel em sessao somente visualizacao.";
        } else if (status.error == "privacy_mode_apply_failed") {
            message = status.enabled
                ? "Host nao conseguiu confirmar a tela preta solicitada."
                : "Host nao conseguiu remover a tela preta solicitada.";
        } else {
            message = QString("Falha ao atualizar tela preta no host: %1").arg(status.error);
        }
    } else {
        message = status.enabled ? "Tela preta ativada no host" : "Tela preta desativada";
    }

    statusBar()->showMessage(message, 5000);
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnToggleBlockInput() {
    if (!IsInteractiveSession() || !network_worker_ || !act_block_input_ || block_input_request_pending_) {
        return;
    }

    const bool requested_enabled = !block_input_active_;
    block_input_request_pending_ = true;
    act_block_input_->setEnabled(false);
    act_block_input_->setChecked(requested_enabled);
    statusBar()->showMessage(
        requested_enabled ? "Solicitando bloqueio de entrada ao host..." : "Solicitando desbloqueio de entrada...",
        3000);
    ShowFullscreenOverlayTemporarily();

    QMetaObject::invokeMethod(network_worker_, [this, requested_enabled]() {
        network_worker_->SetBlockInput(requested_enabled);
    }, Qt::QueuedConnection);
}

void RemoteSessionWindow::OnBlockInputStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status) {
    block_input_request_pending_ = false;
    block_input_active_ = status.enabled;
    if (act_block_input_) {
        act_block_input_->setEnabled(true);
        act_block_input_->setChecked(status.enabled);
    }

    QString message;
    if (!status.error.isEmpty()) {
        if (status.error == "watch_only_session") {
            message = "Bloqueio de entrada indisponivel em sessao somente visualizacao.";
        } else if (status.error == "block_input_enable_failed") {
            message = "Host nao conseguiu bloquear a entrada local.";
        } else if (status.error == "block_input_disable_failed") {
            message = "Host nao conseguiu desbloquear a entrada local.";
        } else {
            message = QString("Falha ao atualizar bloqueio de entrada no host: %1").arg(status.error);
        }
    } else {
        message = status.enabled ? "Entrada bloqueada no host" : "Entrada desbloqueada";
    }

    statusBar()->showMessage(message, 5000);
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnDiagnosticsSummaryUpdated(const remotex::viewer::SessionDiagnosticsSummary& summary) {
    if (summary.host_capture_fps > 0.0) {
        diagnostics_summary_.host_capture_fps = summary.host_capture_fps;
    }
    if (summary.host_encoded_fps > 0.0) {
        diagnostics_summary_.host_encoded_fps = summary.host_encoded_fps;
    }
    if (summary.host_sent_fps > 0.0) {
        diagnostics_summary_.host_sent_fps = summary.host_sent_fps;
    }

    diagnostics_summary_.viewer_arrival_fps = summary.viewer_arrival_fps;
    diagnostics_summary_.viewer_decode_ready_fps = summary.viewer_decode_ready_fps;
    diagnostics_summary_.avg_transport_ms = summary.avg_transport_ms;

    if (summary.frame_age_ms > 0.0 &&
        (summary.frame_age_is_end_to_end || diagnostics_summary_.frame_age_ms <= 0.0)) {
        diagnostics_summary_.frame_age_ms = summary.frame_age_ms;
        diagnostics_summary_.frame_age_is_end_to_end = summary.frame_age_is_end_to_end;
    }

    UpdateDiagnosticsSummary();
}

void RemoteSessionWindow::OnRendererDiagnosticsUpdated(
    const remotex::viewer::RendererDiagnosticsSnapshot& snapshot) {
    diagnostics_summary_.viewer_render_fps = snapshot.submit_fps;
    diagnostics_summary_.viewer_upload_fps = snapshot.upload_fps;
    diagnostics_summary_.avg_decode_queue_ms = snapshot.avg_present_delay_ms;
    diagnostics_summary_.avg_render_upload_ms = snapshot.avg_upload_ms;
    diagnostics_summary_.viewer_dropped_fps = snapshot.viewer_dropped_fps;
    diagnostics_summary_.avg_present_delay_ms = snapshot.avg_present_delay_ms;

    if (snapshot.avg_frame_age_ms > 0.0 &&
        (snapshot.frame_age_is_end_to_end || diagnostics_summary_.frame_age_ms <= 0.0)) {
        diagnostics_summary_.frame_age_ms = snapshot.avg_frame_age_ms;
        diagnostics_summary_.frame_age_is_end_to_end = snapshot.frame_age_is_end_to_end;
    }

    UpdateDiagnosticsSummary();
}

void RemoteSessionWindow::UpdateDiagnosticsSummary() {
    diagnostics_summary_.bottleneck = ClassifySessionDiagnostics(diagnostics_summary_);
    UpdateDiagnosticsPanel();
}

void RemoteSessionWindow::UpdateDiagnosticsPanel() {
    if (diagnostics_window_) {
        diagnostics_window_->UpdateDiagnostics(diagnostics_summary_);
    }
}

void RemoteSessionWindow::OnToggleDiagnosticsWindow() {
    if (diagnostics_window_) {
        diagnostics_window_->close();
        // WA_DeleteOnClose + destroyed signal sets pointer to null
        return;
    }

    diagnostics_window_ = new DiagnosticsWindow(this);
    connect(diagnostics_window_, &QObject::destroyed, this, [this]() {
        diagnostics_window_ = nullptr;
    });
    diagnostics_window_->UpdateDiagnostics(diagnostics_summary_);
    diagnostics_window_->show();
    diagnostics_window_->raise();
    diagnostics_window_->activateWindow();
}

void RemoteSessionWindow::OnLatencyUpdated(int ms) {
    latency_label_->setText(QString("Latencia: %1ms").arg(ms));
    UpdateFullscreenOverlayTexts();
}

void RemoteSessionWindow::OnStatusUpdated(const QString& msg) {
    status_label_->setText(msg);
    UpdateFullscreenOverlayTexts();
    if (retry_timer_ && !received_video_frame_ && !msg.startsWith("Desconectado")) {
        retry_timer_->start();
        retry_btn_->hide();
    }
    ShowFullscreenOverlayTemporarily();
}

void RemoteSessionWindow::OnRetry() {
    retry_timer_->stop();
    if (video_wait_timer_) {
        video_wait_timer_->stop();
    }
    retry_btn_->hide();
    received_video_frame_ = false;
    status_label_->setText("Reconectando...");
    UpdateFullscreenOverlayTexts();
    ShowFullscreenOverlayTemporarily();

    QMetaObject::invokeMethod(network_worker_, "Disconnect", Qt::QueuedConnection);
    QMetaObject::invokeMethod(network_worker_, "ConnectToMachine",
                              Qt::QueuedConnection,
                              Q_ARG(QString, machine_id_),
                              Q_ARG(QString, connection_settings_.signaling_host),
                              Q_ARG(quint16, connection_settings_.signaling_port));
    retry_timer_->start();
}

void RemoteSessionWindow::OnMonitorListReceived(const QList<QVariantMap>& monitors) {
    available_monitors_ = monitors;
    monitor_combo_->blockSignals(true);
    monitor_combo_->clear();
    for (int i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        QString label = QString("Monitor %1 (%2x%3)")
            .arg(i + 1)
            .arg(m["width"].toInt())
            .arg(m["height"].toInt());
        if (m["primary"].toBool()) {
            label += " *";
        }
        monitor_combo_->addItem(label);
    }
    monitor_combo_->setEnabled(monitors.size() > 1);
    monitor_combo_->blockSignals(false);
}

void RemoteSessionWindow::OnMonitorSelected(int index) {
    if (index < 0 || index >= available_monitors_.size()) return;
    const auto& m = available_monitors_[index];
    QMetaObject::invokeMethod(network_worker_, "SelectMonitor",
        Qt::QueuedConnection,
        Q_ARG(int, m["adapter_index"].toInt()),
        Q_ARG(int, m["output_index"].toInt()));
}

void RemoteSessionWindow::OnMonitorChanged(const remotex::viewer::SessionDisplayContract& contract) {
    const auto renderer_state = renderer_
        ? renderer_->SnapshotFrameState()
        : remotex::viewer::RendererFrameState{};
    const bool requires_reset =
        renderer_ &&
        ShouldResetRendererForDisplayContractUpdate(
            current_display_contract_,
            renderer_state,
            contract);

    current_display_contract_ = contract;

    if (requires_reset) {
        renderer_->ResetFrameState();
        received_video_frame_ = false;
        if (video_wait_timer_) {
            video_wait_timer_->start();
        }
        QMetaObject::invokeMethod(network_worker_, "SendRefreshRequest", Qt::QueuedConnection);
        status_label_->setText(
            QString("Monitor alterado (%1x%2 rot=%3) - aguardando video...")
                .arg(contract.width)
                .arg(contract.height)
                .arg(contract.rotation));
    } else {
        status_label_->setText(
            QString("Monitor alterado (%1x%2 rot=%3)")
                .arg(contract.width)
                .arg(contract.height)
                .arg(contract.rotation));
    }
    UpdateFullscreenOverlayTexts();
    ShowFullscreenOverlayTemporarily();
}

