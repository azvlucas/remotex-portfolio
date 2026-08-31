void RemoteSessionWindow::SetupFullscreenOverlay() {
    fullscreen_overlay_ = new QFrame(renderer_);
    fullscreen_overlay_->setObjectName("fullscreenOverlay");
    fullscreen_overlay_->setFocusPolicy(Qt::NoFocus);
    fullscreen_overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    fullscreen_overlay_->setStyleSheet(
        "QFrame#fullscreenOverlay {"
        "background-color: rgba(12, 12, 12, 190);"
        "border: 1px solid rgba(255, 255, 255, 48);"
        "border-radius: 10px;"
        "}"
        "QLabel { color: white; }");

    auto* layout = new QVBoxLayout(fullscreen_overlay_);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);

    auto* title_label = new QLabel("Tela cheia", fullscreen_overlay_);
    title_label->setStyleSheet("font-weight: 600;");
    layout->addWidget(title_label);

    auto* hint_label = new QLabel("F11 para sair", fullscreen_overlay_);
    hint_label->setStyleSheet("color: rgba(255, 255, 255, 210);");
    layout->addWidget(hint_label);

    auto* info_row = new QHBoxLayout;
    info_row->setContentsMargins(0, 0, 0, 0);
    info_row->setSpacing(16);

    fullscreen_overlay_status_label_ = new QLabel(status_label_->text(), fullscreen_overlay_);
    fullscreen_overlay_latency_label_ = new QLabel(latency_label_->text(), fullscreen_overlay_);
    fullscreen_overlay_latency_label_->setStyleSheet("color: rgba(255, 255, 255, 210);");

    info_row->addWidget(fullscreen_overlay_status_label_, 1);
    info_row->addWidget(fullscreen_overlay_latency_label_, 0, Qt::AlignRight);
    layout->addLayout(info_row);

    fullscreen_overlay_timer_ = new QTimer(this);
    fullscreen_overlay_timer_->setSingleShot(true);
    fullscreen_overlay_timer_->setInterval(kFullscreenOverlayHideDelayMs);
    connect(fullscreen_overlay_timer_, &QTimer::timeout,
            this, &RemoteSessionWindow::HideFullscreenOverlay);

    UpdateFullscreenOverlayTexts();
    fullscreen_overlay_->hide();
}

void RemoteSessionWindow::SetupTransferToast() {
    transfer_toast_ = new QFrame(renderer_);
    transfer_toast_->setObjectName("transferToast");
    transfer_toast_->setFocusPolicy(Qt::NoFocus);
    transfer_toast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    transfer_toast_->setStyleSheet(
        "QFrame#transferToast {"
        "background-color: rgba(18, 18, 18, 220);"
        "border: 1px solid rgba(255, 255, 255, 52);"
        "border-radius: 12px;"
        "}"
        "QLabel#transferToastTitle {"
        "color: white;"
        "font-weight: 600;"
        "}"
        "QLabel#transferToastBody {"
        "color: rgba(255, 255, 255, 210);"
        "}"
        "QProgressBar {"
        "border: 1px solid rgba(255, 255, 255, 40);"
        "border-radius: 5px;"
        "background: rgba(255, 255, 255, 22);"
        "text-align: center;"
        "color: white;"
        "}"
        "QProgressBar::chunk {"
        "background-color: rgb(50, 160, 90);"
        "border-radius: 4px;"
        "}");

    auto* layout = new QVBoxLayout(transfer_toast_);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    transfer_toast_title_label_ = new QLabel(QStringLiteral("Transferencia de arquivos"), transfer_toast_);
    transfer_toast_title_label_->setObjectName(QStringLiteral("transferToastTitle"));
    layout->addWidget(transfer_toast_title_label_);

    transfer_toast_body_label_ = new QLabel(QStringLiteral("Aguardando transferencia..."), transfer_toast_);
    transfer_toast_body_label_->setObjectName(QStringLiteral("transferToastBody"));
    transfer_toast_body_label_->setWordWrap(true);
    layout->addWidget(transfer_toast_body_label_);

    transfer_toast_progress_bar_ = new QProgressBar(transfer_toast_);
    transfer_toast_progress_bar_->setRange(0, 100);
    transfer_toast_progress_bar_->setValue(0);
    layout->addWidget(transfer_toast_progress_bar_);

    transfer_toast_timer_ = new QTimer(this);
    transfer_toast_timer_->setSingleShot(true);
    transfer_toast_timer_->setInterval(kTransferToastHideDelayMs);
    connect(transfer_toast_timer_, &QTimer::timeout,
            this, &RemoteSessionWindow::HideTransferToast);

    transfer_toast_->hide();
}

void RemoteSessionWindow::UpdateFullscreenPresentation() {
    if (is_fullscreen_) {
        toolbar_->hide();
        statusBar()->hide();
        showFullScreen();
        if (act_fullscreen_) {
            act_fullscreen_->setText("Sair Tela Cheia");
        }
        UpdateFullscreenActionHints();
        ShowFullscreenOverlayTemporarily();
    } else {
        if (fullscreen_overlay_timer_) {
            fullscreen_overlay_timer_->stop();
        }
        HideFullscreenOverlay();
        showNormal();
        toolbar_->show();
        statusBar()->show();
        if (act_fullscreen_) {
            act_fullscreen_->setText("Tela Cheia");
        }
        UpdateFullscreenActionHints();
    }

    if (renderer_) {
        renderer_->setFocus(Qt::OtherFocusReason);
    }
}

void RemoteSessionWindow::UpdateFullscreenOverlayTexts() {
    if (!fullscreen_overlay_) {
        return;
    }
    if (fullscreen_overlay_status_label_ && status_label_) {
        fullscreen_overlay_status_label_->setText(status_label_->text());
    }
    if (fullscreen_overlay_latency_label_ && latency_label_) {
        fullscreen_overlay_latency_label_->setText(latency_label_->text());
    }
}

void RemoteSessionWindow::UpdateFullscreenOverlayGeometry() {
    if (!fullscreen_overlay_ || !renderer_) {
        return;
    }

    const int available_width = renderer_->width() - (kFullscreenOverlayMarginPx * 2);
    if (available_width <= 0) {
        return;
    }

    fullscreen_overlay_->setMaximumWidth(available_width);
    fullscreen_overlay_->adjustSize();

    QSize overlay_size = fullscreen_overlay_->sizeHint();
    overlay_size.setWidth(std::min(overlay_size.width(), available_width));
    fullscreen_overlay_->resize(overlay_size);
    fullscreen_overlay_->move(std::max(kFullscreenOverlayMarginPx,
                                       (renderer_->width() - overlay_size.width()) / 2),
                              kFullscreenOverlayMarginPx);
    fullscreen_overlay_->raise();
}

void RemoteSessionWindow::ShowFullscreenOverlayTemporarily() {
    if (!is_fullscreen_ || !fullscreen_overlay_) {
        return;
    }

    UpdateFullscreenOverlayTexts();
    UpdateFullscreenOverlayGeometry();
    fullscreen_overlay_->show();
    fullscreen_overlay_->raise();

    if (fullscreen_overlay_timer_) {
        fullscreen_overlay_timer_->start();
    }
}

void RemoteSessionWindow::HideFullscreenOverlay() {
    if (fullscreen_overlay_) {
        fullscreen_overlay_->hide();
    }
}

void RemoteSessionWindow::UpdateTransferToastGeometry() {
    if (!transfer_toast_ || !renderer_) {
        return;
    }

    const int available_width = renderer_->width() - (kTransferToastMarginPx * 2);
    if (available_width <= 0) {
        return;
    }

    transfer_toast_->setMaximumWidth(std::min(kTransferToastWidthPx, available_width));
    transfer_toast_->adjustSize();

    QSize toast_size = transfer_toast_->sizeHint();
    toast_size.setWidth(std::min(toast_size.width(), std::min(kTransferToastWidthPx, available_width)));
    transfer_toast_->resize(toast_size);
    transfer_toast_->move(std::max(kTransferToastMarginPx,
                                   renderer_->width() - toast_size.width() - kTransferToastMarginPx),
                          std::max(kTransferToastMarginPx,
                                   renderer_->height() - toast_size.height() - kTransferToastMarginPx));
    transfer_toast_->raise();
}

void RemoteSessionWindow::ShowTransferToastStarted(const QString& job_id,
                                                   const QString& destination_root,
                                                   quint64 total_bytes,
                                                   int total_entries,
                                                   bool receiving) {
    if (!transfer_toast_ || !transfer_toast_title_label_ || !transfer_toast_body_label_ ||
        !transfer_toast_progress_bar_) {
        return;
    }

    transfer_toast_job_id_ = job_id;
    if (transfer_toast_timer_) {
        transfer_toast_timer_->stop();
    }

    transfer_toast_title_label_->setText(receiving
        ? QStringLiteral("Recebendo arquivos")
        : QStringLiteral("Enviando arquivos"));
    double size_value = static_cast<double>(total_bytes);
    int size_unit_index = 0;
    static const char* size_units[] = {"B", "KB", "MB", "GB", "TB"};
    while (size_value >= 1024.0 && size_unit_index < 4) {
        size_value /= 1024.0;
        ++size_unit_index;
    }
    const QString formatted_size = QString::number(size_value,
                                                   size_unit_index == 0 ? 'f' : 'f',
                                                   size_unit_index == 0 ? 0 : 1) +
                                   QStringLiteral(" ") + QString::fromLatin1(size_units[size_unit_index]);
    transfer_toast_body_label_->setText(
        QStringLiteral("Destino: %1\n%2 item(ns) • %3")
            .arg(destination_root.isEmpty() ? QStringLiteral("-") : destination_root)
            .arg(total_entries)
            .arg(formatted_size));
    transfer_toast_progress_bar_->setRange(0, 100);
    transfer_toast_progress_bar_->setValue(0);
    transfer_toast_progress_bar_->setFormat(QStringLiteral("%p%"));
    transfer_toast_progress_bar_->setStyleSheet(
        QStringLiteral("QProgressBar::chunk { background-color: rgb(50, 160, 90); border-radius: 4px; }"));
    UpdateTransferToastGeometry();
    transfer_toast_->show();
    transfer_toast_->raise();
}

void RemoteSessionWindow::ShowTransferToastProgress(const QString& job_id,
                                                    const QString& current_item,
                                                    quint64 bytes_done,
                                                    quint64 total_bytes,
                                                    int entries_done,
                                                    int total_entries) {
    if (!transfer_toast_ || !transfer_toast_body_label_ || !transfer_toast_progress_bar_) {
        return;
    }

    if (!transfer_toast_job_id_.isEmpty() && transfer_toast_job_id_ != job_id) {
        return;
    }
    transfer_toast_job_id_ = job_id;
    if (transfer_toast_timer_) {
        transfer_toast_timer_->stop();
    }

    int percent = 0;
    if (total_bytes > 0) {
        percent = static_cast<int>((bytes_done * 100) / total_bytes);
    } else if (total_entries > 0) {
        percent = static_cast<int>((entries_done * 100) / total_entries);
    }

    transfer_toast_progress_bar_->setRange(0, 100);
    transfer_toast_progress_bar_->setValue(std::clamp(percent, 0, 100));
    transfer_toast_progress_bar_->setFormat(QStringLiteral("%p%"));
    transfer_toast_progress_bar_->setStyleSheet(
        QStringLiteral("QProgressBar::chunk { background-color: rgb(50, 160, 90); border-radius: 4px; }"));
    transfer_toast_body_label_->setText(
        QStringLiteral("%1\n%2/%3 item(ns)")
            .arg(current_item.trimmed().isEmpty() ? QStringLiteral("Preparando transferencia...") : current_item)
            .arg(entries_done)
            .arg(total_entries));
    UpdateTransferToastGeometry();
    transfer_toast_->show();
    transfer_toast_->raise();
}

void RemoteSessionWindow::ShowTransferToastFinished(const QString& job_id,
                                                    bool success,
                                                    const QString& destination_root,
                                                    const QString& message) {
    if (!transfer_toast_ || !transfer_toast_title_label_ || !transfer_toast_body_label_ ||
        !transfer_toast_progress_bar_) {
        return;
    }

    if (!transfer_toast_job_id_.isEmpty() && transfer_toast_job_id_ != job_id) {
        return;
    }
    transfer_toast_job_id_.clear();

    transfer_toast_title_label_->setText(success
        ? QStringLiteral("Transferencia concluida")
        : QStringLiteral("Falha na transferencia"));
    transfer_toast_body_label_->setText(success
        ? QStringLiteral("Destino: %1").arg(destination_root.isEmpty() ? QStringLiteral("-") : destination_root)
        : (message.trimmed().isEmpty()
              ? QStringLiteral("A transferencia falhou sem detalhes adicionais.")
              : message));
    transfer_toast_progress_bar_->setRange(0, 100);
    transfer_toast_progress_bar_->setValue(success ? 100 : transfer_toast_progress_bar_->value());
    transfer_toast_progress_bar_->setFormat(success ? QStringLiteral("100%") : QStringLiteral("Falha"));
    transfer_toast_progress_bar_->setStyleSheet(success
        ? QStringLiteral("QProgressBar::chunk { background-color: rgb(50, 160, 90); border-radius: 4px; }")
        : QStringLiteral("QProgressBar::chunk { background-color: rgb(196, 65, 65); border-radius: 4px; }"));
    UpdateTransferToastGeometry();
    transfer_toast_->show();
    transfer_toast_->raise();

    if (transfer_toast_timer_) {
        transfer_toast_timer_->start();
    }
}

void RemoteSessionWindow::HideTransferToast() {
    transfer_toast_job_id_.clear();
    if (transfer_toast_) {
        transfer_toast_->hide();
    }
}

void RemoteSessionWindow::OnToggleFullscreen() {
    is_fullscreen_ = !is_fullscreen_;
    if (act_fullscreen_) {
        act_fullscreen_->setIcon(style()->standardIcon(
            is_fullscreen_ ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
    }
    UpdateFullscreenActionHints();
    UpdateFullscreenPresentation();
}

bool RemoteSessionWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == renderer_) {
        switch (event->type()) {
            case QEvent::Resize:
                if (is_fullscreen_) {
                    UpdateFullscreenOverlayGeometry();
                }
                if (transfer_toast_ && transfer_toast_->isVisible()) {
                    UpdateTransferToastGeometry();
                }
                break;
            case QEvent::Enter:
            case QEvent::FocusIn:
            case QEvent::MouseMove:
            case QEvent::WindowActivate:
                ShowFullscreenOverlayTemporarily();
                break;
            default:
                break;
        }
    }

    return QObject::eventFilter(watched, event);
}

void RemoteSessionWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && input_capture_) {
        input_capture_->SetEnabled(input_capture_->IsEnabled());
    }
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        ShowFullscreenOverlayTemporarily();
    }
    QMainWindow::changeEvent(event);
}

void RemoteSessionWindow::closeEvent(QCloseEvent* event) {
    if (input_capture_) {
        input_capture_->SetEnabled(false);
    }
    if (fullscreen_overlay_timer_) {
        fullscreen_overlay_timer_->stop();
    }
    if (transfer_toast_timer_) {
        transfer_toast_timer_->stop();
    }
    HideTransferToast();
    if (diagnostics_window_) {
        diagnostics_window_->close();
        diagnostics_window_ = nullptr;
    }
    emit SessionEnded();
    QMainWindow::closeEvent(event);
}
