#pragma once
// RemoteX - viewer/video_renderer.h

#include <QByteArray>
#include <QMutex>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLWidget>
#include <QSharedPointer>

#include <chrono>
#include <cstdint>
#include <memory>

namespace remotex::viewer {

struct SessionDisplayContract {
    int adapter_index = 0;
    int output_index = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;

    [[nodiscard]] bool operator==(const SessionDisplayContract& other) const = default;
};

struct VideoFramePacket {
    QByteArray y;
    QByteArray u;
    QByteArray v;
    int stride_y = 0;
    int stride_u = 0;
    int stride_v = 0;
    int width = 0;
    int height = 0;
    uint64_t frame_id = 0;
    int64_t timestamp_us = 0;
    int64_t capture_ready_timestamp_us = 0;
    int64_t encode_ready_timestamp_us = 0;
    int64_t send_started_timestamp_us = 0;
    int64_t decode_ready_timestamp_us = 0;
};

using VideoFramePacketPtr = QSharedPointer<VideoFramePacket>;

struct RendererDiagnosticsSnapshot {
    uint64_t frame_id = 0;
    double submit_fps = 0.0;
    double upload_fps = 0.0;
    double avg_upload_ms = 0.0;
    double avg_present_delay_ms = 0.0;
    double avg_frame_age_ms = 0.0;
    double viewer_dropped_fps = 0.0;
    bool frame_age_is_end_to_end = false;
};

struct RendererFrameState {
    int frame_width = 0;
    int frame_height = 0;
    bool has_pending_frame = false;
    bool has_displayed_frame = false;
    bool textures_created = false;
    int texture_width = 0;
    int texture_height = 0;
};

inline bool RequiresRendererResetForDisplayChange(
    const SessionDisplayContract& current_display,
    const SessionDisplayContract& next_display,
    const RendererFrameState& renderer_state) {
    if (current_display == next_display) {
        return false;
    }

    if (renderer_state.has_pending_frame || renderer_state.has_displayed_frame) {
        return true;
    }

    if (renderer_state.textures_created &&
        (renderer_state.texture_width != next_display.width ||
         renderer_state.texture_height != next_display.height)) {
        return true;
    }

    return current_display.width != next_display.width ||
           current_display.height != next_display.height ||
           current_display.rotation != next_display.rotation;
}

class VideoRenderer : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT

public:
    explicit VideoRenderer(QWidget* parent = nullptr);
    ~VideoRenderer() override;

    void SubmitFrame(const VideoFramePacketPtr& frame);
    int FrameWidth() const { return frame_width_; }
    int FrameHeight() const { return frame_height_; }
    int DisplayWidth() const { return frame_width_; }
    int DisplayHeight() const { return frame_height_; }

    [[nodiscard]] RendererFrameState SnapshotFrameState() const;
    void ResetFrameState();

signals:
    void FrameSizeChanged(int width, int height);
    void DiagnosticsUpdated(const remotex::viewer::RendererDiagnosticsSnapshot& snapshot);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void CreateTextures(int width, int height);
    void UploadTextures(const VideoFramePacket& frame);

    std::unique_ptr<QOpenGLShaderProgram> shader_;

    GLuint tex_y_ = 0;
    GLuint tex_u_ = 0;
    GLuint tex_v_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    mutable QMutex frame_mutex_;
    VideoFramePacketPtr pending_frame_;
    VideoFramePacketPtr displayed_frame_;
    int frame_width_ = 0;
    int frame_height_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    bool frame_dirty_ = false;
    bool textures_created_ = false;
    uint64_t frames_submitted_ = 0;
    uint64_t frames_uploaded_ = 0;
    double present_delay_total_ms_ = 0.0;
    double frame_age_total_ms_ = 0.0;
    bool frame_age_window_is_end_to_end_ = false;
    uint64_t last_frame_id_ = 0;
    double upload_time_total_ms_ = 0.0;
    std::chrono::steady_clock::time_point stats_window_started_ =
        std::chrono::steady_clock::now();
};

} // namespace remotex::viewer

Q_DECLARE_METATYPE(remotex::viewer::VideoFramePacketPtr)
Q_DECLARE_METATYPE(remotex::viewer::RendererDiagnosticsSnapshot)
