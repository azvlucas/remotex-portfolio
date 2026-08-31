// RemoteX - viewer/diagnostics_window.cpp

#include "viewer/diagnostics_window.h"

#include <QCloseEvent>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace remotex::viewer {

namespace {

constexpr char kWindowStyleSheet[] =
    "QWidget#DiagnosticsWindow {"
    "  background-color: #1a1a2e;"
    "}"
    "QGroupBox {"
    "  color: #8892b0;"
    "  border: 1px solid #2a2a4a;"
    "  border-radius: 6px;"
    "  margin-top: 14px;"
    "  padding: 10px 8px 8px 8px;"
    "  font-weight: 600;"
    "  font-size: 11px;"
    "}"
    "QGroupBox::title {"
    "  subcontrol-origin: margin;"
    "  left: 10px;"
    "  padding: 0 6px;"
    "}"
    "QLabel {"
    "  color: #ccd6f6;"
    "  font-size: 12px;"
    "}"
    "QLabel[role=\"value\"] {"
    "  color: #64ffda;"
    "  font-family: Consolas, 'Courier New', monospace;"
    "  font-size: 13px;"
    "  font-weight: 600;"
    "}"
    "QLabel[role=\"header\"] {"
    "  color: #e6f1ff;"
    "  font-size: 14px;"
    "  font-weight: 700;"
    "}"
    "QLabel[role=\"bottleneck-ok\"] {"
    "  color: #64ffda;"
    "  font-size: 13px;"
    "  font-weight: 700;"
    "}"
    "QLabel[role=\"bottleneck-warn\"] {"
    "  color: #ffcb6b;"
    "  font-size: 13px;"
    "  font-weight: 700;"
    "}";

QString FormatMetric(double value, const QString& suffix = "") {
    if (value <= 0.0) return QStringLiteral("--");
    return QString::number(value, 'f', 1) + suffix;
}

QString FormatFps(double value) {
    return FormatMetric(value, " fps");
}

QString FormatMs(double value) {
    return FormatMetric(value, " ms");
}

QLabel* CreateValueLabel(QWidget* parent) {
    auto* label = new QLabel("--", parent);
    label->setProperty("role", "value");
    label->setMinimumWidth(80);
    return label;
}

void AddMetricRow(QGridLayout* grid, int row, const QString& name, QLabel* value_label) {
    auto* name_label = new QLabel(name, grid->parentWidget());
    grid->addWidget(name_label, row, 0);
    grid->addWidget(value_label, row, 1);
}

QGroupBox* CreateGroup(const QString& title, QWidget* parent) {
    auto* group = new QGroupBox(title, parent);
    return group;
}

} // namespace

DiagnosticsWindow::DiagnosticsWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint) {
    setObjectName("DiagnosticsWindow");
    setWindowTitle("Diagnosticos da Sessao");
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(340);

    BuildLayout();
    ApplyStyle();
    RefreshDisplay();
}

void DiagnosticsWindow::BuildLayout() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(12, 12, 12, 12);
    main_layout->setSpacing(6);

    // Header: bottleneck status
    bottleneck_label_ = new QLabel("Status: --", this);
    bottleneck_label_->setProperty("role", "header");
    main_layout->addWidget(bottleneck_label_);

    // ---- Capture group ----
    auto* capture_group = CreateGroup("Captura (Host)", this);
    auto* capture_grid = new QGridLayout(capture_group);
    capture_grid->setColumnStretch(1, 1);
    capture_fps_label_ = CreateValueLabel(capture_group);
    AddMetricRow(capture_grid, 0, "Capture FPS", capture_fps_label_);
    main_layout->addWidget(capture_group);

    // ---- Encode / Transport group ----
    auto* encode_group = CreateGroup("Encode / Transporte", this);
    auto* encode_grid = new QGridLayout(encode_group);
    encode_grid->setColumnStretch(1, 1);
    encoded_fps_label_ = CreateValueLabel(encode_group);
    sent_fps_label_ = CreateValueLabel(encode_group);
    transport_ms_label_ = CreateValueLabel(encode_group);
    AddMetricRow(encode_grid, 0, "Encoded FPS", encoded_fps_label_);
    AddMetricRow(encode_grid, 1, "Sent FPS", sent_fps_label_);
    AddMetricRow(encode_grid, 2, "Transport", transport_ms_label_);
    main_layout->addWidget(encode_group);

    // ---- Network / Arrival group ----
    auto* network_group = CreateGroup("Rede / Chegada", this);
    auto* network_grid = new QGridLayout(network_group);
    network_grid->setColumnStretch(1, 1);
    arrival_fps_label_ = CreateValueLabel(network_group);
    AddMetricRow(network_grid, 0, "Arrival FPS", arrival_fps_label_);
    main_layout->addWidget(network_group);

    // ---- Viewer / Decode / Render group ----
    auto* viewer_group = CreateGroup("Viewer (Decode / Render)", this);
    auto* viewer_grid = new QGridLayout(viewer_group);
    viewer_grid->setColumnStretch(1, 1);
    decode_ready_fps_label_ = CreateValueLabel(viewer_group);
    render_fps_label_ = CreateValueLabel(viewer_group);
    upload_fps_label_ = CreateValueLabel(viewer_group);
    dropped_fps_label_ = CreateValueLabel(viewer_group);
    AddMetricRow(viewer_grid, 0, "Decode Ready FPS", decode_ready_fps_label_);
    AddMetricRow(viewer_grid, 1, "Render FPS", render_fps_label_);
    AddMetricRow(viewer_grid, 2, "Upload FPS", upload_fps_label_);
    AddMetricRow(viewer_grid, 3, "Dropped FPS", dropped_fps_label_);
    main_layout->addWidget(viewer_group);

    // ---- Timing group ----
    auto* timing_group = CreateGroup("Tempos", this);
    auto* timing_grid = new QGridLayout(timing_group);
    timing_grid->setColumnStretch(1, 1);
    decode_queue_ms_label_ = CreateValueLabel(timing_group);
    render_upload_ms_label_ = CreateValueLabel(timing_group);
    present_delay_ms_label_ = CreateValueLabel(timing_group);
    frame_age_label_ = CreateValueLabel(timing_group);
    AddMetricRow(timing_grid, 0, "Decode Queue", decode_queue_ms_label_);
    AddMetricRow(timing_grid, 1, "Render Upload", render_upload_ms_label_);
    AddMetricRow(timing_grid, 2, "Present Delay", present_delay_ms_label_);
    AddMetricRow(timing_grid, 3, "Frame Age", frame_age_label_);
    main_layout->addWidget(timing_group);

    main_layout->addStretch();
}

void DiagnosticsWindow::ApplyStyle() {
    setStyleSheet(kWindowStyleSheet);
}

void DiagnosticsWindow::UpdateDiagnostics(const SessionDiagnosticsSummary& summary) {
    summary_ = summary;
    RefreshDisplay();
}

void DiagnosticsWindow::RefreshDisplay() {
    // Bottleneck header
    const QString bottleneck_text = SessionDiagnosticsBottleneckLabel(summary_.bottleneck);
    const bool is_ok = summary_.bottleneck == SessionDiagnosticsBottleneck::Unknown;
    bottleneck_label_->setText(QString("Diagnostico: %1").arg(bottleneck_text));
    bottleneck_label_->setProperty("role", is_ok ? "bottleneck-ok" : "bottleneck-warn");
    bottleneck_label_->style()->unpolish(bottleneck_label_);
    bottleneck_label_->style()->polish(bottleneck_label_);

    // Capture
    capture_fps_label_->setText(FormatFps(summary_.host_capture_fps));

    // Encode / Transport
    encoded_fps_label_->setText(FormatFps(summary_.host_encoded_fps));
    sent_fps_label_->setText(FormatFps(summary_.host_sent_fps));
    transport_ms_label_->setText(FormatMs(summary_.avg_transport_ms));

    // Network
    arrival_fps_label_->setText(FormatFps(summary_.viewer_arrival_fps));

    // Viewer
    decode_ready_fps_label_->setText(FormatFps(summary_.viewer_decode_ready_fps));
    render_fps_label_->setText(FormatFps(summary_.viewer_render_fps));
    upload_fps_label_->setText(FormatFps(summary_.viewer_upload_fps));
    dropped_fps_label_->setText(FormatFps(summary_.viewer_dropped_fps));

    // Timing
    decode_queue_ms_label_->setText(FormatMs(summary_.avg_decode_queue_ms));
    render_upload_ms_label_->setText(FormatMs(summary_.avg_render_upload_ms));
    present_delay_ms_label_->setText(FormatMs(summary_.avg_present_delay_ms));

    const QString age_prefix = summary_.frame_age_is_end_to_end
        ? "Frame Age" : "Viewer Age";
    frame_age_label_->setText(
        summary_.frame_age_ms > 0.0
            ? QString("%1: %2 ms").arg(age_prefix).arg(summary_.frame_age_ms, 0, 'f', 1)
            : "--");
}

void DiagnosticsWindow::closeEvent(QCloseEvent* event) {
    event->accept();
}

} // namespace remotex::viewer
