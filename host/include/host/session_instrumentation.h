#pragma once

#include "host/capture_frame_preparer.h"
#include "core/webrtc_peer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace remotex::host {

struct SessionInstrumentationWindow {
    double window_seconds = 0.0;
    uint64_t capture_attempt_count = 0;
    uint64_t capture_hit_count = 0;
    uint64_t captured_frames = 0;
    uint64_t encoded_frames = 0;
    uint64_t sent_frames = 0;
    uint64_t timeout_capture_count = 0;
    uint64_t idle_skipped = 0;
    uint64_t dropped_frames = 0;
    uint64_t copied_bytes = 0;
    uint64_t dirty_rects = 0;
    uint64_t move_rects = 0;
    uint64_t last_frame_id = 0;
    uint64_t desktop_switch_count = 0;
    uint64_t desktop_context_unavailable_count = 0;
    uint64_t fallback_snapshot_success_count = 0;
    uint64_t fallback_snapshot_failure_count = 0;
    int quality_level = 0;
    double acquire_wait_ms = 0.0;
    double accumulated_frames = 0.0;
    double capture_ms = 0.0;
    double encode_ms = 0.0;
    double map_ms = 0.0;
    double copy_ms = 0.0;
    double overlay_ms = 0.0;
    double send_ms = 0.0;
    double seconds_since_last_successful_capture = 0.0;
    bool input_matches_thread = false;
    bool secure_transition_failure = false;
    std::string last_capture_status;
    std::string previous_input_desktop_name;
    std::string input_desktop_name;
    std::string thread_desktop_name;
    std::string secure_transition_stage;
    std::string secure_transition_detail;
    std::string last_successful_transition_stage;
    std::string last_successful_transition_detail;
    std::string last_failed_transition_stage;
    std::string last_failed_transition_reason;
};

struct SessionInstrumentationSummary {
    double capture_fps = 0.0;
    double capture_attempt_fps = 0.0;
    double capture_hit_ratio = 0.0;
    double encoded_fps = 0.0;
    double sent_fps = 0.0;
    double avg_acquire_wait_ms = 0.0;
    uint64_t timeout_capture_count = 0;
    double avg_accumulated_frames = 0.0;
    uint64_t idle_skipped = 0;
    uint64_t dropped_frames = 0;
    double copy_mb_s = 0.0;
    double avg_capture_ms = 0.0;
    double avg_encode_ms = 0.0;
    double avg_map_ms = 0.0;
    double avg_copy_ms = 0.0;
    double avg_overlay_ms = 0.0;
    double avg_send_ms = 0.0;
    double avg_dirty_rects = 0.0;
    double avg_move_rects = 0.0;
    uint64_t last_frame_id = 0;
    uint64_t desktop_switch_count = 0;
    uint64_t desktop_context_unavailable_count = 0;
    uint64_t fallback_snapshot_success_count = 0;
    uint64_t fallback_snapshot_failure_count = 0;
    int quality_level = 0;
    double seconds_since_last_successful_capture = 0.0;
    bool input_matches_thread = false;
    bool secure_transition_failure = false;
    std::string last_capture_status;
    std::string previous_input_desktop_name;
    std::string input_desktop_name;
    std::string thread_desktop_name;
    std::string secure_transition_stage;
    std::string secure_transition_detail;
    std::string last_successful_transition_stage;
    std::string last_successful_transition_detail;
    std::string last_failed_transition_stage;
    std::string last_failed_transition_reason;
};

inline SessionInstrumentationSummary SummarizeInstrumentationWindow(
    const SessionInstrumentationWindow& window) {
    const double seconds = (std::max)(window.window_seconds, 0.001);
    const auto raw_attempts = window.capture_attempt_count != 0
                                  ? window.capture_attempt_count
                                  : window.captured_frames + window.timeout_capture_count;
    const auto raw_hits =
        window.capture_hit_count != 0 ? window.capture_hit_count : window.captured_frames;
    const auto per_capture = static_cast<double>((std::max<uint64_t>)(window.captured_frames, 1));
    const auto per_attempt =
        static_cast<double>((std::max<uint64_t>)(raw_attempts, 1));
    const auto per_encoded = static_cast<double>((std::max<uint64_t>)(window.encoded_frames, 1));
    const auto per_sent = static_cast<double>((std::max<uint64_t>)(window.sent_frames, 1));

    SessionInstrumentationSummary summary;
    summary.capture_fps = static_cast<double>(window.captured_frames) / seconds;
    summary.capture_attempt_fps = static_cast<double>(raw_attempts) / seconds;
    summary.capture_hit_ratio = static_cast<double>(raw_hits) / per_attempt;
    summary.encoded_fps = static_cast<double>(window.encoded_frames) / seconds;
    summary.sent_fps = static_cast<double>(window.sent_frames) / seconds;
    summary.avg_acquire_wait_ms = window.acquire_wait_ms / per_attempt;
    summary.timeout_capture_count = window.timeout_capture_count;
    summary.avg_accumulated_frames = window.accumulated_frames / per_capture;
    summary.idle_skipped = window.idle_skipped;
    summary.dropped_frames = window.dropped_frames;
    summary.copy_mb_s =
        static_cast<double>(window.copied_bytes) / seconds / (1024.0 * 1024.0);
    summary.avg_capture_ms = window.capture_ms / per_capture;
    summary.avg_encode_ms = window.encode_ms / per_encoded;
    summary.avg_map_ms = window.map_ms / per_capture;
    summary.avg_copy_ms = window.copy_ms / per_capture;
    summary.avg_overlay_ms = window.overlay_ms / per_capture;
    summary.avg_send_ms = window.send_ms / per_sent;
    summary.avg_dirty_rects =
        static_cast<double>(window.dirty_rects) / per_capture;
    summary.avg_move_rects =
        static_cast<double>(window.move_rects) / per_capture;
    summary.last_frame_id = window.last_frame_id;
    summary.desktop_switch_count = window.desktop_switch_count;
    summary.desktop_context_unavailable_count = window.desktop_context_unavailable_count;
    summary.fallback_snapshot_success_count = window.fallback_snapshot_success_count;
    summary.fallback_snapshot_failure_count = window.fallback_snapshot_failure_count;
    summary.quality_level = window.quality_level;
    summary.seconds_since_last_successful_capture = window.seconds_since_last_successful_capture;
    summary.input_matches_thread = window.input_matches_thread;
    summary.secure_transition_failure = window.secure_transition_failure;
    summary.last_capture_status = window.last_capture_status;
    summary.previous_input_desktop_name = window.previous_input_desktop_name;
    summary.input_desktop_name = window.input_desktop_name;
    summary.thread_desktop_name = window.thread_desktop_name;
    summary.secure_transition_stage = window.secure_transition_stage;
    summary.secure_transition_detail = window.secure_transition_detail;
    summary.last_successful_transition_stage = window.last_successful_transition_stage;
    summary.last_successful_transition_detail = window.last_successful_transition_detail;
    summary.last_failed_transition_stage = window.last_failed_transition_stage;
    summary.last_failed_transition_reason = window.last_failed_transition_reason;
    return summary;
}

inline void AppendJsonQuoted(std::string& json,
                             const char* key,
                             const std::string& value) {
    json += ",\"";
    json += key;
    json += "\":\"";
    for (const char ch : value) {
        switch (ch) {
            case '\\': json += "\\\\"; break;
            case '"': json += "\\\""; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default: json.push_back(ch); break;
        }
    }
    json += '"';
}

inline void AppendJsonBool(std::string& json,
                           const char* key,
                           bool value) {
    json += ",\"";
    json += key;
    json += "\":";
    json += value ? "true" : "false";
}

inline std::string BuildSessionDiagnosticsJson(
    const SessionInstrumentationSummary& summary) {
    char diagnostics_json[768];
    std::snprintf(
        diagnostics_json,
        sizeof(diagnostics_json),
        "{\"type\":\"session_diagnostics\",\"capture_fps\":%.1f,\"capture_attempt_fps\":%.1f,\"capture_hit_ratio\":%.3f,\"encoded_fps\":%.1f,\"sent_fps\":%.1f,\"avg_acquire_wait_ms\":%.2f,\"timeout_capture_count\":%llu,\"avg_accumulated_frames\":%.2f,\"dropped_frames\":%llu,\"quality_level\":%d,\"desktop_switch_count\":%llu,\"desktop_context_unavailable_count\":%llu,\"seconds_since_last_successful_capture\":%.2f,\"fallback_snapshot_success_count\":%llu,\"fallback_snapshot_failure_count\":%llu,\"input_matches_thread\":%s",
        summary.capture_fps,
        summary.capture_attempt_fps,
        summary.capture_hit_ratio,
        summary.encoded_fps,
        summary.sent_fps,
        summary.avg_acquire_wait_ms,
        static_cast<unsigned long long>(summary.timeout_capture_count),
        summary.avg_accumulated_frames,
        static_cast<unsigned long long>(summary.dropped_frames),
        summary.quality_level,
        static_cast<unsigned long long>(summary.desktop_switch_count),
        static_cast<unsigned long long>(summary.desktop_context_unavailable_count),
        summary.seconds_since_last_successful_capture,
        static_cast<unsigned long long>(summary.fallback_snapshot_success_count),
        static_cast<unsigned long long>(summary.fallback_snapshot_failure_count),
        summary.input_matches_thread ? "true" : "false");

    std::string json = diagnostics_json;
    AppendJsonQuoted(json, "last_capture_status", summary.last_capture_status);
    AppendJsonQuoted(json, "previous_input_desktop_name", summary.previous_input_desktop_name);
    AppendJsonQuoted(json, "input_desktop_name", summary.input_desktop_name);
    AppendJsonQuoted(json, "thread_desktop_name", summary.thread_desktop_name);
    AppendJsonQuoted(json, "secure_transition_stage", summary.secure_transition_stage);
    AppendJsonQuoted(json, "secure_transition_detail", summary.secure_transition_detail);
    AppendJsonBool(json, "secure_transition_failure", summary.secure_transition_failure);
    AppendJsonQuoted(json,
                     "last_successful_transition_stage",
                     summary.last_successful_transition_stage);
    AppendJsonQuoted(json,
                     "last_successful_transition_detail",
                     summary.last_successful_transition_detail);
    AppendJsonQuoted(json,
                     "last_failed_transition_stage",
                     summary.last_failed_transition_stage);
    AppendJsonQuoted(json,
                     "last_failed_transition_reason",
                     summary.last_failed_transition_reason);
    json += '}';
    return json;
}

inline remotex::core::VideoFrameMetrics MakeVideoFrameMetrics(
    uint64_t frame_id,
    int64_t capture_ready_timestamp_us,
    int64_t encode_ready_timestamp_us = 0,
    int64_t send_started_timestamp_us = 0) {
    remotex::core::VideoFrameMetrics metrics;
    metrics.frame_id = frame_id;
    metrics.capture_ready_timestamp_us = capture_ready_timestamp_us;
    metrics.encode_ready_timestamp_us = encode_ready_timestamp_us;
    metrics.send_started_timestamp_us = send_started_timestamp_us;
    return metrics;
}

} // namespace remotex::host
