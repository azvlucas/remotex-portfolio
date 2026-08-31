namespace {

constexpr qsizetype kPreviewSummaryListLimit = 6;

QString FormatTransferSize(quint64 bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit_index = 0;
    while (value >= 1024.0 && unit_index < 4) {
        value /= 1024.0;
        ++unit_index;
    }
    return QString::number(value, unit_index == 0 ? 'f' : 'f', unit_index == 0 ? 0 : 1) + " " + units[unit_index];
}

QString BuildTransferConflictSummary(const remotex::core::TransferConflictPreview& preview) {
    QStringList relative_paths;
    for (const auto& conflict : preview.conflicts) {
        const QString relative_path = QString::fromStdString(conflict.relative_path).trimmed();
        if (!relative_path.isEmpty() && !relative_paths.contains(relative_path)) {
            relative_paths.push_back(relative_path);
        }
    }

    relative_paths.sort(Qt::CaseInsensitive);
    if (relative_paths.isEmpty()) {
        return {};
    }

    QStringList lines;
    lines.push_back(
        QStringLiteral("%1 encontrou conflitos em %2 de %3 itens (%4 total).")
            .arg(preview.direction == remotex::core::TransferDirection::Receive
                     ? QStringLiteral("O recebimento")
                     : QStringLiteral("O envio"))
            .arg(relative_paths.size())
            .arg(preview.total_entries)
            .arg(FormatTransferSize(preview.total_bytes)));
    lines.push_back(QStringLiteral("Confirme a sobrescrita apenas se estes caminhos estiverem corretos:"));
    lines.push_back(QString());

    const int listed_items = std::min(relative_paths.size(), kPreviewSummaryListLimit);
    for (int index = 0; index < listed_items; ++index) {
        lines.push_back(QStringLiteral("- %1").arg(relative_paths.at(index)));
    }
    if (relative_paths.size() > listed_items) {
        lines.push_back(QStringLiteral("- ... e mais %1 item(ns)").arg(relative_paths.size() - listed_items));
    }

    return lines.join(QStringLiteral("\n"));
}

struct TransferConflictFailurePresentation {
    QString outcome;
    QString message;
};

TransferConflictFailurePresentation DescribeTransferConflictFailure(
    const remotex::core::TransferJobInfo& info,
    const std::string& error) {
    const bool receiving = info.direction == remotex::core::TransferDirection::Receive;
    const QString direction_label = receiving
        ? QStringLiteral("Recebimento")
        : QStringLiteral("Envio");
    const QString error_text = QString::fromStdString(error);

    if (error.rfind("conflict-declined:", 0) == 0) {
        return {
            QStringLiteral("preview-declined"),
            QStringLiteral("%1 cancelado: sobrescrita nao aprovada.").arg(direction_label)
        };
    }
    if (error.rfind("preview-timeout:", 0) == 0) {
        return {
            QStringLiteral("preview-timeout"),
            QStringLiteral("%1 cancelado: o host nao respondeu ao resumo de conflitos.").arg(direction_label)
        };
    }
    if (error.rfind("preview-unsupported:", 0) == 0) {
        return {
            QStringLiteral("preview-unsupported"),
            QStringLiteral("%1 cancelado: o host nao conseguiu validar conflitos antes da transferencia.").arg(direction_label)
        };
    }
    if (error.rfind("preview-malformed:", 0) == 0) {
        return {
            QStringLiteral("preview-malformed"),
            QStringLiteral("%1 cancelado: o resumo de conflitos retornou dados invalidos.").arg(direction_label)
        };
    }

    return {
        QStringLiteral("preview-error"),
        error_text.trimmed().isEmpty()
            ? QStringLiteral("%1 cancelado: falha ao validar conflitos.").arg(direction_label)
            : QStringLiteral("%1 cancelado: %2").arg(direction_label, error_text)
    };
}

} // namespace

void NetworkWorker::OnLocalDescription(const remotex::core::SessionDescription& desc) {
    if (QThread::currentThread() != thread()) {
        const auto desc_copy = desc;
        QMetaObject::invokeMethod(this, [this, desc_copy]() {
            OnLocalDescription(desc_copy);
        }, Qt::QueuedConnection);
        return;
    }
    if (shutting_down_.load()) {
        return;
    }

    if (desc.type == "offer") {
        qInfo().noquote() << QString("[viewer][attempt=%1] local offer created").arg(session_attempt_id_);
        signaling_.SendOffer(session_id_,
                             desc.sdp,
                             std::string(remotex::core::SessionAccessModeToString(access_mode_)),
                             backend_session_id_);
        const bool waiting_for_client_consent =
            IsInteractiveSession()
            && backend_session_status_ == std::string(remotex::core::kSessionStatusPendingConsent);
        emit StatusUpdated(waiting_for_client_consent
            ? "Aguardando autorizacao do cliente..."
            : "Aguardando resposta do host...");
        ArmRecoveryTimer(waiting_for_client_consent ? kWaitingConsentTimeoutMs : kNegotiationTimeoutMs,
                         waiting_for_client_consent ? "waiting_client_consent" : "waiting_answer");
    } else {
        qInfo().noquote() << QString("[viewer][attempt=%1] local answer created").arg(session_attempt_id_);
        signaling_.SendAnswer(session_id_, desc.sdp);
        emit StatusUpdated("Conectando relay...");
        ArmRecoveryTimer(kWaitingVideoTimeoutMs, "connecting_video");
    }
}

void NetworkWorker::HandleControlMessage(const std::string& json) {
    std::string type_needle = "\"type\":\"";
    auto tpos = json.find(type_needle);
    if (tpos == std::string::npos) return;
    tpos += type_needle.size();
    auto tend = json.find('"', tpos);
    if (tend == std::string::npos) return;
    std::string type = json.substr(tpos, tend - tpos);

    if (type == "monitor_list") {
        QList<QVariantMap> monitors;
        std::string search_pos_str = "\"monitors\":[";
        auto arr_start = json.find(search_pos_str);
        if (arr_start == std::string::npos) return;
        arr_start += search_pos_str.size();

        size_t pos = arr_start;
        while (pos < json.size()) {
            auto obj_start = json.find('{', pos);
            if (obj_start == std::string::npos) break;
            auto obj_end = json.find('}', obj_start);
            if (obj_end == std::string::npos) break;
            std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

            auto extractInt = [&](const std::string& key) -> int {
                std::string needle = "\"" + key + "\":";
                auto p = obj.find(needle);
                if (p == std::string::npos) return 0;
                p += needle.size();
                while (p < obj.size() && obj[p] == ' ') ++p;
                return std::atoi(obj.c_str() + p);
            };
            auto extractBool = [&](const std::string& key) -> bool {
                std::string needle = "\"" + key + "\":";
                auto p = obj.find(needle);
                if (p == std::string::npos) return false;
                p += needle.size();
                while (p < obj.size() && obj[p] == ' ') ++p;
                return obj.substr(p, 4) == "true";
            };
            auto extractStr = [&](const std::string& key) -> QString {
                std::string needle = "\"" + key + "\":\"";
                auto p = obj.find(needle);
                if (p == std::string::npos) return {};
                p += needle.size();
                auto e = obj.find('"', p);
                if (e == std::string::npos) return {};
                return QString::fromStdString(obj.substr(p, e - p));
            };

            QVariantMap m;
            m["adapter_index"] = extractInt("adapter_index");
            m["output_index"] = extractInt("output_index");
            m["name"] = extractStr("name");
            m["width"] = extractInt("width");
            m["height"] = extractInt("height");
            m["primary"] = extractBool("primary");
            monitors.append(m);

            pos = obj_end + 1;
        }
        emit MonitorListReceived(monitors);
    }
    else if (type == "monitor_changed") {
        const auto contract = ParseMonitorChangedContract(json);
        if (!contract) {
            qWarning().noquote() << QString("[viewer] failed to parse monitor_changed payload: %1")
                                        .arg(QString::fromStdString(json));
            return;
        }
        emit MonitorChanged(*contract);
    }
    else if (type == "reboot_ack") {
        emit StatusUpdated("Host reiniciando - aguardando reconexao...");
        emit RebootAcknowledged();
    }
    else if (type == "reboot_failed") {
        reboot_reconnect_mode_ = false;
        std::string err_needle = "\"error\":\"";
        auto epos = json.find(err_needle);
        QString error = "Erro desconhecido";
        if (epos != std::string::npos) {
            epos += err_needle.size();
            auto eend = json.find('"', epos);
            if (eend != std::string::npos)
                error = QString::fromStdString(json.substr(epos, eend - epos));
        }
        emit RebootFailed(error);
    }
    else if (type == "secure_attention_status") {
        remotex::viewer::SecureAttentionStatus status;
        status.acknowledged = (json.find("\"acknowledged\":true") != std::string::npos);

        std::string err_needle = "\"error\":\"";
        auto epos = json.find(err_needle);
        if (epos != std::string::npos) {
            epos += err_needle.size();
            auto eend = json.find('"', epos);
            if (eend != std::string::npos) {
                status.error = QString::fromStdString(json.substr(epos, eend - epos));
            }
        }

        emit SecureAttentionStatusChanged(status);
    }
    else if (type == "privacy_mode_status") {
        remotex::viewer::RemoteControlToggleStatus status;
        status.enabled = (json.find("\"enabled\":true") != std::string::npos);

        std::string err_needle = "\"error\":\"";
        auto epos = json.find(err_needle);
        if (epos != std::string::npos) {
            epos += err_needle.size();
            auto eend = json.find('"', epos);
            if (eend != std::string::npos) {
                status.error = QString::fromStdString(json.substr(epos, eend - epos));
            }
        }

        emit PrivacyModeStatusChanged(status);
    }
    else if (type == "block_input_status") {
        remotex::viewer::RemoteControlToggleStatus status;
        status.enabled = (json.find("\"enabled\":true") != std::string::npos);

        std::string err_needle = "\"error\":\"";
        auto epos = json.find(err_needle);
        if (epos != std::string::npos) {
            epos += err_needle.size();
            auto eend = json.find('"', epos);
            if (eend != std::string::npos) {
                status.error = QString::fromStdString(json.substr(epos, eend - epos));
            }
        }

        emit BlockInputStatusChanged(status);
    }
    else if (type == "clipboard_files_offer") {
        HandleClipboardFilesOffer(json);
    }
    else if (type == "clipboard_files_accept") {
        HandleClipboardFilesAccept(json);
    }
    else if (type == "clipboard_files_cancel") {
        HandleClipboardFilesCancel(json);
    }
    else if (type == "session_diagnostics") {
        auto extractDouble = [&](const std::string& key) -> double {
            std::string needle = "\"" + key + "\":";
            auto p = json.find(needle);
            if (p == std::string::npos) return 0.0;
            p += needle.size();
            while (p < json.size() && json[p] == ' ') ++p;
            return std::atof(json.c_str() + p);
        };
        UpdateHostDiagnostics(extractDouble("capture_fps"),
                              extractDouble("encoded_fps"),
                              extractDouble("sent_fps"));
        EmitDiagnosticsSummary();
    }
}

void NetworkWorker::SelectMonitor(int adapter_index, int output_index) {
    if (!peer_session_active_.load()) return;
    std::string json = "{\"type\":\"select_monitor\""
        ",\"adapter_index\":" + std::to_string(adapter_index)
        + ",\"output_index\":" + std::to_string(output_index)
        + "}";
    peer_.SendDataChannelMessage("control", json);
}

void NetworkWorker::SendRebootRequest() {
    if (!peer_session_active_.load()) return;
    reboot_reconnect_mode_ = true;
    peer_.SendDataChannelMessage("control", R"({"type":"reboot_request"})");
}

void NetworkWorker::RequestSecureAttention() {
    if (!peer_session_active_.load()) {
        return;
    }
    peer_.SendDataChannelMessage("control", R"({"type":"secure_attention"})");
}

void NetworkWorker::SendNumLockPolicy(bool enabled) {
    if (!IsInteractiveSession() || !peer_session_active_.load()) {
        return;
    }
    std::string json = R"({"type":"num_lock_policy","enabled":)";
    json += (enabled ? "true" : "false");
    json += "}";
    peer_.SendDataChannelMessage("control", json);
}

void NetworkWorker::SetPrivacyMode(bool enabled) {
    if (!peer_session_active_.load()) return;
    std::string json = R"({"type":"privacy_mode","enabled":)";
    json += (enabled ? "true" : "false");
    json += "}";
    peer_.SendDataChannelMessage("control", json);
}

void NetworkWorker::SetBlockInput(bool enabled) {
    if (!peer_session_active_.load()) return;
    std::string json = R"({"type":"block_input","enabled":)";
    json += (enabled ? "true" : "false");
    json += "}";
    peer_.SendDataChannelMessage("control", json);
}

void NetworkWorker::SendRefreshRequest() {
    if (!peer_session_active_.load()) return;
    peer_.SendDataChannelMessage("control", R"({"type":"request_refresh"})");
}

void NetworkWorker::OnRemoteListing(const remotex::core::RemoteFsListing& listing) {
    if (!IsInteractiveSession()) {
        return;
    }
    RemoteListingResult result;
    result.request_id = listing.request_id;
    result.path = QString::fromStdString(listing.path);
    result.is_root_listing = listing.is_root_listing;
    result.success = listing.success;
    result.error = QString::fromStdString(listing.error);

    for (const auto& entry : listing.entries) {
        RemoteFileItem item;
        item.name = QString::fromStdString(entry.name);
        item.absolute_path = QString::fromStdString(entry.absolute_path);
        item.is_directory = entry.is_directory;
        item.is_access_denied = entry.is_access_denied;
        item.size_bytes = entry.size_bytes;
        item.modified_unix_sec = entry.modified_unix_sec;
        result.entries.push_back(item);
    }

    emit RemoteListingReady(result);
}

void NetworkWorker::OnTransferConflictPreviewReady(const remotex::core::TransferConflictPreview& preview) {
    if (!IsInteractiveSession()) {
        return;
    }

    const QString summary = BuildTransferConflictSummary(preview);
    qInfo().noquote() << QString("[viewer][attempt=%1] transfer conflict preview ready job=%2 conflicts=%3 direction=%4")
                             .arg(session_attempt_id_)
                             .arg(QString::fromStdString(preview.job_id_hex))
                             .arg(preview.conflicts.size())
                             .arg(preview.direction == remotex::core::TransferDirection::Receive
                                      ? QStringLiteral("download")
                                      : QStringLiteral("upload"));
    emit TransferConflictPreviewReady(QString::fromStdString(preview.job_id_hex),
                                      preview.direction == remotex::core::TransferDirection::Receive,
                                      summary);
}

void NetworkWorker::OnTransferConflictPreviewFailed(const remotex::core::TransferJobInfo& info,
                                                    const std::string& error) {
    if (!IsInteractiveSession()) {
        return;
    }

    const auto presentation = DescribeTransferConflictFailure(info, error);
    qWarning().noquote() << QString("[viewer][attempt=%1] transfer conflict preview failed job=%2 outcome=%3 error=%4")
                                .arg(session_attempt_id_)
                                .arg(QString::fromStdString(info.id_hex))
                                .arg(presentation.outcome)
                                .arg(QString::fromStdString(error));
    RecordTransferAuditEvent("file_transfer_failed",
                             info,
                             presentation.outcome.toStdString(),
                             error);
    emit TransferConflictPreviewFailed(QString::fromStdString(info.id_hex),
                                       presentation.outcome,
                                       presentation.message);
}

void NetworkWorker::OnTransferJobStarted(const remotex::core::TransferJobInfo& info) {
    if (!IsInteractiveSession()) {
        return;
    }
    RecordTransferAuditEvent(
        info.direction == remotex::core::TransferDirection::Receive
            ? "file_download_started"
            : "file_upload_started",
        info,
        "started");
    emit TransferJobStarted(QString::fromStdString(info.id_hex),
                            QString::fromStdString(info.destination_root),
                            info.total_bytes,
                            static_cast<int>(info.total_entries),
                            info.direction == remotex::core::TransferDirection::Receive);
}

void NetworkWorker::OnTransferJobProgress(const remotex::core::TransferJobInfo& job,
                                          const remotex::core::TransferEntryInfo& entry) {
    if (!IsInteractiveSession()) {
        return;
    }
    emit TransferJobProgress(QString::fromStdString(job.id_hex),
                             QString::fromStdString(entry.relative_path),
                             job.bytes_done,
                             job.total_bytes,
                             static_cast<int>(job.entries_done),
                             static_cast<int>(job.total_entries));
}

void NetworkWorker::OnTransferJobCompleted(const remotex::core::TransferJobInfo& info,
                                           const std::filesystem::path& destination_root) {
    if (!IsInteractiveSession()) {
        return;
    }

    RecordTransferAuditEvent(
        info.direction == remotex::core::TransferDirection::Receive
            ? "file_download_finished"
            : "file_upload_finished",
        info,
        "completed");
    emit TransferJobFinished(QString::fromStdString(info.id_hex),
                             true,
                             QString::fromStdWString(destination_root.wstring()),
                             QString());
}

void NetworkWorker::OnTransferJobFailed(const remotex::core::TransferJobInfo& info,
                                        const std::string& error) {
    if (!IsInteractiveSession()) {
        return;
    }

    RecordTransferAuditEvent("file_transfer_failed", info, "failed", error);
    emit TransferJobFinished(QString::fromStdString(info.id_hex),
                             false,
                             QString::fromStdString(info.destination_root),
                             QString::fromStdString(error));
}

void NetworkWorker::OnTransferJobCancelled(const remotex::core::TransferJobInfo& info) {
    if (!IsInteractiveSession()) {
        return;
    }

    RecordTransferAuditEvent("file_transfer_failed", info, "cancelled", "Transferencia cancelada");
    emit TransferJobFinished(QString::fromStdString(info.id_hex),
                             false,
                             QString::fromStdString(info.destination_root),
                             QStringLiteral("Transferencia cancelada"));
}
