// RemoteX — tests/host/test_clipboard_sync.cpp

#include <gtest/gtest.h>
#include "host/clipboard_sync.h"
#include <thread>
#include <chrono>
#include <filesystem>

using namespace remotex::host;

TEST(ClipboardSyncTest, BasicLifecycle) {
    ClipboardSync sync;
    // Should start and stop without crashing
    sync.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sync.Stop();
    SUCCEED();
}

TEST(ClipboardSyncTest, SetRemoteClipboardAndReceiveCallback) {
    ClipboardSync sync;

    ClipboardSync::ClipboardPayload received_payload;
    bool callback_invoked = false;

    sync.SetOnClipboardChanged([&](const ClipboardSync::ClipboardPayload& payload) {
        received_payload = payload;
        callback_invoked = true;
    });

    sync.Start();

    // Give the message loop time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Inject text into the clipboard
    std::string test_string = "Hello RemoteX Testing 123";
    sync.SetRemoteClipboard(test_string);

    // Wait for the clipboard event to be processed by the message loop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sync.Stop();

    // Since SetRemoteClipboard sets `is_updating_clipboard_` to true, the callback
    // SHOULD NOT be invoked when we are the ones updating the clipboard.
    EXPECT_FALSE(callback_invoked);
    EXPECT_TRUE(received_payload.text.empty());
    EXPECT_TRUE(received_payload.file_paths.empty());
}

TEST(ClipboardSyncTest, SetRemoteClipboardFilesAndReceiveCallback) {
    ClipboardSync sync;

    ClipboardSync::ClipboardPayload received_payload;
    bool callback_invoked = false;

    sync.SetOnClipboardChanged([&](const ClipboardSync::ClipboardPayload& payload) {
        received_payload = payload;
        callback_invoked = true;
    });

    sync.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::filesystem::path> test_paths = {
        std::filesystem::temp_directory_path() / "remotex-clipboard-sync-test.txt"
    };
    sync.SetRemoteClipboardFiles(test_paths);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sync.Stop();

    EXPECT_FALSE(callback_invoked);
    EXPECT_TRUE(received_payload.text.empty());
    EXPECT_TRUE(received_payload.file_paths.empty());
}
