// RemoteX - viewer/video_renderer.cpp
// QOpenGLWidget com texturas Y/U/V para renderizacao I420.
// Mantem aspect ratio com letterboxing.

#include "viewer/video_renderer.h"

#include <QDebug>
#include <QOpenGLExtraFunctions>

#include <algorithm>

namespace remotex::viewer {

static const char* kVertexShader = R"(
    #version 330 core
    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec2 aTexCoord;
    out vec2 vTexCoord;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        vTexCoord = aTexCoord;
    }
)";

static const char* kFragmentShader = R"(
    #version 330 core
    in vec2 vTexCoord;
    out vec4 fragColor;
    uniform sampler2D texY;
    uniform sampler2D texU;
    uniform sampler2D texV;
    void main() {
        float y = texture(texY, vTexCoord).r;
        float u = texture(texU, vTexCoord).r - 0.5;
        float v = texture(texV, vTexCoord).r - 0.5;
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        fragColor = vec4(clamp(r, 0.0, 1.0),
                         clamp(g, 0.0, 1.0),
                         clamp(b, 0.0, 1.0), 1.0);
    }
)";

static const float kQuadVertices[] = {
    -1.0f,  1.0f,  0.0f, 0.0f,
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

VideoRenderer::VideoRenderer(QWidget* parent)
    : QOpenGLWidget(parent) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

VideoRenderer::~VideoRenderer() {
    makeCurrent();
    if (tex_y_) {
        glDeleteTextures(1, &tex_y_);
    }
    if (tex_u_) {
        glDeleteTextures(1, &tex_u_);
    }
    if (tex_v_) {
        glDeleteTextures(1, &tex_v_);
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
    }
    doneCurrent();
}

RendererFrameState VideoRenderer::SnapshotFrameState() const {
    QMutexLocker lock(&frame_mutex_);
    return RendererFrameState{
        .frame_width = frame_width_,
        .frame_height = frame_height_,
        .has_pending_frame = !pending_frame_.isNull(),
        .has_displayed_frame = !displayed_frame_.isNull(),
        .textures_created = textures_created_,
        .texture_width = texture_width_,
        .texture_height = texture_height_,
    };
}

void VideoRenderer::ResetFrameState() {
    {
        QMutexLocker lock(&frame_mutex_);
        pending_frame_.clear();
        frame_dirty_ = false;
    }

    QMetaObject::invokeMethod(this, QOverload<>::of(&QWidget::update), Qt::QueuedConnection);
}

void VideoRenderer::SubmitFrame(const VideoFramePacketPtr& frame) {
    if (!frame) {
        return;
    }

    bool size_changed = false;
    {
        QMutexLocker lock(&frame_mutex_);
        size_changed = (frame->width != frame_width_ || frame->height != frame_height_);
        pending_frame_ = frame;
        frame_width_ = frame->width;
        frame_height_ = frame->height;
        frame_dirty_ = true;
        last_frame_id_ = frame->frame_id;
        ++frames_submitted_;
    }

    if (size_changed) {
        emit FrameSizeChanged(frame->width, frame->height);
    }

    QMetaObject::invokeMethod(this, QOverload<>::of(&QWidget::update), Qt::QueuedConnection);
}

void VideoRenderer::initializeGL() {
    initializeOpenGLFunctions();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    shader_ = std::make_unique<QOpenGLShaderProgram>();
    shader_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    shader_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    shader_->link();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    glGenTextures(1, &tex_y_);
    glGenTextures(1, &tex_u_);
    glGenTextures(1, &tex_v_);

    auto setupTex = [this](GLuint tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    setupTex(tex_y_);
    setupTex(tex_u_);
    setupTex(tex_v_);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void VideoRenderer::resizeGL(int w, int h) {
    (void)w;
    (void)h;
}

void VideoRenderer::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    VideoFramePacketPtr frame;
    bool upload_required = false;
    {
        QMutexLocker lock(&frame_mutex_);
        if (pending_frame_) {
            displayed_frame_ = pending_frame_;
            pending_frame_.clear();
            frame_dirty_ = true;
        }

        frame = displayed_frame_;
        if (!frame || frame->width <= 0 || frame->height <= 0 || frame->y.isEmpty()) {
            return;
        }

        upload_required = frame_dirty_;
        frame_dirty_ = false;
    }

    if (upload_required) {
        if (!textures_created_ || frame->width != texture_width_ || frame->height != texture_height_) {
            CreateTextures(frame->width, frame->height);
        }
        const auto upload_now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const double present_delay_ms =
            frame->decode_ready_timestamp_us > 0 && upload_now_us > frame->decode_ready_timestamp_us
            ? static_cast<double>(upload_now_us - frame->decode_ready_timestamp_us) / 1000.0
            : 0.0;
        double frame_age_ms = present_delay_ms;
        bool frame_age_is_end_to_end = false;
        if (frame->capture_ready_timestamp_us > 0 && upload_now_us > frame->capture_ready_timestamp_us) {
            const double candidate_ms =
                static_cast<double>(upload_now_us - frame->capture_ready_timestamp_us) / 1000.0;
            if (candidate_ms >= 0.0 && candidate_ms <= 10000.0) {
                frame_age_ms = candidate_ms;
                frame_age_is_end_to_end = true;
            }
        }

        const auto upload_started = std::chrono::steady_clock::now();
        UploadTextures(*frame);
        const auto upload_finished = std::chrono::steady_clock::now();
        const double upload_ms = std::chrono::duration<double, std::milli>(
            upload_finished - upload_started).count();

        QMutexLocker lock(&frame_mutex_);
        ++frames_uploaded_;
        upload_time_total_ms_ += upload_ms;
        present_delay_total_ms_ += present_delay_ms;
        frame_age_total_ms_ += frame_age_ms;
        frame_age_window_is_end_to_end_ = frame_age_window_is_end_to_end_ || frame_age_is_end_to_end;
    }

    if (frame_width_ <= 0 || frame_height_ <= 0) {
        return;
    }

    const int display_w = DisplayWidth();
    const int display_h = DisplayHeight();
    const float video_aspect = static_cast<float>(display_w) / static_cast<float>(display_h);
    const float widget_aspect = static_cast<float>(width()) / static_cast<float>(height());

    int vp_x = 0;
    int vp_y = 0;
    int vp_w = 0;
    int vp_h = 0;
    if (widget_aspect > video_aspect) {
        vp_h = height();
        vp_w = static_cast<int>(height() * video_aspect);
        vp_x = (width() - vp_w) / 2;
        vp_y = 0;
    } else {
        vp_w = width();
        vp_h = static_cast<int>(width() / video_aspect);
        vp_x = 0;
        vp_y = (height() - vp_h) / 2;
    }
    glViewport(vp_x, vp_y, vp_w, vp_h);

    shader_->bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    shader_->setUniformValue("texY", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u_);
    shader_->setUniformValue("texU", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v_);
    shader_->setUniformValue("texV", 2);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    shader_->release();

    const auto now = std::chrono::steady_clock::now();
    if (now - stats_window_started_ >= std::chrono::seconds(5)) {
        const double elapsed_s = std::chrono::duration<double>(now - stats_window_started_).count();
        uint64_t frames_submitted = 0;
        uint64_t frames_uploaded = 0;
        uint64_t frame_id = 0;
        double upload_total_ms = 0.0;
        double present_delay_total_ms = 0.0;
        double frame_age_total_ms = 0.0;
        bool frame_age_is_end_to_end = false;
        {
            QMutexLocker lock(&frame_mutex_);
            frames_submitted = frames_submitted_;
            frames_uploaded = frames_uploaded_;
            frame_id = last_frame_id_;
            upload_total_ms = upload_time_total_ms_;
            present_delay_total_ms = present_delay_total_ms_;
            frame_age_total_ms = frame_age_total_ms_;
            frame_age_is_end_to_end = frame_age_window_is_end_to_end_;
            frames_submitted_ = 0;
            frames_uploaded_ = 0;
            upload_time_total_ms_ = 0.0;
            present_delay_total_ms_ = 0.0;
            frame_age_total_ms_ = 0.0;
            frame_age_window_is_end_to_end_ = false;
            stats_window_started_ = now;
        }

        const double submit_fps = elapsed_s > 0.0
            ? static_cast<double>(frames_submitted) / elapsed_s
            : 0.0;
        const double upload_fps = elapsed_s > 0.0
            ? static_cast<double>(frames_uploaded) / elapsed_s
            : 0.0;
        const uint64_t dropped_frames = frames_submitted >= frames_uploaded
            ? (frames_submitted - frames_uploaded)
            : 0;
        const double viewer_dropped_fps = elapsed_s > 0.0
            ? static_cast<double>(dropped_frames) / elapsed_s
            : 0.0;
        const double avg_upload_ms = frames_uploaded > 0
            ? upload_total_ms / static_cast<double>(frames_uploaded)
            : 0.0;
        const double avg_present_delay_ms = frames_uploaded > 0
            ? present_delay_total_ms / static_cast<double>(frames_uploaded)
            : 0.0;
        const double avg_frame_age_ms = frames_uploaded > 0
            ? frame_age_total_ms / static_cast<double>(frames_uploaded)
            : 0.0;

        qInfo().noquote() << QString("[viewer][renderer] submitted_fps=%1 uploaded_fps=%2 avg_upload_ms=%3 size=%4x%5")
                                 .arg(submit_fps, 0, 'f', 1)
                                 .arg(upload_fps, 0, 'f', 1)
                                 .arg(avg_upload_ms, 0, 'f', 2)
                                 .arg(frame->width)
                                 .arg(frame->height);

        RendererDiagnosticsSnapshot snapshot;
        snapshot.frame_id = frame_id;
        snapshot.submit_fps = submit_fps;
        snapshot.upload_fps = upload_fps;
        snapshot.avg_upload_ms = avg_upload_ms;
        snapshot.avg_present_delay_ms = avg_present_delay_ms;
        snapshot.avg_frame_age_ms = avg_frame_age_ms;
        snapshot.viewer_dropped_fps = viewer_dropped_fps;
        snapshot.frame_age_is_end_to_end = frame_age_is_end_to_end;
        emit DiagnosticsUpdated(snapshot);
    }
}

void VideoRenderer::UploadTextures(const VideoFramePacket& frame) {
    const int uv_w = frame.width / 2;
    const int uv_h = frame.height / 2;

    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride_y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                    GL_RED, GL_UNSIGNED_BYTE, frame.y.constData());

    glBindTexture(GL_TEXTURE_2D, tex_u_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride_u);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                    GL_RED, GL_UNSIGNED_BYTE, frame.u.constData());

    glBindTexture(GL_TEXTURE_2D, tex_v_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride_v);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                    GL_RED, GL_UNSIGNED_BYTE, frame.v.constData());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoRenderer::CreateTextures(int width, int height) {
    const int uv_w = std::max(1, width / 2);
    const int uv_h = std::max(1, height / 2);

    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, tex_u_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_w, uv_h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, tex_v_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_w, uv_h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    texture_width_ = width;
    texture_height_ = height;
    textures_created_ = true;
}

} // namespace remotex::viewer
