// RemoteX - viewer/file_transfer_dialog_transfer.inc

FileTransferDialog::DispatchHooks FileTransferDialog::BuildDispatchHooks(NetworkWorker* worker) {
    DispatchHooks hooks;
    if (!worker) {
        return hooks;
    }

    QPointer<NetworkWorker> worker_guard(worker);
    hooks.request_remote_roots = [worker_guard]() {
        if (!worker_guard) {
            return;
        }
        QMetaObject::invokeMethod(worker_guard, "RequestRemoteRoots", Qt::QueuedConnection);
    };
    hooks.request_remote_listing = [worker_guard](const QString& path) {
        if (!worker_guard) {
            return;
        }
        QMetaObject::invokeMethod(worker_guard, "RequestRemoteListing",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, path));
    };
    hooks.send_paths = [worker_guard](const QStringList& source_paths, const QString& destination_root) {
        if (!worker_guard) {
            return;
        }
        QMetaObject::invokeMethod(worker_guard, "SendPaths",
                                  Qt::QueuedConnection,
                                  Q_ARG(QStringList, source_paths),
                                  Q_ARG(QString, destination_root));
    };
    hooks.receive_paths = [worker_guard](const QStringList& source_paths, const QString& destination_root) {
        if (!worker_guard) {
            return;
        }
        QMetaObject::invokeMethod(worker_guard, "ReceivePaths",
                                  Qt::QueuedConnection,
                                  Q_ARG(QStringList, source_paths),
                                  Q_ARG(QString, destination_root));
    };
    hooks.resolve_conflict = [worker_guard](const QString& job_id, bool allow_overwrite) {
        if (!worker_guard) {
            return;
        }
        QMetaObject::invokeMethod(worker_guard, "ResolveTransferPreview",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, job_id),
                                  Q_ARG(bool, allow_overwrite));
    };

    return hooks;
}

FileTransferDialog::FileTransferDialog(NetworkWorker* worker, QWidget* parent)
    : FileTransferDialog(worker, BuildDispatchHooks(worker), parent) {
}

FileTransferDialog::FileTransferDialog(NetworkWorker* worker,
                                       DispatchHooks dispatch_hooks,
                                       QWidget* parent)
    : QDialog(parent)
    , worker_(worker)
    , dispatch_hooks_(std::move(dispatch_hooks)) {
    qRegisterMetaType<remotex::viewer::RemoteFileItem>("remotex::viewer::RemoteFileItem");
    qRegisterMetaType<QList<remotex::viewer::RemoteFileItem>>("QList<remotex::viewer::RemoteFileItem>");
    qRegisterMetaType<remotex::viewer::RemoteListingResult>("remotex::viewer::RemoteListingResult");

    SetupUi();

    if (worker_) {
        QObject::connect(worker_, SIGNAL(RemoteListingReady(remotex::viewer::RemoteListingResult)),
                         this, SLOT(ApplyRemoteListing(remotex::viewer::RemoteListingResult)));
        QObject::connect(worker_, SIGNAL(TransferConflictPreviewReady(QString,bool,QString)),
                         this, SLOT(ApplyTransferConflictPreview(QString,bool,QString)));
        QObject::connect(worker_, SIGNAL(TransferConflictPreviewFailed(QString,QString,QString)),
                         this, SLOT(ApplyTransferConflictFailure(QString,QString,QString)));
        QObject::connect(worker_, SIGNAL(TransferJobStarted(QString,QString,quint64,int,bool)),
                         this, SLOT(ApplyTransferStarted(QString,QString,quint64,int,bool)));
        QObject::connect(worker_, SIGNAL(TransferJobProgress(QString,QString,quint64,quint64,int,int)),
                         this, SLOT(ApplyTransferProgress(QString,QString,quint64,quint64,int,int)));
        QObject::connect(worker_, SIGNAL(TransferJobFinished(QString,bool,QString,QString)),
                         this, SLOT(ApplyTransferFinished(QString,bool,QString,QString)));
    }

    SetLocalRoot(QDir::homePath());
    RequestRemoteRoots();
    UpdateActionState();
}

FileTransferDialog::~FileTransferDialog() {
    CancelPendingConflictPreview();
}

void FileTransferDialog::closeEvent(QCloseEvent* event) {
    CancelPendingConflictPreview();
    QDialog::closeEvent(event);
}

void FileTransferDialog::BeginConflictPreviewRequest(const QString& status_text) {
    pending_conflict_decision_ = true;
    pending_conflict_job_id_.clear();
    approved_conflict_job_id_.clear();
    progress_bar_->setRange(0, 0);
    progress_label_->setText(status_text);
    UpdateActionState();
}

void FileTransferDialog::ClearConflictPreviewState() {
    pending_conflict_decision_ = false;
    pending_conflict_job_id_.clear();
    pending_conflict_receiving_ = false;
    prompt_open_ = false;
    progress_bar_->setRange(0, 100);
    UpdateActionState();
}

void FileTransferDialog::ResolveConflictPreview(bool allow_overwrite, const QString& status_text) {
    if (!dispatch_hooks_.resolve_conflict || pending_conflict_job_id_.trimmed().isEmpty()) {
        return;
    }

    const QString job_id = pending_conflict_job_id_;
    pending_conflict_job_id_.clear();
    progress_label_->setText(status_text);
    dispatch_hooks_.resolve_conflict(job_id, allow_overwrite);
}

void FileTransferDialog::CancelPendingConflictPreview() {
    if (dispatch_hooks_.resolve_conflict && !pending_conflict_job_id_.trimmed().isEmpty()) {
        dispatch_hooks_.resolve_conflict(pending_conflict_job_id_, false);
    }
    pending_conflict_job_id_.clear();
    pending_conflict_decision_ = false;
    prompt_open_ = false;
}

std::optional<bool> FileTransferDialog::PromptConflictDecision(const QString& title, const QString& message) {
    if (dispatch_hooks_.prompt_conflict_decision) {
        return dispatch_hooks_.prompt_conflict_decision(title, message);
    }

    QMessageBox prompt(this);
    prompt.setObjectName(QStringLiteral("file_transfer_conflict_prompt"));
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowTitle(title);
    prompt.setText(message);
    auto* overwrite_button = prompt.addButton(QStringLiteral("Sobrescrever"), QMessageBox::AcceptRole);
    auto* cancel_button = prompt.addButton(QStringLiteral("Cancelar"), QMessageBox::RejectRole);
    prompt.setDefaultButton(qobject_cast<QPushButton*>(cancel_button));
    prompt.exec();

    if (prompt.clickedButton() == overwrite_button) {
        return true;
    }
    if (prompt.clickedButton() == cancel_button) {
        return false;
    }
    return std::nullopt;
}

QString FileTransferDialog::DescribeTransferFailure(const QString& message, bool approved_conflict) const {
    const QString trimmed_message = message.trimmed();
    if (trimmed_message.startsWith(QStringLiteral("late-conflict:"))) {
        const QString detail = trimmed_message.section(QLatin1Char(':'), 1).trimmed();
        return approved_conflict
            ? QStringLiteral("Sobrescrita aprovada, mas houve conflito tardio: %1").arg(detail)
            : QStringLiteral("Transferencia interrompida por conflito tardio: %1").arg(detail);
    }

    if (trimmed_message.isEmpty()) {
        return approved_conflict
            ? QStringLiteral("Sobrescrita aprovada, mas a transferencia falhou sem detalhe adicional.")
            : QStringLiteral("Transferencia falhou sem detalhe adicional.");
    }

    if (approved_conflict) {
        return QStringLiteral("Sobrescrita aprovada, mas a transferencia falhou: %1").arg(trimmed_message);
    }

    return QStringLiteral("Transferencia falhou: %1").arg(trimmed_message);
}

bool FileTransferDialog::StartSendTransfer(const QStringList& source_paths,
                                           const QString& remote_destination_root,
                                           const QString& status_text) {
    if (pending_conflict_decision_ || source_paths.isEmpty() || remote_destination_root.trimmed().isEmpty() ||
        !dispatch_hooks_.send_paths || !dispatch_hooks_.resolve_conflict) {
        return false;
    }

    BeginConflictPreviewRequest(status_text);
    dispatch_hooks_.send_paths(source_paths, remote_destination_root);
    return true;
}

bool FileTransferDialog::StartReceiveTransfer(const QStringList& remote_paths,
                                              const QString& local_destination_root,
                                              const QString& status_text) {
    if (pending_conflict_decision_ || remote_paths.isEmpty() || local_destination_root.trimmed().isEmpty() ||
        !dispatch_hooks_.receive_paths || !dispatch_hooks_.resolve_conflict) {
        return false;
    }

    BeginConflictPreviewRequest(status_text);
    dispatch_hooks_.receive_paths(remote_paths, local_destination_root);
    return true;
}

void FileTransferDialog::UpdateActionState() {
    const bool has_local_selection = !SelectedLocalPaths().isEmpty();
    const bool has_remote_selection = !SelectedRemotePaths().isEmpty();
    const bool has_local_tree = !CurrentLocalTreeTransferPaths().isEmpty();
    const bool has_remote_tree = !CurrentRemoteTreeTransferPaths().isEmpty();
    const bool has_remote_target = !remote_at_roots_ && !remote_current_path_.isEmpty();
    const bool has_local_target = !local_at_roots_ && !local_current_path_.isEmpty();
    const bool can_request_remote = static_cast<bool>(dispatch_hooks_.request_remote_roots) ||
                                    static_cast<bool>(dispatch_hooks_.request_remote_listing);
    const bool can_preview_send = static_cast<bool>(dispatch_hooks_.send_paths) &&
                                  static_cast<bool>(dispatch_hooks_.resolve_conflict);
    const bool can_preview_receive = static_cast<bool>(dispatch_hooks_.receive_paths) &&
                                     static_cast<bool>(dispatch_hooks_.resolve_conflict);
    const bool transfer_controls_enabled = !pending_conflict_decision_;

    local_up_btn_->setEnabled(!local_at_roots_);
    local_refresh_btn_->setEnabled(local_model_ != nullptr);
    remote_up_btn_->setEnabled(can_request_remote && !remote_at_roots_);
    remote_refresh_btn_->setEnabled(can_request_remote);
    send_btn_->setEnabled(can_preview_send && transfer_controls_enabled && has_local_selection && has_remote_target);
    send_tree_btn_->setEnabled(can_preview_send && transfer_controls_enabled && has_local_tree && has_remote_target);
    receive_btn_->setEnabled(can_preview_receive && transfer_controls_enabled && has_remote_selection && has_local_target);
    receive_tree_btn_->setEnabled(can_preview_receive && transfer_controls_enabled && has_remote_tree && has_local_target);
}

void FileTransferDialog::SendSelection() {
    StartSendTransfer(SelectedLocalPaths(),
                      remote_current_path_,
                      QStringLiteral("Verificando conflitos para o envio..."));
}

void FileTransferDialog::SendCurrentTree() {
    StartSendTransfer(CurrentLocalTreeTransferPaths(),
                      remote_current_path_,
                      QStringLiteral("Verificando conflitos para o envio da arvore atual..."));
}

void FileTransferDialog::ReceiveSelection() {
    StartReceiveTransfer(SelectedRemotePaths(),
                         CurrentLocalDirectory(),
                         QStringLiteral("Verificando conflitos para o recebimento..."));
}

void FileTransferDialog::ReceiveCurrentTree() {
    StartReceiveTransfer(CurrentRemoteTreeTransferPaths(),
                         CurrentLocalDirectory(),
                         QStringLiteral("Verificando conflitos para o recebimento da arvore atual..."));
}

void FileTransferDialog::ApplyTransferConflictPreview(const QString& job_id,
                                                      bool receiving,
                                                      const QString& summary) {
    pending_conflict_decision_ = true;
    pending_conflict_receiving_ = receiving;
    pending_conflict_job_id_ = job_id;
    prompt_open_ = true;
    progress_bar_->setRange(0, 0);
    progress_label_->setText(receiving
        ? QStringLiteral("Conflitos detectados no recebimento. Aguardando confirmacao...")
        : QStringLiteral("Conflitos detectados no envio. Aguardando confirmacao..."));
    UpdateActionState();

    if (!dispatch_hooks_.resolve_conflict || job_id.trimmed().isEmpty() || summary.trimmed().isEmpty()) {
        prompt_open_ = false;
        progress_label_->setText(QStringLiteral("Transferencia cancelada: resumo de conflitos invalido."));
        if (dispatch_hooks_.resolve_conflict && !job_id.trimmed().isEmpty()) {
            ResolveConflictPreview(false, QStringLiteral("Resumo de conflitos invalido. Cancelando transferencia..."));
        } else {
            ClearConflictPreviewState();
        }
        return;
    }

    const auto decision = PromptConflictDecision(
        receiving
            ? QStringLiteral("Confirmar sobrescrita no recebimento")
            : QStringLiteral("Confirmar sobrescrita no envio"),
        summary);
    prompt_open_ = false;

    if (!decision.has_value()) {
        approved_conflict_job_id_.clear();
        ResolveConflictPreview(false,
                               QStringLiteral("Janela fechada sem decidir a sobrescrita. Cancelando transferencia..."));
        return;
    }

    if (*decision) {
        approved_conflict_job_id_ = job_id;
        ResolveConflictPreview(true,
                               QStringLiteral("Sobrescrita aprovada. Iniciando transferencia..."));
        return;
    }

    approved_conflict_job_id_.clear();
    ResolveConflictPreview(false,
                           QStringLiteral("Sobrescrita recusada. Cancelando transferencia..."));
}

void FileTransferDialog::ApplyTransferConflictFailure(const QString& job_id,
                                                      const QString& outcome,
                                                      const QString& message) {
    Q_UNUSED(outcome);

    if (active_job_id_ == job_id) {
        active_job_id_.clear();
    }
    if (approved_conflict_job_id_ == job_id) {
        approved_conflict_job_id_.clear();
    }
    ClearConflictPreviewState();
    progress_bar_->setValue(0);
    progress_label_->setText(message.trimmed().isEmpty()
        ? QStringLiteral("Transferencia cancelada durante a validacao de conflitos.")
        : message);
}

void FileTransferDialog::ApplyTransferStarted(const QString& job_id, const QString& destination_root,
                                              quint64 total_bytes, int total_entries, bool receiving) {
    if (IsClipboardStageDestination(destination_root)) {
        ignored_job_ids_.insert(job_id);
        return;
    }

    const bool approved_conflict = approved_conflict_job_id_ == job_id;
    active_job_id_ = job_id;
    ClearConflictPreviewState();
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_label_->setText(
        QString("%1 iniciado%2: %3 (%4 itens)")
            .arg(receiving ? "Recebimento" : "Envio")
            .arg(approved_conflict ? QStringLiteral(" com sobrescrita aprovada") : QString())
            .arg(destination_root.isEmpty() ? "-" : destination_root)
            .arg(total_entries));
    Q_UNUSED(total_bytes);
}

void FileTransferDialog::ApplyTransferProgress(const QString& job_id, const QString& current_item,
                                               quint64 bytes_done, quint64 total_bytes,
                                               int entries_done, int total_entries) {
    if (ignored_job_ids_.contains(job_id)) {
        return;
    }

    active_job_id_ = job_id;
    progress_bar_->setRange(0, 100);
    int percent = 0;
    if (total_bytes > 0) {
        percent = static_cast<int>((bytes_done * 100) / total_bytes);
    }
    progress_bar_->setValue(percent);
    progress_label_->setText(
        QString("%1 (%2/%3 itens)")
            .arg(current_item)
            .arg(entries_done)
            .arg(total_entries));
}

void FileTransferDialog::ApplyTransferFinished(const QString& job_id, bool success,
                                               const QString& destination_root, const QString& message) {
    if (ignored_job_ids_.contains(job_id)) {
        ignored_job_ids_.remove(job_id);
        return;
    }

    const bool approved_conflict = approved_conflict_job_id_ == job_id;
    if (approved_conflict) {
        approved_conflict_job_id_.clear();
    }
    if (active_job_id_ == job_id) {
        active_job_id_.clear();
    }
    ClearConflictPreviewState();
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(success ? 100 : progress_bar_->value());
    progress_label_->setText(success
        ? QString("Transferencia concluida em %1").arg(destination_root)
        : DescribeTransferFailure(message, approved_conflict));
    RefreshLocal();
    RefreshRemote();
}

