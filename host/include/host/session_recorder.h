#pragma once
// RemoteX — host/session_recorder.h
// Grava frames BGRA da sessão remota em arquivo MP4 (H.264) usando FFmpeg.
// Quando REMOTEX_HAS_FFMPEG não está definido, todos os métodos são no-op.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace remotex::host {

struct RecorderConfig {
    std::filesystem::path output_dir;
    int                   width           = 0;
    int                   height          = 0;
    int                   fps             = 15;
    int                   bitrate         = 2000000;
    bool                  use_hw_encoder  = true;
};

class SessionRecorder {
public:
    SessionRecorder();
    ~SessionRecorder();

    SessionRecorder(const SessionRecorder&)            = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    // Inicia a gravação. Retorna true se inicializado com sucesso.
    bool Start(const RecorderConfig& config);

    // Enfileira um frame BGRA para gravação. Thread-safe.
    // Decimação: ignora frames se não passou tempo suficiente para manter o fps alvo.
    void PushFrame(const uint8_t* bgra, size_t size,
                   int width, int height, int pitch);

    // Para a gravação, drena a fila, fecha o arquivo.
    // Retorna o caminho do MP4 gerado, ou vazio em caso de erro.
    std::filesystem::path Stop();

    bool IsRecording() const { return recording_.load(); }

private:
#ifdef REMOTEX_HAS_FFMPEG
    struct Frame {
        std::vector<uint8_t> data;
        int                  width;
        int                  height;
        int                  pitch;
    };

    void EncoderThread();

    struct Impl;
    std::unique_ptr<Impl>       impl_;

    std::thread                 encoder_thread_;
    std::mutex                  queue_mutex_;
    std::condition_variable     queue_cv_;
    std::queue<Frame>           frame_queue_;
    static constexpr size_t     kMaxQueueSize = 10;

    std::atomic<bool>           recording_{false};
    std::atomic<bool>           stop_requested_{false};
    std::filesystem::path       output_path_;

    RecorderConfig              config_;

    std::chrono::steady_clock::time_point last_frame_time_{};
    int64_t                     frame_count_ = 0;
#else
    std::atomic<bool>           recording_{false};
#endif
};

} // namespace remotex::host
