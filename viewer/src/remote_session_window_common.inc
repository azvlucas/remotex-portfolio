namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kWaitHostTimeoutMs = 8000;
constexpr int kNegotiationTimeoutMs = 10000;
constexpr int kWaitingConsentTimeoutMs = 40000;
constexpr int kWaitingVideoTimeoutMs = 12000;
constexpr int kMaxAutoRecoveryAttempts = 4;
constexpr int kMaxRebootRecoveryAttempts = 30;
constexpr int kFullscreenOverlayHideDelayMs = 2500;
constexpr int kFullscreenOverlayMarginPx = 16;
constexpr int kTransferToastHideDelayMs = 5000;
constexpr int kTransferToastMarginPx = 16;
constexpr int kTransferToastWidthPx = 360;

std::string TransferDirectionToAuditDirection(remotex::core::TransferDirection direction) {
    return direction == remotex::core::TransferDirection::Receive ? "download" : "upload";
}

std::string TransferSummary(const std::string& event_type,
                            remotex::core::TransferDirection direction) {
    const bool download = direction == remotex::core::TransferDirection::Receive;
    if (event_type == "file_transfer_failed") {
        return download ? "Falha ao receber arquivos" : "Falha ao enviar arquivos";
    }
    if (event_type == "file_upload_started") {
        return "Envio de arquivos iniciado";
    }
    if (event_type == "file_upload_finished") {
        return "Envio de arquivos concluido";
    }
    if (event_type == "file_download_started") {
        return "Recebimento de arquivos iniciado";
    }
    if (event_type == "file_download_finished") {
        return "Recebimento de arquivos concluido";
    }
    return download ? "Transferencia de arquivos" : "Envio de arquivos";
}

QString SessionModeWindowSuffix(remotex::core::SessionAccessMode access_mode) {
    return access_mode == remotex::core::SessionAccessMode::WatchOnly
        ? "Somente visualizacao"
        : "Controle remoto";
}

} // namespace

NetworkWorker::NetworkWorker(remotex::core::AuthClient* auth,
                             remotex::core::SessionAccessMode access_mode,
                             const remotex::viewer::ConnectionSettings& connection_settings,
                             QObject* parent)
    : QObject(parent)
    , auth_(auth)
    , access_mode_(access_mode)
    , connection_settings_(connection_settings) {
    signaling_.SetListener(this);
    peer_.SetListener(this);
    file_transfer_.SetListener(this);
    machine_client_.Configure(connection_settings_.ToMachineClientConfig(), auth_);
}

NetworkWorker::~NetworkWorker() {
    Disconnect();
}

bool NetworkWorker::IsInteractiveSession() const {
    return access_mode_ == remotex::core::SessionAccessMode::Interactive;
}

QStringList NetworkWorker::TopLevelClipboardEntryNames(const QStringList& paths) {
    QStringList entry_names;
    for (const QString& path : paths) {
        const QString cleaned_path = QDir::cleanPath(path);
        if (cleaned_path.trimmed().isEmpty()) {
            continue;
        }

        const QFileInfo info(cleaned_path);
        const QString entry_name = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
        if (!entry_name.trimmed().isEmpty() && !entry_names.contains(entry_name)) {
            entry_names.push_back(entry_name);
        }
    }
    return entry_names;
}

void NetworkWorker::CancelClipboardFileOffers(const QString& reason) {
    std::vector<std::string> tokens;
    tokens.reserve(pending_outgoing_clipboard_offers_.size() + pending_incoming_clipboard_offers_.size());

    for (const auto& [token, _] : pending_outgoing_clipboard_offers_) {
        tokens.push_back(token);
    }
    for (const auto& [token, _] : pending_incoming_clipboard_offers_) {
        if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
            tokens.push_back(token);
        }
    }

    pending_outgoing_clipboard_offers_.clear();
    pending_incoming_clipboard_offers_.clear();
    UpdateClipboardFilePasteAvailability();

    if (!peer_session_active_.load()) {
        return;
    }

    for (const std::string& token : tokens) {
        json payload;
        payload["type"] = "clipboard_files_cancel";
        payload["token"] = token;
        if (!reason.trimmed().isEmpty()) {
            payload["error"] = reason.toStdString();
        }
        peer_.SendDataChannelMessage("control", payload.dump());
    }
}

void NetworkWorker::UpdateClipboardFilePasteAvailability() {
    const bool has_pending_outgoing = std::any_of(
        pending_outgoing_clipboard_offers_.begin(),
        pending_outgoing_clipboard_offers_.end(),
        [](const auto& item) { return !item.second.paste_requested; });
    const bool has_pending_incoming = !pending_incoming_clipboard_offers_.empty();
    emit ClipboardFilePastePendingChanged(has_pending_outgoing || has_pending_incoming);
}

QString NetworkWorker::GenerateClipboardTransferToken() {
    return QStringLiteral("viewer-%1-%2")
        .arg(session_attempt_id_)
        .arg(++clipboard_transfer_counter_);
}

void NetworkWorker::HandleClipboardFilesOffer(const std::string& raw_json) {
    if (!IsInteractiveSession()) {
        return;
    }

    QString token;
    QStringList entry_names;
    try {
        const json payload = json::parse(raw_json);
        token = QString::fromStdString(payload.value("token", std::string{}));
        if (payload.contains("entry_names") && payload["entry_names"].is_array()) {
            for (const auto& entry : payload["entry_names"]) {
                if (!entry.is_string()) {
                    continue;
                }
                const QString entry_name = QString::fromStdString(entry.get<std::string>()).trimmed();
                if (!entry_name.isEmpty() && !entry_names.contains(entry_name)) {
                    entry_names.push_back(entry_name);
                }
            }
        }
    } catch (const std::exception& e) {
        qWarning().noquote() << QString("[viewer][attempt=%1] failed to parse clipboard_files_offer: %2")
                                    .arg(session_attempt_id_)
                                    .arg(e.what());
        return;
    }

    if (token.trimmed().isEmpty() || entry_names.isEmpty()) {
        return;
    }

    CancelClipboardFileOffers(QStringLiteral("new remote file clipboard offer superseded the previous session clipboard offer"));

    ClipboardFileOffer offer;
    offer.token = token;
    offer.entry_names = entry_names;
    pending_incoming_clipboard_offers_[token.toStdString()] = offer;
    UpdateClipboardFilePasteAvailability();
    emit StatusUpdated(QStringLiteral("Arquivos copiados no computador remoto. Pressione Ctrl+V na sessao para transferir."));
}

void NetworkWorker::HandleClipboardFilesAccept(const std::string& raw_json) {
    QString token;
    QString destination_root;
    bool used_desktop_fallback = false;
    try {
        const json payload = json::parse(raw_json);
        token = QString::fromStdString(payload.value("token", std::string{}));
        destination_root = QString::fromStdString(payload.value("destination_root", std::string{}));
        used_desktop_fallback = payload.value("used_desktop_fallback", false);
    } catch (const std::exception& e) {
        qWarning().noquote() << QString("[viewer][attempt=%1] failed to parse clipboard_files_accept: %2")
                                    .arg(session_attempt_id_)
                                    .arg(e.what());
        return;
    }

    auto it = pending_outgoing_clipboard_offers_.find(token.toStdString());
    if (it == pending_outgoing_clipboard_offers_.end()) {
        return;
    }

    std::vector<fs::path> source_paths;
    for (const QString& path : it->second.source_paths) {
        if (!path.trimmed().isEmpty()) {
            source_paths.emplace_back(path.toStdWString());
        }
    }

    if (source_paths.empty() || destination_root.trimmed().isEmpty()) {
        pending_outgoing_clipboard_offers_.erase(it);
        UpdateClipboardFilePasteAvailability();
        emit StatusUpdated(QStringLiteral("Falha ao preparar a transferencia de clipboard de arquivos."));
        return;
    }

    const std::string job_id = file_transfer_.SendLocalPaths(
        source_paths,
        fs::path(destination_root.toStdWString()),
        remotex::core::TransferOverwritePolicy::Allow);
    pending_outgoing_clipboard_offers_.erase(it);
    UpdateClipboardFilePasteAvailability();

    if (job_id.empty()) {
        json payload;
        payload["type"] = "clipboard_files_cancel";
        payload["token"] = token.toStdString();
        payload["error"] = "viewer clipboard send failed";
        peer_.SendDataChannelMessage("control", payload.dump());
        emit StatusUpdated(QStringLiteral("Falha ao iniciar a transferencia de arquivos para o host."));
        return;
    }

    emit StatusUpdated(used_desktop_fallback
        ? QStringLiteral("Enviando arquivos para a Area de Trabalho remota...")
        : QStringLiteral("Enviando arquivos para o computador remoto..."));
}

void NetworkWorker::HandleClipboardFilesCancel(const std::string& raw_json) {
    QString token;
    QString error;
    try {
        const json payload = json::parse(raw_json);
        token = QString::fromStdString(payload.value("token", std::string{}));
        error = QString::fromStdString(payload.value("error", std::string{}));
    } catch (const std::exception& e) {
        qWarning().noquote() << QString("[viewer][attempt=%1] failed to parse clipboard_files_cancel: %2")
                                    .arg(session_attempt_id_)
                                    .arg(e.what());
        return;
    }

    if (token.trimmed().isEmpty()) {
        return;
    }

    pending_outgoing_clipboard_offers_.erase(token.toStdString());
    pending_incoming_clipboard_offers_.erase(token.toStdString());
    UpdateClipboardFilePasteAvailability();
    if (!error.trimmed().isEmpty()) {
        emit StatusUpdated(error);
    }
}

void NetworkWorker::RecordTransferAuditEvent(const std::string& event_type,
                                             const remotex::core::TransferJobInfo& info,
                                             const std::string& status,
                                             const std::string& error) {
    if (machine_id_.empty() || backend_session_id_.empty()) {
        return;
    }

    remotex::core::MachineAuditEventRequest request;
    request.session_id = backend_session_id_;
    request.event_type = event_type;
    request.status = status;
    request.summary = TransferSummary(event_type, info.direction);
    request.transfer_direction = TransferDirectionToAuditDirection(info.direction);
    request.transfer_root = info.destination_root;
    request.transfer_item = info.current_item;
    request.transfer_total_bytes = info.total_bytes;
    request.transfer_total_entries = info.total_entries;
    request.error_message = error;

    machine_client_.CreateMachineAuditEvent(machine_id_, request);
}

void NetworkWorker::RequestRemoteRoots() {
    if (!IsInteractiveSession()) {
        return;
    }
    file_transfer_.RequestRemoteRoots();
}

void NetworkWorker::RequestRemoteListing(const QString& remote_path) {
    if (!IsInteractiveSession()) {
        return;
    }
    if (remote_path.isEmpty()) {
        file_transfer_.RequestRemoteRoots();
    } else {
        file_transfer_.RequestRemoteListing(fs::path(remote_path.toStdWString()));
    }
}

void NetworkWorker::SendPaths(const QStringList& local_paths, const QString& remote_destination_root) {
    if (!IsInteractiveSession()) {
        return;
    }
    std::vector<fs::path> paths;
    for (const auto& path : local_paths) {
        if (!path.isEmpty()) {
            paths.emplace_back(path.toStdWString());
        }
    }
    file_transfer_.PreviewSendLocalPaths(paths, fs::path(remote_destination_root.toStdWString()));
}

void NetworkWorker::ReceivePaths(const QStringList& remote_paths, const QString& local_destination_root) {
    if (!IsInteractiveSession()) {
        return;
    }
    std::vector<fs::path> paths;
    for (const auto& path : remote_paths) {
        if (!path.isEmpty()) {
            paths.emplace_back(path.toStdWString());
        }
    }
    file_transfer_.PreviewRemotePaths(paths, fs::path(local_destination_root.toStdWString()));
}

void NetworkWorker::ResolveTransferPreview(const QString& job_id, bool allow_overwrite) {
    if (!IsInteractiveSession() || job_id.trimmed().isEmpty()) {
        return;
    }
    file_transfer_.ResolveTransferPreview(job_id.toStdString(), allow_overwrite);
}

void NetworkWorker::SendClipboardText(const QString& text) {
    if (!IsInteractiveSession() || !peer_session_active_.load()) {
        return;
    }

    CancelClipboardFileOffers(QStringLiteral("text clipboard superseded pending file clipboard transfer"));

    const QByteArray utf8 = text.toUtf8();
    peer_.SendDataChannelMessage("clipboard",
                                 reinterpret_cast<const uint8_t*>(utf8.constData()),
                                 static_cast<size_t>(utf8.size()),
                                 false);
}

void NetworkWorker::PublishLocalClipboardFiles(const QStringList& local_paths) {
    if (!IsInteractiveSession() || !peer_session_active_.load()) {
        return;
    }

    QStringList normalized_paths;
    for (const QString& path : local_paths) {
        const QString cleaned_path = QDir::cleanPath(path);
        if (!cleaned_path.trimmed().isEmpty() && !normalized_paths.contains(cleaned_path)) {
            normalized_paths.push_back(cleaned_path);
        }
    }

    const QStringList entry_names = TopLevelClipboardEntryNames(normalized_paths);
    if (normalized_paths.isEmpty() || entry_names.isEmpty()) {
        return;
    }

    CancelClipboardFileOffers(QStringLiteral("file clipboard replaced the previous pending session transfer"));

    ClipboardFileOffer offer;
    offer.token = GenerateClipboardTransferToken();
    offer.source_paths = normalized_paths;
    offer.entry_names = entry_names;

    pending_outgoing_clipboard_offers_[offer.token.toStdString()] = offer;
    UpdateClipboardFilePasteAvailability();

    json payload;
    payload["type"] = "clipboard_files_offer";
    payload["token"] = offer.token.toStdString();
    payload["entry_names"] = json::array();
    for (const QString& entry_name : offer.entry_names) {
        payload["entry_names"].push_back(entry_name.toStdString());
    }

    peer_.SendDataChannelMessage("control", payload.dump());
    emit StatusUpdated(QStringLiteral("Arquivos copiados localmente. Pressione Ctrl+V na sessao para transferir."));
}

void NetworkWorker::TriggerClipboardPaste() {
    if (!IsInteractiveSession() || !peer_session_active_.load()) {
        return;
    }

    for (auto& [token, offer] : pending_outgoing_clipboard_offers_) {
        if (offer.paste_requested) {
            continue;
        }

        offer.paste_requested = true;
        UpdateClipboardFilePasteAvailability();

        json payload;
        payload["type"] = "clipboard_files_paste_request";
        payload["token"] = token;
        peer_.SendDataChannelMessage("control", payload.dump());
        emit StatusUpdated(QStringLiteral("Preparando envio de arquivos para o computador remoto..."));
        return;
    }

    if (!pending_incoming_clipboard_offers_.empty()) {
        auto it = pending_incoming_clipboard_offers_.begin();
        const QString token = it->second.token;
        const auto target = remotex::core::ResolveExplorerDirectoryOrDesktop();
        pending_incoming_clipboard_offers_.erase(it);
        UpdateClipboardFilePasteAvailability();

        if (target.path.empty()) {
            emit StatusUpdated(QStringLiteral("Nao foi possivel resolver o destino local da transferencia."));
            json payload;
            payload["type"] = "clipboard_files_cancel";
            payload["token"] = token.toStdString();
            payload["error"] = "viewer local destination unavailable";
            peer_.SendDataChannelMessage("control", payload.dump());
            return;
        }

        json payload;
        payload["type"] = "clipboard_files_accept";
        payload["token"] = token.toStdString();
        payload["destination_root"] = target.path.string();
        payload["resolved_from_explorer"] = target.resolved_from_explorer;
        payload["used_desktop_fallback"] = target.used_desktop_fallback;
        peer_.SendDataChannelMessage("control", payload.dump());
        emit StatusUpdated(target.used_desktop_fallback
            ? QStringLiteral("Recebendo arquivos do computador remoto para a Area de Trabalho local...")
            : QStringLiteral("Recebendo arquivos do computador remoto para a pasta local ativa..."));
    }
}

void NetworkWorker::SendChatMessage(const QString& text) {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string json = "{\"from\":\"viewer\",\"text\":\"";
    const std::string utf8 = text.toStdString();
    for (char c : utf8) {
        switch (c) {
            case '"':  json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            default:   json += c; break;
        }
    }
    json += "\",\"ts\":" + std::to_string(now) + "}";
    peer_.SendDataChannelMessage("chat", json);
}
