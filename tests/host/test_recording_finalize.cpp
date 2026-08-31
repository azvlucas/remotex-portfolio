#include <gtest/gtest.h>

#include "host/recording_finalize.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace remotex::host;

TEST(RecordingFinalize, DoesNothingWhenRecorderIsIdle) {
    bool stop_called = false;
    bool upload_called = false;

    const auto result = FinalizeRecordingIfPresent(
        false,
        std::chrono::steady_clock::time_point{},
        std::chrono::steady_clock::time_point{},
        "session-1",
        [&]() {
            stop_called = true;
            return fs::path("ignored.mp4");
        },
        [&](const fs::path&, const std::string&, int) {
            upload_called = true;
        });

    EXPECT_FALSE(stop_called);
    EXPECT_FALSE(upload_called);
    EXPECT_TRUE(result.path.empty());
    EXPECT_EQ(result.duration_sec, 0);
    EXPECT_FALSE(result.upload_dispatched);
}

TEST(RecordingFinalize, DoesNotUploadWhenRecorderStopsWithoutMp4) {
    bool stop_called = false;
    bool upload_called = false;

    const auto result = FinalizeRecordingIfPresent(
        true,
        std::chrono::steady_clock::now() - std::chrono::seconds(5),
        std::chrono::steady_clock::now(),
        "session-1",
        [&]() {
            stop_called = true;
            return fs::path{};
        },
        [&](const fs::path&, const std::string&, int) {
            upload_called = true;
        });

    EXPECT_TRUE(stop_called);
    EXPECT_FALSE(upload_called);
    EXPECT_TRUE(result.path.empty());
    EXPECT_EQ(result.duration_sec, 0);
    EXPECT_FALSE(result.upload_dispatched);
}

TEST(RecordingFinalize, UploadsSavedRecordingFromAnyTeardownPath) {
    bool stop_called = false;
    bool upload_called = false;
    fs::path uploaded_path;
    std::string uploaded_session_id;
    int uploaded_duration_sec = -1;

    const auto started_at = std::chrono::steady_clock::time_point(std::chrono::seconds(10));
    const auto finished_at = std::chrono::steady_clock::time_point(std::chrono::seconds(17));

    const auto result = FinalizeRecordingIfPresent(
        true,
        started_at,
        finished_at,
        "session-42",
        [&]() {
            stop_called = true;
            return fs::path("C:/temp/session.mp4");
        },
        [&](const fs::path& path, const std::string& session_id, int duration_sec) {
            upload_called = true;
            uploaded_path = path;
            uploaded_session_id = session_id;
            uploaded_duration_sec = duration_sec;
        });

    EXPECT_TRUE(stop_called);
    EXPECT_TRUE(upload_called);
    EXPECT_EQ(result.path, fs::path("C:/temp/session.mp4"));
    EXPECT_EQ(result.duration_sec, 7);
    EXPECT_TRUE(result.upload_dispatched);
    EXPECT_EQ(uploaded_path, result.path);
    EXPECT_EQ(uploaded_session_id, "session-42");
    EXPECT_EQ(uploaded_duration_sec, 7);
}
