#pragma once
// RemoteX - viewer/remote_session_window.h

#include "viewer/connection_settings.h"
#include "core/auth_client.h"
#include "core/file_transfer.h"
#include "core/machine_client.h"
#include "core/signaling_client.h"
#include "core/webrtc_peer.h"
#include "viewer/input_capture.h"
#include "viewer/video_renderer.h"

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QMetaType>
#include <QPushButton>
#include <QProgressBar>
#include <QShortcut>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QVariantMap>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <map>
#include <vector>

namespace remotex::viewer {

class ChatPanel;
class DiagnosticsWindow;
class FileTransferDialog;

struct RemoteFileItem {
    QString name;
    QString absolute_path;
    bool    is_directory = false;
    bool    is_access_denied = false;
    quint64 size_bytes = 0;
    qint64  modified_unix_sec = 0;
};

struct RemoteListingResult {
    quint64             request_id = 0;
    QString             path;
    bool                is_root_listing = false;
    bool                success = false;
    QString             error;
    QList<RemoteFileItem> entries;
};

struct RemoteControlToggleStatus {
    bool enabled = false;
    QString error;
};

struct SecureAttentionStatus {
    bool acknowledged = false;
    QString error;
};

enum class SessionDiagnosticsBottleneck {
    Unknown,
    Capture,
    EncodeTransport,
    DecodeRender,
    Viewer,
};

struct SessionDiagnosticsSummary {
    double host_capture_fps = 0.0;
    double host_encoded_fps = 0.0;
    double host_sent_fps = 0.0;
    double viewer_arrival_fps = 0.0;
    double viewer_decode_ready_fps = 0.0;
    double viewer_render_fps = 0.0;
    double viewer_upload_fps = 0.0;
    double avg_transport_ms = 0.0;
    double avg_decode_queue_ms = 0.0;
    double avg_render_upload_ms = 0.0;
    double frame_age_ms = 0.0;
    bool frame_age_is_end_to_end = false;
    double viewer_dropped_fps = 0.0;
    double avg_present_delay_ms = 0.0;
    SessionDiagnosticsBottleneck bottleneck = SessionDiagnosticsBottleneck::Unknown;
};

inline QString SessionDiagnosticsBottleneckLabel(SessionDiagnosticsBottleneck bottleneck) {
    switch (bottleneck) {
    case SessionDiagnosticsBottleneck::Capture:
        return QStringLiteral("capture");
    case SessionDiagnosticsBottleneck::EncodeTransport:
        return QStringLiteral("encode_transport");
    case SessionDiagnosticsBottleneck::DecodeRender:
        return QStringLiteral("decode_render");
    case SessionDiagnosticsBottleneck::Viewer:
        return QStringLiteral("viewer");
    case SessionDiagnosticsBottleneck::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

inline SessionDiagnosticsBottleneck ClassifySessionDiagnostics(
    const SessionDiagnosticsSummary& summary) {
    constexpr double kHealthyFrameAgeMs = 45.0;
    constexpr double kHighFrameAgeMs = 120.0;
    constexpr double kLowFpsFloor = 20.0;
    constexpr double kViewerSlowFpsFloor = 18.0;

    const double host_capture = summary.host_capture_fps;
    const double host_encoded = summary.host_encoded_fps;
    const double host_sent = summary.host_sent_fps;
    const double viewer_arrival = summary.viewer_arrival_fps;
    const double viewer_decode = summary.viewer_decode_ready_fps;
    const double viewer_render = summary.viewer_render_fps;
    const double viewer_upload = summary.viewer_upload_fps;

    if (host_capture > 0.0 &&
        host_capture <= std::max({host_encoded, host_sent, viewer_arrival, viewer_decode, viewer_render, viewer_upload}) - 3.0 &&
        host_capture < kLowFpsFloor) {
        return SessionDiagnosticsBottleneck::Capture;
    }

    if (summary.frame_age_ms > kHealthyFrameAgeMs) {
        if ((host_encoded > 0.0 && host_encoded < host_capture - 3.0) ||
            (host_sent > 0.0 && host_sent < host_capture - 3.0) ||
            (viewer_arrival > 0.0 && viewer_arrival < host_sent - 3.0)) {
            return SessionDiagnosticsBottleneck::EncodeTransport;
        }

        if ((viewer_decode > 0.0 && viewer_decode < viewer_arrival - 3.0) ||
            (viewer_render > 0.0 && viewer_render < viewer_decode - 2.0)) {
            return SessionDiagnosticsBottleneck::DecodeRender;
        }

        if ((viewer_upload > 0.0 && viewer_upload < viewer_render - 2.0) ||
            (summary.avg_render_upload_ms >= 12.0 && summary.frame_age_ms >= kHighFrameAgeMs) ||
            viewer_upload < kViewerSlowFpsFloor) {
            return SessionDiagnosticsBottleneck::Viewer;
        }
    }

    return SessionDiagnosticsBottleneck::Unknown;
}

inline bool ShouldResetRendererForDisplayContractUpdate(
    const std::optional<SessionDisplayContract>& current_display,
    const RendererFrameState& renderer_state,
    const SessionDisplayContract& next_display) {
    // Video and control metadata travel on separate transport lanes, so the
    // initial monitor contract may arrive after the first rotated frame.
    // The Host owns rotated bootstrap now; the Viewer must not blank a good
    // first frame just because the initial contract arrived later.
    if (!current_display.has_value()) {
        return false;
    }

    return RequiresRendererResetForDisplayChange(
        *current_display,
        next_display,
        renderer_state);
}

inline std::optional<SessionDisplayContract> ParseMonitorChangedContract(
    const std::string& json) {
    auto extractInt = [&](const std::string& key) -> std::optional<int> {
        const std::string needle = "\"" + key + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) {
            return std::nullopt;
        }

        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ') {
            ++pos;
        }

        char* end = nullptr;
        const long value = std::strtol(json.c_str() + pos, &end, 10);
        if (end == json.c_str() + pos) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    };

    const auto adapter_index = extractInt("adapter_index");
    const auto output_index = extractInt("output_index");
    const auto width = extractInt("width");
    const auto height = extractInt("height");
    const auto rotation = extractInt("rotation");
    if (!adapter_index || !output_index || !width || !height) {
        return std::nullopt;
    }

    return SessionDisplayContract{
        .adapter_index = *adapter_index,
        .output_index = *output_index,
        .width = *width,
        .height = *height,
        .rotation = rotation.value_or(0),
    };
}

} // namespace remotex::viewer

Q_DECLARE_METATYPE(remotex::viewer::RemoteFileItem)
Q_DECLARE_METATYPE(QList<remotex::viewer::RemoteFileItem>)
Q_DECLARE_METATYPE(remotex::viewer::RemoteListingResult)
Q_DECLARE_METATYPE(remotex::viewer::RemoteControlToggleStatus)
Q_DECLARE_METATYPE(remotex::viewer::SecureAttentionStatus)
Q_DECLARE_METATYPE(remotex::viewer::SessionDiagnosticsSummary)
Q_DECLARE_METATYPE(remotex::viewer::SessionDisplayContract)

namespace remotex::viewer {

class NetworkWorker : public QObject,
                      public remotex::core::SignalingListener,
                      public remotex::core::WebRTCPeerListener,
                      public remotex::core::FileTransferListener {
    Q_OBJECT

public:
    explicit NetworkWorker(remotex::core::AuthClient* auth,
                           remotex::core::SessionAccessMode access_mode,
                           const remotex::viewer::ConnectionSettings& connection_settings,
                           QObject* parent = nullptr);
    ~NetworkWorker() override;

    remotex::core::WebRTCPeer* Peer() { return &peer_; }

public slots:
    void ConnectToMachine(const QString& machine_id,
                          const QString& signaling_host, quint16 signaling_port);
    void Disconnect();
    void RequestRemoteRoots();
    void RequestRemoteListing(const QString& remote_path);
    void SendPaths(const QStringList& local_paths, const QString& remote_destination_root);
    void ReceivePaths(const QStringList& remote_paths, const QString& local_destination_root);
    void ResolveTransferPreview(const QString& job_id, bool allow_overwrite);
    void SendClipboardText(const QString& text);
    void PublishLocalClipboardFiles(const QStringList& local_paths);
    void TriggerClipboardPaste();
    void SendChatMessage(const QString& text);
    void SelectMonitor(int adapter_index, int output_index);
    void SendRebootRequest();
    void RequestSecureAttention();
    void SendNumLockPolicy(bool enabled);
    void SetPrivacyMode(bool enabled);
    void SetBlockInput(bool enabled);
    void SendRefreshRequest();

signals:
    void Connected();
    void Disconnected(const QString& reason);
    void ConnectionStateChanged(int state);

    void VideoFrameReceived(const remotex::viewer::VideoFramePacketPtr& frame);
    void DiagnosticsSummaryUpdated(const remotex::viewer::SessionDiagnosticsSummary& summary);

    void RemoteListingReady(const remotex::viewer::RemoteListingResult& listing);
    void TransferConflictPreviewReady(const QString& job_id, bool receiving, const QString& summary);
    void TransferConflictPreviewFailed(const QString& job_id, const QString& outcome, const QString& message);
    void TransferJobStarted(const QString& job_id, const QString& destination_root,
                            quint64 total_bytes, int total_entries, bool receiving);
    void TransferJobProgress(const QString& job_id, const QString& current_item,
                             quint64 bytes_done, quint64 total_bytes,
                             int entries_done, int total_entries);
    void TransferJobFinished(const QString& job_id, bool success,
                             const QString& destination_root, const QString& message);

    void ClipboardReceived(const QString& text);
    void ClipboardFilePastePendingChanged(bool pending);
    void ChatMessageReceived(const QString& sender, const QString& text);
    void LatencyUpdated(int ms);
    void StatusUpdated(const QString& msg);
    void MonitorListReceived(const QList<QVariantMap>& monitors);
    void MonitorChanged(const remotex::viewer::SessionDisplayContract& contract);
    void RebootAcknowledged();
    void RebootFailed(const QString& error);
    void SecureAttentionStatusChanged(const remotex::viewer::SecureAttentionStatus& status);
    void PrivacyModeStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status);
    void BlockInputStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status);

public:
    bool IsRebootReconnectMode() const { return reboot_reconnect_mode_; }

private:
    struct ClipboardFileOffer {
        QString token;
        QStringList source_paths;
        QStringList entry_names;
        bool paste_requested = false;
    };

    static int64_t SteadyClockNowMicros();
    static double DurationUsToMs(int64_t duration_us);
    static double SanitizedCrossClockDeltaMs(int64_t now_us, int64_t then_us);
    static QStringList TopLevelClipboardEntryNames(const QStringList& paths);
    void EmitDiagnosticsSummary();
    void UpdateHostDiagnostics(double capture_fps, double encoded_fps, double sent_fps);

    bool IsInteractiveSession() const;
    bool HasActivePeerSession() const;
    void UpdateRelayBundle(const remotex::core::SessionInfo& session);
    void ResetPeerSession();
    void StartPeerSession();
    void EnsureRecoveryTimer();
    void ArmRecoveryTimer(int interval_ms, const QString& phase);
    void CancelRecoveryTimer();
    void RestartConnection(const QString& reason);
    bool StartTrackedSession(QString* error_message = nullptr);
    void EndTrackedSession();
    void RecordTransferAuditEvent(const std::string& event_type,
                                  const remotex::core::TransferJobInfo& info,
                                  const std::string& status,
                                  const std::string& error = "");
    void CancelClipboardFileOffers(const QString& reason);
    void UpdateClipboardFilePasteAvailability();
    QString GenerateClipboardTransferToken();
    void HandleClipboardFilesOffer(const std::string& json);
    void HandleClipboardFilesAccept(const std::string& json);
    void HandleClipboardFilesCancel(const std::string& json);

    void OnConnected() override;
    void OnDisconnected() override;
    void OnSignalingMessage(const remotex::core::SignalingEvent& event) override;
    void OnError(const std::string& error) override;

    void OnLocalDescription(const remotex::core::SessionDescription& desc) override;
    void OnIceCandidate(const remotex::core::IceCandidate& candidate) override;
    void OnConnectionStateChanged(remotex::core::PeerConnectionState state) override;
    void OnIceGatheringStateChanged(remotex::core::IceGatheringState state) override;
    void OnVideoFrame(const remotex::core::VideoFrame& frame) override;
    void OnDataChannelMessage(const remotex::core::DataChannelMessage& message) override;
    void OnDataChannelOpen(const std::string& label) override;
    void OnDataChannelClosed(const std::string& label) override;

    void HandleControlMessage(const std::string& json);

    void OnRemoteListing(const remotex::core::RemoteFsListing& listing) override;
    void OnTransferConflictPreviewReady(const remotex::core::TransferConflictPreview& preview) override;
    void OnTransferConflictPreviewFailed(const remotex::core::TransferJobInfo& info,
                                         const std::string& error) override;
    void OnTransferJobStarted(const remotex::core::TransferJobInfo& info) override;
    void OnTransferJobProgress(const remotex::core::TransferJobInfo& job,
                               const remotex::core::TransferEntryInfo& entry) override;
    void OnTransferJobCompleted(const remotex::core::TransferJobInfo& info,
                                const std::filesystem::path& destination_root) override;
    void OnTransferJobFailed(const remotex::core::TransferJobInfo& info,
                             const std::string& error) override;
    void OnTransferJobCancelled(const remotex::core::TransferJobInfo& info) override;

    remotex::core::AuthClient* auth_;
    remotex::core::SessionAccessMode access_mode_ = remotex::core::SessionAccessMode::Interactive;
    remotex::viewer::ConnectionSettings connection_settings_;
    remotex::core::MachineClient machine_client_;
    remotex::core::SignalingClient signaling_;
    remotex::core::WebRTCPeer peer_;
    remotex::core::FileTransfer file_transfer_;
    std::string machine_id_;
    std::string session_id_;
    std::string backend_session_id_;
    std::string backend_session_status_;
    int         backend_relay_protocol_version_ = 0;
    std::string backend_relay_session_id_;
    std::string backend_relay_role_;
    std::string backend_relay_ticket_;
    int64_t     backend_relay_ticket_expires_unix_ = 0;
    std::string backend_relay_host_;
    uint16_t    backend_relay_port_ = 0;
    std::filesystem::path default_receive_dir_;
    QString signaling_host_;
    quint16 signaling_port_ = 0;
    QTimer* recovery_timer_ = nullptr;
    QTimer* ping_timer_ = nullptr;
    QTimer* session_heartbeat_timer_ = nullptr;
    QString recovery_phase_;
    int auto_recovery_attempts_ = 0;
    quint64 session_attempt_id_ = 0;
    bool restart_in_progress_ = false;
    bool video_frame_seen_ = false;
    std::atomic<bool> peer_session_active_{false};
    std::atomic<bool> peer_transport_connected_{false};
    std::atomic<bool> shutting_down_{false};
    bool reboot_reconnect_mode_ = false;
    uint64_t frames_received_ = 0;
    uint64_t bytes_received_ = 0;
    double transport_time_total_ms_ = 0.0;
    double frame_age_total_ms_ = 0.0;
    bool frame_age_window_is_end_to_end_ = false;
    uint64_t last_frame_id_ = 0;
    SessionDiagnosticsSummary diagnostics_summary_;
    std::chrono::steady_clock::time_point frame_stats_window_started_ = std::chrono::steady_clock::now();
    quint64 clipboard_transfer_counter_ = 0;
    std::map<std::string, ClipboardFileOffer> pending_outgoing_clipboard_offers_;
    std::map<std::string, ClipboardFileOffer> pending_incoming_clipboard_offers_;
};

class RemoteSessionWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit RemoteSessionWindow(remotex::core::AuthClient* auth,
                                 const QString& machine_id,
                                 const QString& machine_name,
                                 remotex::core::SessionAccessMode access_mode,
                                 QWidget* parent = nullptr);
    ~RemoteSessionWindow() override;

signals:
    void SessionEnded();

private slots:
    void OnConnected();
    void OnDisconnected(const QString& reason);
    void OnVideoFrame(const remotex::viewer::VideoFramePacketPtr& frame);
    void OnTransferFile();
    void OnSendCtrlAltDel();
    void OnToggleFullscreen();
    void OnDisconnectClicked();
    void OnLatencyUpdated(int ms);
    void OnStatusUpdated(const QString& msg);
    void OnRetry();
    void OnMonitorListReceived(const QList<QVariantMap>& monitors);
    void OnMonitorSelected(int index);
    void OnMonitorChanged(const remotex::viewer::SessionDisplayContract& contract);
    void OnRebootClicked();
    void OnRebootAcknowledged();
    void OnRebootFailed(const QString& error);
    void OnSecureAttentionStatusChanged(const remotex::viewer::SecureAttentionStatus& status);
    void OnTogglePrivacyMode();
    void OnLocalNumLockStateChanged(bool enabled);
    void OnPrivacyModeStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status);
    void OnToggleBlockInput();
    void OnBlockInputStatusChanged(const remotex::viewer::RemoteControlToggleStatus& status);
    void OnDiagnosticsSummaryUpdated(const remotex::viewer::SessionDiagnosticsSummary& summary);
    void OnRendererDiagnosticsUpdated(const remotex::viewer::RendererDiagnosticsSnapshot& snapshot);
    void OnClipboardFilePasteShortcut();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    bool IsInteractiveSession() const;
    void SetupUI();
    void SetupToolbar();
    void SetupFullscreenOverlay();
    void SetupTransferToast();
    void UpdateDiagnosticsSummary();
    void UpdateDiagnosticsPanel();
    void OnToggleDiagnosticsWindow();
    void UpdateFullscreenPresentation();
    void UpdateFullscreenActionHints();
    void UpdateFullscreenOverlayTexts();
    void UpdateFullscreenOverlayGeometry();
    void ShowFullscreenOverlayTemporarily();
    void HideFullscreenOverlay();
    void UpdateTransferToastGeometry();
    void ShowTransferToastStarted(const QString& job_id,
                                  const QString& destination_root,
                                  quint64 total_bytes,
                                  int total_entries,
                                  bool receiving);
    void ShowTransferToastProgress(const QString& job_id,
                                   const QString& current_item,
                                   quint64 bytes_done,
                                   quint64 total_bytes,
                                   int entries_done,
                                   int total_entries);
    void ShowTransferToastFinished(const QString& job_id,
                                   bool success,
                                   const QString& destination_root,
                                   const QString& message);
    void HideTransferToast();

    remotex::core::AuthClient* auth_;
    QString machine_id_;
    remotex::core::SessionAccessMode access_mode_ = remotex::core::SessionAccessMode::Interactive;
    ConnectionSettings connection_settings_;

    VideoRenderer* renderer_ = nullptr;
    InputCapture* input_capture_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QLabel* latency_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    SessionDiagnosticsSummary diagnostics_summary_;
    DiagnosticsWindow* diagnostics_window_ = nullptr;
    QShortcut* diagnostics_shortcut_ = nullptr;

    QAction* act_transfer_ = nullptr;
    QAction* act_chat_ = nullptr;
    QAction* act_cad_ = nullptr;
    QAction* act_reboot_ = nullptr;
    QAction* act_privacy_ = nullptr;
    QAction* act_block_input_ = nullptr;
    QAction* act_fullscreen_ = nullptr;
    QAction* act_disconnect_ = nullptr;
    QShortcut* fullscreen_shortcut_ = nullptr;
    QShortcut* file_paste_shortcut_ = nullptr;

    QThread* network_thread_ = nullptr;
    NetworkWorker* network_worker_ = nullptr;
    bool is_fullscreen_ = false;
    QFrame* fullscreen_overlay_ = nullptr;
    QLabel* fullscreen_overlay_status_label_ = nullptr;
    QLabel* fullscreen_overlay_latency_label_ = nullptr;
    QTimer* fullscreen_overlay_timer_ = nullptr;
    QFrame* transfer_toast_ = nullptr;
    QLabel* transfer_toast_title_label_ = nullptr;
    QLabel* transfer_toast_body_label_ = nullptr;
    QProgressBar* transfer_toast_progress_bar_ = nullptr;
    QTimer* transfer_toast_timer_ = nullptr;
    QString transfer_toast_job_id_;

    QTimer* retry_timer_ = nullptr;
    QTimer* video_wait_timer_ = nullptr;
    QPushButton* retry_btn_ = nullptr;
    bool secure_attention_request_pending_ = false;
    bool privacy_mode_request_pending_ = false;
    bool block_input_request_pending_ = false;
    bool privacy_mode_active_ = false;
    bool block_input_active_ = false;
    bool is_updating_clipboard_ = false;
    bool received_video_frame_ = false;
    ChatPanel* chat_panel_ = nullptr;
    FileTransferDialog* transfer_dialog_ = nullptr;

    QComboBox* monitor_combo_ = nullptr;
    QList<QVariantMap> available_monitors_;
    std::optional<SessionDisplayContract> current_display_contract_;
    std::optional<bool> pending_num_lock_policy_;
};

} // namespace remotex::viewer
