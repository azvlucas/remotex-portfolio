RemoteSessionWindow::RemoteSessionWindow(remotex::core::AuthClient* auth,
                                         const QString& machine_id,
                                         const QString& machine_name,
                                         remotex::core::SessionAccessMode access_mode,
                                         QWidget* parent)
    : QMainWindow(parent)
    , auth_(auth)
    , machine_id_(machine_id)
    , access_mode_(access_mode)
    , connection_settings_(ConnectionSettings::Load()) {
    qRegisterMetaType<remotex::viewer::VideoFramePacketPtr>("remotex::viewer::VideoFramePacketPtr");
    qRegisterMetaType<remotex::viewer::RemoteListingResult>("remotex::viewer::RemoteListingResult");
    qRegisterMetaType<QList<remotex::viewer::RemoteFileItem>>("QList<remotex::viewer::RemoteFileItem>");
    qRegisterMetaType<remotex::viewer::RemoteControlToggleStatus>("remotex::viewer::RemoteControlToggleStatus");
    qRegisterMetaType<remotex::viewer::SecureAttentionStatus>("remotex::viewer::SecureAttentionStatus");
    qRegisterMetaType<remotex::viewer::RendererDiagnosticsSnapshot>("remotex::viewer::RendererDiagnosticsSnapshot");
    qRegisterMetaType<remotex::viewer::SessionDiagnosticsSummary>("remotex::viewer::SessionDiagnosticsSummary");
    qRegisterMetaType<remotex::viewer::SessionDisplayContract>("remotex::viewer::SessionDisplayContract");

    setWindowTitle(QString("RemoteX - %1 (%2)")
                       .arg(machine_name, SessionModeWindowSuffix(access_mode_)));

    network_thread_ = new QThread(this);
    network_worker_ = new NetworkWorker(auth_, access_mode_, connection_settings_);
    network_worker_->moveToThread(network_thread_);

    SetupUI();
    SetupToolbar();

    fullscreen_shortcut_ = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreen_shortcut_->setContext(Qt::WindowShortcut);
    connect(fullscreen_shortcut_, &QShortcut::activated,
            this, &RemoteSessionWindow::OnToggleFullscreen);

    connect(network_worker_, &NetworkWorker::Connected,
            this, &RemoteSessionWindow::OnConnected);
    connect(network_worker_, &NetworkWorker::Disconnected,
            this, &RemoteSessionWindow::OnDisconnected);
    connect(network_worker_, &NetworkWorker::VideoFrameReceived,
            this, &RemoteSessionWindow::OnVideoFrame);
    connect(network_worker_, &NetworkWorker::LatencyUpdated,
            this, &RemoteSessionWindow::OnLatencyUpdated);
    connect(network_worker_, &NetworkWorker::StatusUpdated,
            this, &RemoteSessionWindow::OnStatusUpdated);
    connect(network_worker_, &NetworkWorker::DiagnosticsSummaryUpdated,
            this, &RemoteSessionWindow::OnDiagnosticsSummaryUpdated);
    connect(network_worker_, &NetworkWorker::MonitorListReceived,
            this, &RemoteSessionWindow::OnMonitorListReceived);
    connect(network_worker_, &NetworkWorker::MonitorChanged,
            this, &RemoteSessionWindow::OnMonitorChanged);
    connect(network_worker_, &NetworkWorker::RebootAcknowledged,
            this, &RemoteSessionWindow::OnRebootAcknowledged);
    connect(network_worker_, &NetworkWorker::RebootFailed,
            this, &RemoteSessionWindow::OnRebootFailed);
    connect(network_worker_, &NetworkWorker::SecureAttentionStatusChanged,
            this, &RemoteSessionWindow::OnSecureAttentionStatusChanged);
    connect(network_worker_, &NetworkWorker::PrivacyModeStatusChanged,
            this, &RemoteSessionWindow::OnPrivacyModeStatusChanged);
    connect(network_worker_, &NetworkWorker::BlockInputStatusChanged,
            this, &RemoteSessionWindow::OnBlockInputStatusChanged);
    connect(monitor_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RemoteSessionWindow::OnMonitorSelected);
    connect(network_worker_, &NetworkWorker::TransferJobStarted,
            this, [this](const QString& job_id, const QString& destination_root,
                         quint64 total_bytes, int total_entries, bool receiving) {
                ShowTransferToastStarted(job_id, destination_root, total_bytes, total_entries, receiving);
            });
    connect(network_worker_, &NetworkWorker::TransferJobProgress,
            this, [this](const QString& job_id, const QString& current_item,
                         quint64 bytes_done, quint64 total_bytes,
                         int entries_done, int total_entries) {
                ShowTransferToastProgress(job_id,
                                          current_item,
                                          bytes_done,
                                          total_bytes,
                                          entries_done,
                                          total_entries);
            });
    connect(network_worker_, &NetworkWorker::TransferJobFinished,
            this, [this](const QString& job_id, bool success, const QString& destination, const QString& message) {
                ShowTransferToastFinished(job_id, success, destination, message);
                if (success) {
                    statusBar()->showMessage(QString("Transferencia concluida em %1").arg(destination), 5000);
                } else {
                    statusBar()->showMessage(QString("Falha na transferencia: %1").arg(message), 7000);
                }
            });

    if (IsInteractiveSession()) {
        file_paste_shortcut_ = new QShortcut(QKeySequence::Paste, renderer_);
        file_paste_shortcut_->setContext(Qt::WidgetShortcut);
        file_paste_shortcut_->setEnabled(false);
        connect(file_paste_shortcut_, &QShortcut::activated,
                this, &RemoteSessionWindow::OnClipboardFilePasteShortcut);
        connect(network_worker_, &NetworkWorker::ClipboardFilePastePendingChanged,
                this, [this](bool pending) {
                    if (file_paste_shortcut_) {
                        file_paste_shortcut_->setEnabled(pending);
                    }
                });

        connect(network_worker_, &NetworkWorker::ClipboardReceived, this, [this](const QString& text) {
            if (auto* clipboard = QGuiApplication::clipboard()) {
                is_updating_clipboard_ = true;
                clipboard->setText(text);
                is_updating_clipboard_ = false;
            }
        });

        if (auto* clipboard = QGuiApplication::clipboard()) {
            connect(clipboard, &QClipboard::dataChanged, this, [this]() {
                if (is_updating_clipboard_ || !network_worker_) {
                    return;
                }
                if (auto* clip = QGuiApplication::clipboard()) {
                    const QMimeData* mime_data = clip->mimeData();
                    if (mime_data && mime_data->hasUrls()) {
                        QStringList local_paths;
                        for (const QUrl& url : mime_data->urls()) {
                            if (url.isLocalFile()) {
                                const QString local_path = QDir::cleanPath(url.toLocalFile());
                                if (!local_path.trimmed().isEmpty() && !local_paths.contains(local_path)) {
                                    local_paths.push_back(local_path);
                                }
                            }
                        }
                        if (!local_paths.isEmpty()) {
                            QMetaObject::invokeMethod(network_worker_, "PublishLocalClipboardFiles",
                                                      Qt::QueuedConnection, Q_ARG(QStringList, local_paths));
                            return;
                        }
                    }

                    const QString text = clip->text();
                    if (!text.isEmpty()) {
                        QMetaObject::invokeMethod(network_worker_, "SendClipboardText",
                                                  Qt::QueuedConnection, Q_ARG(QString, text));
                    }
                }
            });
        }
    }

    chat_panel_ = new ChatPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, chat_panel_);
    chat_panel_->hide();

    connect(chat_panel_, &ChatPanel::MessageSent, this, [this](const QString& text) {
        QMetaObject::invokeMethod(network_worker_, "SendChatMessage",
                                  Qt::QueuedConnection, Q_ARG(QString, text));
    });
    connect(network_worker_, &NetworkWorker::ChatMessageReceived,
            chat_panel_, &ChatPanel::OnMessageReceived);
    connect(network_worker_, &NetworkWorker::Connected, chat_panel_, [this]() {
        chat_panel_->OnConnectionStateChanged(true);
    });
    connect(network_worker_, &NetworkWorker::Disconnected, chat_panel_, [this](const QString&) {
        chat_panel_->OnConnectionStateChanged(false);
    });

    network_thread_->start();

    retry_btn_ = new QPushButton("Tentar Novamente");
    retry_btn_->hide();
    statusBar()->addPermanentWidget(retry_btn_);
    connect(retry_btn_, &QPushButton::clicked, this, &RemoteSessionWindow::OnRetry);

    retry_timer_ = new QTimer(this);
    retry_timer_->setSingleShot(true);
    retry_timer_->setInterval(20000);
    connect(retry_timer_, &QTimer::timeout, this, [this]() {
        if (network_worker_ && network_worker_->IsRebootReconnectMode()) {
            return;
        }
        retry_btn_->show();
        status_label_->setText("Tempo esgotado - host indisponivel");
        UpdateFullscreenOverlayTexts();
        ShowFullscreenOverlayTemporarily();
    });

    video_wait_timer_ = new QTimer(this);
    video_wait_timer_->setSingleShot(true);
    video_wait_timer_->setInterval(5000);
    connect(video_wait_timer_, &QTimer::timeout, this, [this]() {
        if (!received_video_frame_) {
            status_label_->setText("Host conectado, aguardando video...");
            UpdateFullscreenOverlayTexts();
            ShowFullscreenOverlayTemporarily();
        }
    });

    QMetaObject::invokeMethod(network_worker_, "ConnectToMachine",
                              Qt::QueuedConnection,
                              Q_ARG(QString, machine_id),
                              Q_ARG(QString, connection_settings_.signaling_host),
                              Q_ARG(quint16, connection_settings_.signaling_port));

    status_label_->setText("Conectando...");
    UpdateFullscreenOverlayTexts();
    retry_timer_->start();
}

bool RemoteSessionWindow::IsInteractiveSession() const {
    return access_mode_ == remotex::core::SessionAccessMode::Interactive;
}

RemoteSessionWindow::~RemoteSessionWindow() {
    if (transfer_dialog_) {
        transfer_dialog_->close();
        transfer_dialog_ = nullptr;
    }

    if (network_worker_) {
        QMetaObject::invokeMethod(network_worker_, "Disconnect", Qt::QueuedConnection);
        QMetaObject::invokeMethod(network_thread_, "quit", Qt::QueuedConnection);
    } else if (network_thread_) {
        network_thread_->quit();
    }

    if (network_thread_) {
        if (!network_thread_->wait(2000)) {
            network_thread_->terminate();
            network_thread_->wait(500);
        }
    }

    delete network_worker_;
    network_worker_ = nullptr;
}

void RemoteSessionWindow::SetupUI() {
    resize(1280, 760);

    renderer_ = new VideoRenderer;
    setCentralWidget(renderer_);
    connect(renderer_, &VideoRenderer::DiagnosticsUpdated,
            this, &RemoteSessionWindow::OnRendererDiagnosticsUpdated);

    input_capture_ = new InputCapture(network_worker_ ? network_worker_->Peer() : nullptr,
                                      renderer_, this);
    input_capture_->SetEnabled(false);
    connect(input_capture_, &InputCapture::NumLockStateChanged,
            this, &RemoteSessionWindow::OnLocalNumLockStateChanged);

    latency_label_ = new QLabel("Latencia: --");
    status_label_ = new QLabel("Desconectado");
    statusBar()->addWidget(status_label_);
    statusBar()->addPermanentWidget(latency_label_);

    diagnostics_shortcut_ = new QShortcut(QKeySequence("Ctrl+Shift+T"), this);
    diagnostics_shortcut_->setContext(Qt::WindowShortcut);
    connect(diagnostics_shortcut_, &QShortcut::activated,
            this, &RemoteSessionWindow::OnToggleDiagnosticsWindow);

    SetupFullscreenOverlay();
    SetupTransferToast();
    renderer_->installEventFilter(this);
}
