void NetworkWorker::ConnectToMachine(const QString& machine_id,
                                     const QString& signaling_host,
                                     quint16 signaling_port) {
    EnsureRecoveryTimer();

    const bool is_restart = restart_in_progress_;
    restart_in_progress_ = false;
    shutting_down_.store(false);
    ResetPeerSession();
    machine_id_ = machine_id.toStdString();
    session_id_ = machine_id.toStdString();
    signaling_host_ = signaling_host;
    signaling_port_ = signaling_port;
    video_frame_seen_ = false;
    ++session_attempt_id_;
    if (!is_restart) {
        auto_recovery_attempts_ = 0;
    }

    qInfo().noquote() << QString("[viewer][attempt=%1] connect machine=%2 signaling=%3:%4")
                             .arg(session_attempt_id_)
                             .arg(machine_id)
                             .arg(QString::fromStdString(
                                 remotex::core::RedactIdentifier(signaling_host.toStdString(), "signaling_host")))
                             .arg(static_cast<int>(signaling_port));

    QString default_receive_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (default_receive_dir.isEmpty()) {
        default_receive_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    default_receive_dir_ = fs::path(default_receive_dir.toStdWString());
    if (IsInteractiveSession()) {
        file_transfer_.Initialize(&peer_, default_receive_dir_);
    }

    remotex::core::SignalingConfig sig_cfg;
    sig_cfg.host = signaling_host.toStdString();
    sig_cfg.port = signaling_port;
    sig_cfg.use_tls = connection_settings_.signaling_use_tls;
    sig_cfg.token_provider = [this](bool force_refresh) {
        if (force_refresh) {
            auth_->RefreshToken();
        }
        return auth_->GetAccessToken();
    };
    sig_cfg.client_id = auth_->GetUserID();
    signaling_.Connect(sig_cfg);
}

void NetworkWorker::Disconnect() {
    CancelRecoveryTimer();
    if (ping_timer_) ping_timer_->stop();
    CancelClipboardFileOffers(QStringLiteral("viewer disconnect"));
    EndTrackedSession();
    shutting_down_.store(true);
    video_frame_seen_ = false;
    ResetPeerSession();
    file_transfer_.Shutdown();
    signaling_.Disconnect();
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->disconnect(this);
    }
}

bool NetworkWorker::HasActivePeerSession() const {
    return peer_session_active_.load();
}

void NetworkWorker::UpdateRelayBundle(const remotex::core::SessionInfo& session) {
    backend_relay_protocol_version_ = session.relay_protocol_version;
    backend_relay_session_id_ = session.relay_session_id;
    backend_relay_role_ = session.relay_role;
    backend_relay_ticket_ = session.relay_ticket;
    backend_relay_ticket_expires_unix_ = session.relay_ticket_expires_unix;
    backend_relay_host_ = session.relay_host;
    backend_relay_port_ = session.relay_port;
}

void NetworkWorker::ResetPeerSession() {
    qWarning().noquote() << QString("[viewer][attempt=%1] reset peer session transport_connected=%2 peer_active=%3 backend_session=%4 backend_status=%5 relay_session=%6")
                                .arg(session_attempt_id_)
                                .arg(peer_transport_connected_.load() ? "true" : "false")
                                .arg(peer_session_active_.load() ? "true" : "false")
                                .arg(QString::fromStdString(backend_session_id_))
                                .arg(QString::fromStdString(backend_session_status_))
                                .arg(QString::fromStdString(backend_relay_session_id_));
    CancelRecoveryTimer();
    if (ping_timer_) ping_timer_->stop();
    CancelClipboardFileOffers(QStringLiteral("peer session reset"));
    EndTrackedSession();
    video_frame_seen_ = false;
    peer_transport_connected_.store(false);
    peer_session_active_.store(false);
    file_transfer_.Shutdown();
    peer_.Shutdown();
}

void NetworkWorker::StartPeerSession() {
    if (shutting_down_.load() || session_id_.empty() || HasActivePeerSession()) {
        return;
    }

    if (backend_session_id_.empty()) {
        qWarning().noquote() << QString("[viewer][attempt=%1] refusing peer bootstrap without backend session")
                                    .arg(session_attempt_id_);
        emit Disconnected("Falha ao iniciar sessao no backend");
        return;
    }
    if (IsInteractiveSession() && !default_receive_dir_.empty()) {
        file_transfer_.Initialize(&peer_, default_receive_dir_);
    }

    qInfo().noquote() << QString("[viewer][attempt=%1] starting peer session for machine=%2 backend_session=%3 relay_session=%4")
                             .arg(session_attempt_id_)
                             .arg(QString::fromStdString(machine_id_))
                             .arg(QString::fromStdString(backend_session_id_))
                             .arg(QString::fromStdString(backend_relay_session_id_.empty() ? backend_session_id_ : backend_relay_session_id_));
    emit StatusUpdated("Host encontrado - negociando...");

    remotex::core::PeerConfig cfg;
    cfg.session_id = backend_relay_session_id_.empty()
        ? backend_session_id_
        : backend_relay_session_id_;
    cfg.is_offerer = true;
    cfg.relay_host = backend_relay_host_.empty()
        ? connection_settings_.relay_host.toStdString()
        : backend_relay_host_;
    cfg.relay_port = backend_relay_port_ == 0
        ? connection_settings_.relay_port
        : backend_relay_port_;
    cfg.relay_protocol_version = backend_relay_protocol_version_ > 0
        ? static_cast<uint32_t>(backend_relay_protocol_version_)
        : 1;
    cfg.relay_ticket_expires_unix = backend_relay_ticket_expires_unix_ > 0
        ? static_cast<uint64_t>(backend_relay_ticket_expires_unix_)
        : 0;
    cfg.relay_ticket = backend_relay_ticket_;

    remotex::core::IceServer stun;
    stun.urls.push_back("stun:stun.l.google.com:19302");
    cfg.ice_servers.push_back(stun);

    if (cfg.session_id.empty() || cfg.relay_ticket.empty()) {
        qWarning().noquote() << QString("[viewer][attempt=%1] missing relay credentials backend_session=%2 relay_session=%3 relay_ticket_present=%4")
                                    .arg(session_attempt_id_)
                                    .arg(QString::fromStdString(backend_session_id_))
                                    .arg(QString::fromStdString(cfg.session_id))
                                    .arg(cfg.relay_ticket.empty() ? "false" : "true");
        EndTrackedSession();
        emit Disconnected("Sessao sem credenciais do relay");
        return;
    }

    if (!peer_.Initialize(cfg)) {
        qWarning().noquote() << QString("[viewer][attempt=%1] peer initialize failed relay_host=%2 relay_port=%3 protocol=%4")
                                    .arg(session_attempt_id_)
                                    .arg(QString::fromStdString(
                                        remotex::core::RedactIdentifier(cfg.relay_host, "relay_host")))
                                    .arg(cfg.relay_port)
                                    .arg(cfg.relay_protocol_version);
        EndTrackedSession();
        emit Disconnected("Falha ao iniciar peer");
        return;
    }

    peer_session_active_.store(true);
    peer_transport_connected_.store(false);
    video_frame_seen_ = false;
    peer_.CreateDataChannel("latency", true, true);
    peer_.CreateDataChannel("chat", true, true);
    peer_.CreateDataChannel("control", true, true);
    if (IsInteractiveSession()) {
        peer_.CreateDataChannel("input", true, true);
        peer_.CreateDataChannel("files", true, true);
        peer_.CreateDataChannel("clipboard", true, true);
    }
    peer_.CreateOffer();
}

void NetworkWorker::EnsureRecoveryTimer() {
    if (recovery_timer_) {
        return;
    }

    recovery_timer_ = new QTimer(this);
    recovery_timer_->setSingleShot(true);
    connect(recovery_timer_, &QTimer::timeout, this, [this]() {
        if (shutting_down_.load()) {
            return;
        }

        qWarning().noquote() << QString("[viewer][attempt=%1] recovery timeout phase=%2 connected=%3 video=%4")
                                    .arg(session_attempt_id_)
                                    .arg(recovery_phase_)
                                    .arg(peer_transport_connected_.load() ? "true" : "false")
                                    .arg(video_frame_seen_ ? "true" : "false");
        RestartConnection(QString("timeout:%1").arg(recovery_phase_));
    });
}

void NetworkWorker::ArmRecoveryTimer(int interval_ms, const QString& phase) {
    EnsureRecoveryTimer();
    recovery_phase_ = phase;
    recovery_timer_->start(interval_ms);
    qInfo().noquote() << QString("[viewer][attempt=%1] arm recovery timer phase=%2 timeout_ms=%3")
                             .arg(session_attempt_id_)
                             .arg(phase)
                             .arg(interval_ms);
}

void NetworkWorker::CancelRecoveryTimer() {
    if (recovery_timer_) {
        recovery_timer_->stop();
    }
    recovery_phase_.clear();
}

void NetworkWorker::RestartConnection(const QString& reason) {
    if (shutting_down_.load()) {
        return;
    }
    if (signaling_host_.isEmpty() || signaling_port_ == 0) {
        qWarning().noquote() << QString("[viewer][attempt=%1] cannot restart: signaling endpoint unavailable")
                                    .arg(session_attempt_id_);
        return;
    }
    const int max_attempts = reboot_reconnect_mode_
        ? kMaxRebootRecoveryAttempts : kMaxAutoRecoveryAttempts;
    if (auto_recovery_attempts_ >= max_attempts) {
        CancelRecoveryTimer();
        reboot_reconnect_mode_ = false;
        qWarning().noquote() << QString("[viewer][attempt=%1] auto recovery exhausted after %2 attempts")
                                    .arg(session_attempt_id_)
                                    .arg(auto_recovery_attempts_);
        emit StatusUpdated("Sessao sem resposta - use Tentar Novamente");
        emit Disconnected("Connection recovery exhausted");
        return;
    }

    ++auto_recovery_attempts_;
    qInfo().noquote() << QString("[viewer][attempt=%1] restarting connection reason=%2 auto_recovery=%3")
                             .arg(session_attempt_id_)
                             .arg(reason)
                             .arg(auto_recovery_attempts_);
    if (reboot_reconnect_mode_) {
        emit StatusUpdated(QString("Host reiniciando - tentativa %1...").arg(auto_recovery_attempts_));
    } else {
        emit StatusUpdated(QString("Recuperando sessao (%1)...").arg(reason));
    }

    restart_in_progress_ = true;
    const QString machine_id = QString::fromStdString(machine_id_);
    const QString signaling_host = signaling_host_;
    const quint16 signaling_port = signaling_port_;

    Disconnect();
    ConnectToMachine(machine_id, signaling_host, signaling_port);
}

bool NetworkWorker::StartTrackedSession(QString* error_message) {
    if (machine_id_.empty() || !backend_session_id_.empty()) {
        return !machine_id_.empty();
    }

    auto result = machine_client_.StartSession(machine_id_, access_mode_);
    if (result.session) {
        backend_session_id_ = result.session->id;
        backend_session_status_ = result.session->status;
        UpdateRelayBundle(*result.session);
        qInfo().noquote() << QString("[viewer] backend session started backend_session=%1")
                                 .arg(QString::fromStdString(
                                     remotex::core::RedactIdentifier(backend_session_id_, "backend_session")));

        if (!session_heartbeat_timer_) {
            session_heartbeat_timer_ = new QTimer(this);
            session_heartbeat_timer_->setInterval(30000);
            connect(session_heartbeat_timer_, &QTimer::timeout, this, [this]() {
                if (backend_session_id_.empty()) return;
                machine_client_.SessionHeartbeat(backend_session_id_);
            });
        }
        session_heartbeat_timer_->start();

        return true;
    } else {
        const QString error = QString::fromStdString(result.error.empty()
            ? "Falha ao iniciar sessao no backend"
            : result.error);
        qWarning().noquote() << QString("[viewer] failed to start backend session for machine=%1 http=%2 error=%3")
                                    .arg(QString::fromStdString(machine_id_))
                                    .arg(result.http_code)
                                    .arg(error);
        if (error_message) {
            *error_message = error;
        }
    }
    return false;
}

void NetworkWorker::EndTrackedSession() {
    if (session_heartbeat_timer_) session_heartbeat_timer_->stop();

    if (backend_session_id_.empty()) {
        qInfo().noquote() << QString("[viewer][attempt=%1] end tracked session skipped because backend session is already empty")
                                 .arg(session_attempt_id_);
        backend_session_status_.clear();
        backend_relay_protocol_version_ = 0;
        backend_relay_session_id_.clear();
        backend_relay_role_.clear();
        backend_relay_ticket_.clear();
        backend_relay_ticket_expires_unix_ = 0;
        backend_relay_host_.clear();
        backend_relay_port_ = 0;
        return;
    }

    qInfo().noquote() << QString("[viewer][attempt=%1] ending backend session backend_session=%2 status=%3 relay_session=%4 shutting_down=%5")
                             .arg(session_attempt_id_)
                             .arg(QString::fromStdString(
                                 remotex::core::RedactIdentifier(backend_session_id_, "backend_session")))
                             .arg(QString::fromStdString(backend_session_status_))
                             .arg(QString::fromStdString(
                                 remotex::core::RedactIdentifier(backend_relay_session_id_, "relay_session")))
                             .arg(shutting_down_.load() ? "true" : "false");

    if (shutting_down_.load()) {
        auto client_copy = machine_client_;
        auto session_id = backend_session_id_;
        std::thread([client_copy, session_id]() mutable {
            client_copy.EndSession(session_id);
        }).detach();
    } else {
        machine_client_.EndSession(backend_session_id_);
    }
    backend_session_id_.clear();
    backend_session_status_.clear();
    backend_relay_protocol_version_ = 0;
    backend_relay_session_id_.clear();
    backend_relay_role_.clear();
    backend_relay_ticket_.clear();
    backend_relay_ticket_expires_unix_ = 0;
    backend_relay_host_.clear();
    backend_relay_port_ = 0;
}

void NetworkWorker::OnConnected() {
    QMetaObject::invokeMethod(this, [this]() {
        if (shutting_down_.load()) {
            return;
        }
        QString session_error;
        if (!StartTrackedSession(&session_error)) {
            qWarning().noquote() << QString("[viewer][attempt=%1] backend session start failed before signaling join error=%2")
                                        .arg(session_attempt_id_)
                                        .arg(session_error.isEmpty() ? "Falha ao iniciar sessao no backend" : session_error);
            shutting_down_.store(true);
            signaling_.Disconnect();
            emit Disconnected(session_error.isEmpty() ? "Falha ao iniciar sessao no backend" : session_error);
            return;
        }
        qInfo().noquote() << QString("[viewer][attempt=%1] signaling connected machine_session=%2 backend_session=%3 backend_status=%4")
                                 .arg(session_attempt_id_)
                                 .arg(QString::fromStdString(session_id_))
                                 .arg(QString::fromStdString(backend_session_id_))
                                 .arg(QString::fromStdString(backend_session_status_));
        signaling_.JoinSession(session_id_, remotex::core::signal_role::Viewer);
        if (peer_transport_connected_.load()) {
            CancelRecoveryTimer();
            emit StatusUpdated("Sinalizacao restabelecida");
        } else {
            emit StatusUpdated("Aguardando host...");
            ArmRecoveryTimer(kWaitHostTimeoutMs, "waiting_host");
        }
    }, Qt::QueuedConnection);
}

void NetworkWorker::OnDisconnected() {
    QMetaObject::invokeMethod(this, [this]() {
        if (shutting_down_.load()) {
            return;
        }

        qWarning().noquote() << QString("[viewer][attempt=%1] signaling disconnected connected=%2")
                                    .arg(session_attempt_id_)
                                    .arg(peer_transport_connected_.load() ? "true" : "false");

        if (peer_transport_connected_.load()) {
            emit StatusUpdated("Reconectando sinalizacao...");
            ArmRecoveryTimer(kNegotiationTimeoutMs, "signaling_reconnect");
            return;
        }

        if (HasActivePeerSession() || !backend_session_id_.empty()) {
            ResetPeerSession();
        }
        qWarning().noquote() << QString("[viewer][attempt=%1] emitting disconnected because signaling dropped before transport machine_session=%2 backend_session=%3")
                                    .arg(session_attempt_id_)
                                    .arg(QString::fromStdString(
                                        remotex::core::RedactIdentifier(session_id_, "machine_session")))
                                    .arg(QString::fromStdString(
                                        remotex::core::RedactIdentifier(backend_session_id_, "backend_session")));
        emit Disconnected("Signaling disconnected");
    }, Qt::QueuedConnection);
}

void NetworkWorker::OnSignalingMessage(const remotex::core::SignalingEvent& event) {
    auto ev = event;
    QMetaObject::invokeMethod(this, [this, ev = std::move(ev)]() {
        using namespace remotex::core;

        if (shutting_down_.load()) {
            return;
        }

        qInfo().noquote() << QString("[viewer][attempt=%1] signaling message type=%2")
                                 .arg(session_attempt_id_)
                                 .arg(QString::fromStdString(ev.type));

        if (ev.type == signal_msg::PeerJoined) {
            if (backend_session_id_.empty()) {
                qWarning().noquote() << QString("[viewer][attempt=%1] peer_joined received before backend session was established")
                                            .arg(session_attempt_id_);
                emit Disconnected("Falha ao iniciar sessao no backend");
                return;
            }
            if (HasActivePeerSession()) {
                if (peer_transport_connected_.load()) {
                    CancelRecoveryTimer();
                    emit StatusUpdated("Sinalizacao restabelecida");
                    return;
                }

                qWarning().noquote() << QString("[viewer][attempt=%1] stale peer session detected on peer_joined; rebuilding")
                                            .arg(session_attempt_id_);
                ResetPeerSession();
                if (backend_session_id_.empty()) {
                    qWarning().noquote() << QString("[viewer][attempt=%1] peer session reset cleared backend session before rebuild")
                                                .arg(session_attempt_id_);
                    emit Disconnected("Falha ao recuperar sessao");
                    return;
                }
            }
            StartPeerSession();
        } else if (ev.type == signal_msg::Answer) {
            if (!HasActivePeerSession()) {
                return;
            }
            SessionDescription desc;
            desc.type = "answer";
            desc.sdp = ev.payload;
            peer_.SetRemoteDescription(desc);
            ArmRecoveryTimer(kWaitingVideoTimeoutMs, "connecting_video");
        } else if (ev.type == signal_msg::ICECandidate) {
            if (!HasActivePeerSession()) {
                return;
            }
            IceCandidate ic;
            ic.candidate = ev.payload;
            peer_.AddIceCandidate(ic);
        } else if (ev.type == signal_msg::PeerLeft) {
            qWarning().noquote() << QString("[viewer][attempt=%1] peer_left received before/after transport machine_session=%2 backend_session=%3 backend_status=%4 relay_session=%5")
                                        .arg(session_attempt_id_)
                                        .arg(QString::fromStdString(
                                            remotex::core::RedactIdentifier(session_id_, "machine_session")))
                                        .arg(QString::fromStdString(
                                            remotex::core::RedactIdentifier(backend_session_id_, "backend_session")))
                                        .arg(QString::fromStdString(backend_session_status_))
                                        .arg(QString::fromStdString(
                                            remotex::core::RedactIdentifier(backend_relay_session_id_, "relay_session")));
            ResetPeerSession();
            emit Disconnected("Host disconnected");
        } else if (ev.type == signal_msg::Error) {
            if (peer_transport_connected_.load()) {
                qWarning().noquote() << QString("[viewer][attempt=%1] ignoring signaling error while transport connected: %2")
                                            .arg(session_attempt_id_)
                                            .arg(QString::fromStdString(ev.error));
                return;
            }
            qWarning().noquote() << QString("[viewer][attempt=%1] signaling error message before transport connected: %2")
                                        .arg(session_attempt_id_)
                                        .arg(QString::fromStdString(ev.error));
            ResetPeerSession();
            const QString error = ev.error.empty()
                ? "Falha na negociacao da sessao"
                : QString::fromStdString(ev.error);
            emit Disconnected(error);
        }
    }, Qt::QueuedConnection);
}

void NetworkWorker::OnError(const std::string& error) {
    QString err_msg = QString::fromStdString(error);
    QMetaObject::invokeMethod(this, [this, err_msg]() {
        if (shutting_down_.load()) {
            return;
        }

        qWarning().noquote() << QString("[viewer][attempt=%1] signaling error=%2")
                                    .arg(session_attempt_id_)
                                    .arg(err_msg);

        if (peer_transport_connected_.load()) {
            emit StatusUpdated("Sinalizacao instavel");
            ArmRecoveryTimer(kNegotiationTimeoutMs, "signaling_unstable");
            return;
        }

        if (HasActivePeerSession() || !backend_session_id_.empty()) {
            ResetPeerSession();
        }
        qWarning().noquote() << QString("[viewer][attempt=%1] emitting disconnected from signaling error before transport: %2")
                                    .arg(session_attempt_id_)
                                    .arg(err_msg);
        emit Disconnected(err_msg);
    }, Qt::QueuedConnection);
}

void NetworkWorker::OnIceCandidate(const remotex::core::IceCandidate& candidate) {
    if (QThread::currentThread() != thread()) {
        const auto candidate_copy = candidate;
        QMetaObject::invokeMethod(this, [this, candidate_copy]() {
            OnIceCandidate(candidate_copy);
        }, Qt::QueuedConnection);
        return;
    }
    qInfo().noquote() << QString("[viewer][attempt=%1] local ICE candidate emitted").arg(session_attempt_id_);
    signaling_.SendICECandidate(session_id_,
                                candidate.candidate,
                                candidate.sdp_mid,
                                candidate.sdp_mline_index);
}

void NetworkWorker::OnConnectionStateChanged(remotex::core::PeerConnectionState state) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, state]() {
            OnConnectionStateChanged(state);
        }, Qt::QueuedConnection);
        return;
    }
    qInfo().noquote() << QString("[viewer][attempt=%1] peer state=%2")
                             .arg(session_attempt_id_)
                             .arg(static_cast<int>(state));
    emit ConnectionStateChanged(static_cast<int>(state));
    if (state == remotex::core::PeerConnectionState::Connected) {
        peer_session_active_.store(true);
        peer_transport_connected_.store(true);
        video_frame_seen_ = false;
        emit StatusUpdated("Host conectado - aguardando video...");
        emit Connected();
        ArmRecoveryTimer(kWaitingVideoTimeoutMs, "waiting_video");
    } else if (state == remotex::core::PeerConnectionState::Connecting) {
        peer_session_active_.store(true);
        emit StatusUpdated("Estabelecendo canal de video...");
        ArmRecoveryTimer(kWaitingVideoTimeoutMs, "establishing_video");
    } else if (state == remotex::core::PeerConnectionState::Disconnected ||
               state == remotex::core::PeerConnectionState::Failed) {
        const bool had_session = peer_session_active_.exchange(false);
        const bool was_connected = peer_transport_connected_.exchange(false);
        file_transfer_.Shutdown();
        EndTrackedSession();
        CancelRecoveryTimer();
        video_frame_seen_ = false;

        if (shutting_down_.load() || (!had_session && !was_connected)) {
            return;
        }
        qWarning().noquote() << QString("[viewer][attempt=%1] peer transport lost state=%2 had_session=%3 was_connected=%4 reboot_mode=%5")
                                    .arg(session_attempt_id_)
                                    .arg(static_cast<int>(state))
                                    .arg(had_session ? "true" : "false")
                                    .arg(was_connected ? "true" : "false")
                                    .arg(reboot_reconnect_mode_ ? "true" : "false");
        if (reboot_reconnect_mode_) {
            QTimer::singleShot(10000, this, [this]() {
                if (!shutting_down_.load() && reboot_reconnect_mode_) {
                    RestartConnection("reboot");
                }
            });
            emit StatusUpdated("Host reiniciando - aguardando reconexao...");
            return;
        }
        emit StatusUpdated("Sessao perdida");
        emit Disconnected("Connection lost");
    } else if (state == remotex::core::PeerConnectionState::Closed) {
        peer_session_active_.store(false);
        peer_transport_connected_.store(false);
        CancelRecoveryTimer();
        video_frame_seen_ = false;
    }
}

void NetworkWorker::OnIceGatheringStateChanged(remotex::core::IceGatheringState /*state*/) {}

int64_t NetworkWorker::SteadyClockNowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

double NetworkWorker::DurationUsToMs(int64_t duration_us) {
    return static_cast<double>(duration_us) / 1000.0;
}

double NetworkWorker::SanitizedCrossClockDeltaMs(int64_t now_us, int64_t then_us) {
    if (then_us <= 0 || now_us <= then_us) {
        return -1.0;
    }

    const double delta_ms = DurationUsToMs(now_us - then_us);
    if (delta_ms < 0.0 || delta_ms > 10000.0) {
        return -1.0;
    }
    return delta_ms;
}

void NetworkWorker::UpdateHostDiagnostics(double capture_fps, double encoded_fps, double sent_fps) {
    diagnostics_summary_.host_capture_fps = capture_fps;
    diagnostics_summary_.host_encoded_fps = encoded_fps;
    diagnostics_summary_.host_sent_fps = sent_fps;
}

void NetworkWorker::EmitDiagnosticsSummary() {
    emit DiagnosticsSummaryUpdated(diagnostics_summary_);
}

void NetworkWorker::OnVideoFrame(const remotex::core::VideoFrame& frame) {
    if (!video_frame_seen_) {
        video_frame_seen_ = true;
        reboot_reconnect_mode_ = false;
        CancelRecoveryTimer();
        qInfo().noquote() << QString("[viewer][attempt=%1] first video frame received").arg(session_attempt_id_);
    }

    const int y_size = frame.stride_y * frame.height;
    const int u_size = frame.stride_u * (frame.height / 2);
    const int v_size = frame.stride_v * (frame.height / 2);
    auto packet = remotex::viewer::VideoFramePacketPtr::create();
    packet->y = QByteArray(y_size, '\0');
    packet->u = QByteArray(u_size, '\0');
    packet->v = QByteArray(v_size, '\0');
    std::memcpy(packet->y.data(), frame.data_y, static_cast<size_t>(y_size));
    std::memcpy(packet->u.data(), frame.data_u, static_cast<size_t>(u_size));
    std::memcpy(packet->v.data(), frame.data_v, static_cast<size_t>(v_size));
    packet->stride_y = frame.stride_y;
    packet->stride_u = frame.stride_u;
    packet->stride_v = frame.stride_v;
    packet->width = frame.width;
    packet->height = frame.height;
    packet->frame_id = frame.frame_id;
    packet->timestamp_us = frame.timestamp_us;
    packet->capture_ready_timestamp_us = frame.capture_ready_timestamp_us;
    packet->encode_ready_timestamp_us = frame.encode_ready_timestamp_us;
    packet->send_started_timestamp_us = frame.send_started_timestamp_us;
    packet->decode_ready_timestamp_us = SteadyClockNowMicros();

    ++frames_received_;
    bytes_received_ += static_cast<uint64_t>(y_size + u_size + v_size);
    last_frame_id_ = frame.frame_id;
    diagnostics_summary_.viewer_decode_ready_fps = diagnostics_summary_.viewer_arrival_fps;

    const double transport_ms =
        SanitizedCrossClockDeltaMs(packet->decode_ready_timestamp_us, frame.send_started_timestamp_us);
    if (transport_ms >= 0.0) {
        transport_time_total_ms_ += transport_ms;
    }

    const double frame_age_ms =
        SanitizedCrossClockDeltaMs(packet->decode_ready_timestamp_us, frame.capture_ready_timestamp_us);
    if (frame_age_ms >= 0.0) {
        frame_age_total_ms_ += frame_age_ms;
        frame_age_window_is_end_to_end_ = true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - frame_stats_window_started_ >= std::chrono::seconds(5)) {
        const double elapsed_s = std::chrono::duration<double>(now - frame_stats_window_started_).count();
        const double fps = elapsed_s > 0.0
            ? static_cast<double>(frames_received_) / elapsed_s
            : 0.0;
        const double mbps = elapsed_s > 0.0
            ? (static_cast<double>(bytes_received_) * 8.0) / (elapsed_s * 1000.0 * 1000.0)
            : 0.0;
        const double avg_transport_ms = frames_received_ > 0
            ? transport_time_total_ms_ / static_cast<double>(frames_received_)
            : 0.0;
        const double avg_frame_age_ms = frames_received_ > 0
            ? frame_age_total_ms_ / static_cast<double>(frames_received_)
            : 0.0;

        diagnostics_summary_.viewer_arrival_fps = fps;
        diagnostics_summary_.viewer_decode_ready_fps = fps;
        diagnostics_summary_.avg_transport_ms = avg_transport_ms;
        diagnostics_summary_.frame_age_ms = avg_frame_age_ms;
        diagnostics_summary_.frame_age_is_end_to_end = frame_age_window_is_end_to_end_;

        qInfo().noquote() << QString("[viewer][video] fps=%1 throughput_mbps=%2 size=%3x%4")
                                 .arg(fps, 0, 'f', 1)
                                 .arg(mbps, 0, 'f', 2)
                                 .arg(frame.width)
                                 .arg(frame.height);
        EmitDiagnosticsSummary();
        if (peer_session_active_.load()) {
            std::string feedback = "{\"type\":\"viewer_status\""
                ",\"frame_age_ms\":" + std::to_string(diagnostics_summary_.frame_age_ms)
                + ",\"upload_fps\":" + std::to_string(diagnostics_summary_.viewer_upload_fps)
                + ",\"render_fps\":" + std::to_string(diagnostics_summary_.viewer_render_fps)
                + ",\"avg_upload_ms\":" + std::to_string(diagnostics_summary_.avg_render_upload_ms)
                + ",\"dropped_fps\":" + std::to_string(diagnostics_summary_.viewer_dropped_fps)
                + "}";
            peer_.SendDataChannelMessage("control", feedback);
        }
        frames_received_ = 0;
        bytes_received_ = 0;
        transport_time_total_ms_ = 0.0;
        frame_age_total_ms_ = 0.0;
        frame_age_window_is_end_to_end_ = false;
        frame_stats_window_started_ = now;
    }

    emit VideoFrameReceived(packet);
}

void NetworkWorker::OnDataChannelMessage(const remotex::core::DataChannelMessage& message) {
    if (QThread::currentThread() != thread()) {
        const auto message_copy = message;
        QMetaObject::invokeMethod(this, [this, message_copy]() {
            OnDataChannelMessage(message_copy);
        }, Qt::QueuedConnection);
        return;
    }
    if (message.channel_label == "latency" && !message.is_binary) {
        std::string text(reinterpret_cast<const char*>(message.data.data()),
                         message.data.size());
        if (text.rfind("pong:", 0) == 0) {
            auto ts = std::stoull(text.substr(5));
            auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            int rtt = static_cast<int>(now - ts);
            emit LatencyUpdated(rtt);
        }
        return;
    }

    if (message.channel_label == "chat" && !message.is_binary) {
        std::string json(reinterpret_cast<const char*>(message.data.data()), message.data.size());
        auto extract = [&](const std::string& key) -> std::string {
            std::string needle = "\"" + key + "\":\"";
            auto pos = json.find(needle);
            if (pos == std::string::npos) return {};
            pos += needle.size();
            std::string value;
            for (size_t i = pos; i < json.size(); ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    char next = json[i + 1];
                    if (next == '"') { value += '"'; ++i; }
                    else if (next == '\\') { value += '\\'; ++i; }
                    else if (next == 'n') { value += '\n'; ++i; }
                    else if (next == 'r') { value += '\r'; ++i; }
                    else { value += next; ++i; }
                } else if (json[i] == '"') {
                    break;
                } else {
                    value += json[i];
                }
            }
            return value;
        };
        QString sender = QString::fromStdString(extract("from"));
        QString text = QString::fromStdString(extract("text"));
        if (!text.isEmpty()) {
            emit ChatMessageReceived(sender, text);
        }
        return;
    }

    if (message.channel_label == "control" && !message.is_binary) {
        std::string json(reinterpret_cast<const char*>(message.data.data()), message.data.size());
        HandleControlMessage(json);
        return;
    }

    if (!IsInteractiveSession()) {
        return;
    }

    if (message.channel_label == "files") {
        file_transfer_.OnDataReceived(message.data, message.is_binary);
    } else if (message.channel_label == "clipboard" && !message.is_binary) {
        CancelClipboardFileOffers(QStringLiteral("remote text clipboard superseded pending file clipboard transfer"));
        QString text = QString::fromUtf8(reinterpret_cast<const char*>(message.data.data()),
                                         static_cast<int>(message.data.size()));
        emit ClipboardReceived(text);
    }
}

void NetworkWorker::OnDataChannelOpen(const std::string& label) {
    if (QThread::currentThread() != thread()) {
        const auto label_copy = label;
        QMetaObject::invokeMethod(this, [this, label_copy]() {
            OnDataChannelOpen(label_copy);
        }, Qt::QueuedConnection);
        return;
    }
    if (label == "latency") {
        if (!ping_timer_) {
            ping_timer_ = new QTimer(this);
            ping_timer_->setInterval(2500);
            connect(ping_timer_, &QTimer::timeout, this, [this]() {
                if (!peer_session_active_.load()) return;
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                std::string payload = "ping:" + std::to_string(ms);
                peer_.SendDataChannelMessage("latency", payload);
            });
        }
        ping_timer_->start();
        return;
    }
    if (!IsInteractiveSession()) {
        return;
    }
    if (label == remotex::core::kFilesChannel) {
        file_transfer_.Initialize(&peer_, default_receive_dir_);
    }
}

void NetworkWorker::OnDataChannelClosed(const std::string& label) {
    if (QThread::currentThread() != thread()) {
        const auto label_copy = label;
        QMetaObject::invokeMethod(this, [this, label_copy]() {
            OnDataChannelClosed(label_copy);
        }, Qt::QueuedConnection);
        return;
    }
}
