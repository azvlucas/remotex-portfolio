#pragma once
// RemoteX - viewer/diagnostics_window.h
// Separate diagnostics window (Ctrl+Shift+T) with terminal-style dark theme.

#include "viewer/remote_session_window.h"

#include <QLabel>
#include <QTimer>
#include <QWidget>

namespace remotex::viewer {

class DiagnosticsWindow : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsWindow(QWidget* parent = nullptr);
    ~DiagnosticsWindow() override = default;

    void UpdateDiagnostics(const SessionDiagnosticsSummary& summary);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void BuildLayout();
    void ApplyStyle();
    void RefreshDisplay();

    SessionDiagnosticsSummary summary_;

    // Header
    QLabel* bottleneck_label_ = nullptr;

    // Capture group
    QLabel* capture_fps_label_ = nullptr;

    // Encode/Transport group
    QLabel* encoded_fps_label_ = nullptr;
    QLabel* sent_fps_label_ = nullptr;
    QLabel* transport_ms_label_ = nullptr;

    // Network/Arrival group
    QLabel* arrival_fps_label_ = nullptr;

    // Viewer/Decode group
    QLabel* decode_ready_fps_label_ = nullptr;
    QLabel* render_fps_label_ = nullptr;
    QLabel* upload_fps_label_ = nullptr;
    QLabel* dropped_fps_label_ = nullptr;

    // Timing group
    QLabel* decode_queue_ms_label_ = nullptr;
    QLabel* render_upload_ms_label_ = nullptr;
    QLabel* present_delay_ms_label_ = nullptr;
    QLabel* frame_age_label_ = nullptr;
};

} // namespace remotex::viewer
