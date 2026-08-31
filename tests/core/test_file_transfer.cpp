// RemoteX - tests/core/test_file_transfer.cpp

#include <gtest/gtest.h>

#include "core/file_transfer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace remotex::core;
namespace fs = std::filesystem;

namespace {

class RecordingListener final : public FileTransferListener {
public:
    void OnRemoteListing(const RemoteFsListing& listing) override {
        listings.push_back(listing);
    }

    void OnTransferJobStarted(const TransferJobInfo& info) override {
        started.push_back(info);
    }

    void OnTransferJobProgress(const TransferJobInfo& job,
                               const TransferEntryInfo& entry) override {
        last_job = job;
        progress.push_back(entry);
    }

    void OnTransferJobCompleted(const TransferJobInfo& info,
                                const fs::path& destination_root) override {
        completed.push_back(info);
        last_destination = destination_root;
    }

    void OnTransferJobFailed(const TransferJobInfo& info,
                             const std::string& error) override {
        failed.push_back(info);
        failed_errors.push_back(error);
        last_error = error;
    }

    void OnTransferJobCancelled(const TransferJobInfo& info) override {
        cancelled.push_back(info);
    }

    void OnTransferConflictPreviewReady(const TransferConflictPreview& preview) override {
        previews_ready.push_back(preview);
    }

    void OnTransferConflictPreviewFailed(const TransferJobInfo& info,
                                         const std::string& error) override {
        previews_failed.push_back(info);
        preview_failed_errors.push_back(error);
    }

    std::vector<RemoteFsListing> listings;
    std::vector<TransferJobInfo> started;
    std::vector<TransferEntryInfo> progress;
    std::vector<TransferJobInfo> completed;
    std::vector<TransferJobInfo> failed;
    std::vector<std::string> failed_errors;
    std::vector<TransferJobInfo> cancelled;
    std::vector<TransferConflictPreview> previews_ready;
    std::vector<TransferJobInfo> previews_failed;
    std::vector<std::string> preview_failed_errors;
    TransferJobInfo last_job;
    fs::path last_destination;
    std::string last_error;
};

std::vector<uint8_t> ToTextMessage(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> MakeChunk(const std::string& entry_id_hex, uint32_t chunk_index,
                               const std::string& payload) {
    std::vector<uint8_t> chunk(kChunkHeaderSize + payload.size(), 0);
    for (size_t i = 0; i < kFileIdSize && (i * 2 + 1) < entry_id_hex.size(); ++i) {
        auto nibble = [](char ch) -> uint8_t {
            if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
            if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
            if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(ch - 'A' + 10);
            return 0;
        };
        chunk[i] = static_cast<uint8_t>((nibble(entry_id_hex[i * 2]) << 4) | nibble(entry_id_hex[i * 2 + 1]));
    }
    chunk[kFileIdSize + 0] = static_cast<uint8_t>(chunk_index & 0xFF);
    chunk[kFileIdSize + 1] = static_cast<uint8_t>((chunk_index >> 8) & 0xFF);
    chunk[kFileIdSize + 2] = static_cast<uint8_t>((chunk_index >> 16) & 0xFF);
    chunk[kFileIdSize + 3] = static_cast<uint8_t>((chunk_index >> 24) & 0xFF);
    std::memcpy(chunk.data() + kChunkHeaderSize, payload.data(), payload.size());
    return chunk;
}

bool WaitForCondition(const std::function<bool()>& condition,
                      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

} // namespace

TEST(FileTransfer, ConstantsRemainStable) {
    EXPECT_EQ(kFileIdSize, 16u);
    EXPECT_EQ(kChunkHeaderSize, 20u);
    EXPECT_EQ(kFileChunkSize, 64u * 1024u);
}

TEST(FileTransfer, TransferJobInfoDefaults) {
    TransferJobInfo info;
    EXPECT_EQ(info.total_bytes, 0u);
    EXPECT_EQ(info.bytes_done, 0u);
    EXPECT_EQ(info.total_entries, 0u);
    EXPECT_EQ(info.entries_done, 0u);
    EXPECT_EQ(info.direction, TransferDirection::Send);
    EXPECT_EQ(info.state, TransferState::Pending);
    EXPECT_TRUE(info.error.empty());
    EXPECT_FALSE(info.overwrite_allowed);
}

TEST(FileTransfer, RemoteListingResponseNotifiesListener) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, fs::temp_directory_path());
    transfer.SetListener(&listener);

    const std::string listing =
        R"({"type":"fs_list_response","request_id":7,"path":"C:\\Users","is_root_listing":false,"success":true,"error":"","entries":[{"name":"Docs","absolute_path":"C:\\Users\\Docs","is_directory":true,"is_access_denied":false,"size_bytes":0,"modified_unix_sec":1}]})";

    transfer.OnDataReceived(ToTextMessage(listing), false);

    ASSERT_EQ(listener.listings.size(), 1u);
    EXPECT_EQ(listener.listings[0].request_id, 7u);
    EXPECT_EQ(listener.listings[0].path, "C:\\Users");
    ASSERT_EQ(listener.listings[0].entries.size(), 1u);
    EXPECT_EQ(listener.listings[0].entries[0].name, "Docs");
    EXPECT_TRUE(listener.listings[0].entries[0].is_directory);
}

TEST(FileTransfer, PreviewRemotePathsTimeoutNotifiesFailure) {
    FileTransfer transfer;
    RecordingListener listener;
    const fs::path root = fs::temp_directory_path() / "remotex_preview_timeout";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);
    transfer.SetPreviewResponseTimeout(std::chrono::milliseconds(50));

    const auto job_id = transfer.PreviewRemotePaths({fs::path("C:/Remote/missing.txt")}, root / "download");
    ASSERT_FALSE(job_id.empty());

    ASSERT_TRUE(WaitForCondition([&]() { return !listener.previews_failed.empty(); }));
    EXPECT_EQ(listener.previews_failed.back().id_hex, job_id);
    ASSERT_FALSE(listener.preview_failed_errors.empty());
    EXPECT_NE(listener.preview_failed_errors.back().find("preview-timeout"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(FileTransfer, PreviewRemotePathsUnsupportedResponseNotifiesFailure) {
    FileTransfer transfer;
    RecordingListener listener;
    const fs::path root = fs::temp_directory_path() / "remotex_preview_unsupported";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);
    transfer.SetPreviewResponseTimeout(std::chrono::seconds(5));

    const auto job_id = transfer.PreviewRemotePaths({fs::path("C:/Remote/file.txt")}, root / "download");
    ASSERT_FALSE(job_id.empty());

    transfer.OnDataReceived(ToTextMessage(
        std::string("{\"type\":\"transfer_preview_response\",\"job_id\":\"") + job_id +
        "\",\"preview_kind\":\"download\",\"success\":false,\"error\":\"preview-unsupported: peer rejected conflict preview\"}"), false);

    ASSERT_EQ(listener.previews_failed.size(), 1u);
    EXPECT_EQ(listener.previews_failed.back().id_hex, job_id);
    ASSERT_FALSE(listener.preview_failed_errors.empty());
    EXPECT_NE(listener.preview_failed_errors.back().find("preview-unsupported"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(FileTransfer, PreviewSendLocalPathsConflictDeclineDoesNotStartTransfer) {
    const fs::path root = fs::temp_directory_path() / "remotex_preview_decline";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const fs::path source = root / "source.txt";
    {
        std::ofstream out(source, std::ios::binary);
        out << "hello";
    }

    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);
    transfer.SetPreviewResponseTimeout(std::chrono::seconds(5));

    const auto job_id = transfer.PreviewSendLocalPaths({source}, fs::path("C:/RemoteDest"));
    ASSERT_FALSE(job_id.empty());

    transfer.OnDataReceived(ToTextMessage(
        std::string("{\"type\":\"transfer_preview_response\",\"job_id\":\"") + job_id +
        "\",\"preview_kind\":\"upload\",\"success\":true,\"conflicts\":[{\"relative_path\":\"source.txt\",\"incoming_is_directory\":false,\"existing_is_directory\":false,\"type_mismatch\":false}]}"), false);

    ASSERT_EQ(listener.previews_ready.size(), 1u);
    ASSERT_EQ(listener.previews_ready[0].conflicts.size(), 1u);
    EXPECT_EQ(listener.previews_ready[0].conflicts[0].relative_path, "source.txt");

    EXPECT_TRUE(transfer.ResolveTransferPreview(job_id, false));
    ASSERT_EQ(listener.previews_failed.size(), 1u);
    ASSERT_FALSE(listener.preview_failed_errors.empty());
    EXPECT_NE(listener.preview_failed_errors.back().find("conflict-declined"), std::string::npos);
    EXPECT_TRUE(listener.started.empty());

    fs::remove_all(root, ec);
}

TEST(FileTransfer, InvalidControlJsonFailsClosedWithoutNotifications) {
    FileTransfer transfer;
    RecordingListener listener;
    const fs::path root = fs::temp_directory_path() / "remotex_invalid_json";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);

    transfer.OnDataReceived(ToTextMessage("{not-json"), false);

    EXPECT_TRUE(listener.listings.empty());
    EXPECT_TRUE(listener.started.empty());
    EXPECT_TRUE(listener.progress.empty());
    EXPECT_TRUE(listener.completed.empty());
    EXPECT_TRUE(listener.failed.empty());
    EXPECT_TRUE(listener.cancelled.empty());
    EXPECT_TRUE(listener.previews_ready.empty());
    EXPECT_TRUE(listener.previews_failed.empty());
    EXPECT_TRUE(transfer.GetActiveJobs().empty());

    fs::remove_all(root, ec);
}

TEST(FileTransfer, PreviewResponseMissingJobIdFailsClosedAndOriginalPreviewTimesOut) {
    FileTransfer transfer;
    RecordingListener listener;
    const fs::path root = fs::temp_directory_path() / "remotex_preview_missing_job_id";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);
    transfer.SetPreviewResponseTimeout(std::chrono::milliseconds(50));

    const auto job_id = transfer.PreviewRemotePaths({fs::path("C:/Remote/file.txt")}, root / "download");
    ASSERT_FALSE(job_id.empty());

    transfer.OnDataReceived(ToTextMessage(
        R"({"type":"transfer_preview_response","preview_kind":"download","success":false,"error":"preview-unsupported: peer rejected conflict preview"})"), false);

    EXPECT_TRUE(listener.previews_failed.empty());
    ASSERT_TRUE(WaitForCondition([&]() { return !listener.previews_failed.empty(); }));
    EXPECT_EQ(listener.previews_failed.back().id_hex, job_id);
    ASSERT_FALSE(listener.preview_failed_errors.empty());
    EXPECT_NE(listener.preview_failed_errors.back().find("preview-timeout"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(FileTransfer, PendingPreviewQueriesRemainStableUntilCancelled) {
    FileTransfer transfer;
    RecordingListener listener;
    const fs::path root = fs::temp_directory_path() / "remotex_preview_query_cancel";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);
    transfer.SetPreviewResponseTimeout(std::chrono::seconds(5));

    const auto job_id = transfer.PreviewRemotePaths({fs::path("C:/Remote/file.txt")}, root / "download");
    ASSERT_FALSE(job_id.empty());

    const auto info = transfer.GetJobInfo(job_id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->id_hex, job_id);
    EXPECT_EQ(info->state, TransferState::Pending);
    EXPECT_EQ(info->direction, TransferDirection::Receive);

    const auto active_first = transfer.GetActiveJobs();
    const auto active_second = transfer.GetActiveJobs();
    ASSERT_EQ(active_first.size(), 1u);
    ASSERT_EQ(active_second.size(), 1u);
    EXPECT_EQ(active_first.front().id_hex, job_id);
    EXPECT_EQ(active_second.front().id_hex, job_id);

    transfer.Cancel(job_id, "user cancelled");

    ASSERT_EQ(listener.cancelled.size(), 1u);
    EXPECT_EQ(listener.cancelled.back().id_hex, job_id);
    EXPECT_EQ(listener.cancelled.back().state, TransferState::Cancelled);
    EXPECT_EQ(listener.cancelled.back().error, "user cancelled");

    const auto cancelled = transfer.GetJobInfo(job_id);
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_TRUE(transfer.GetActiveJobs().empty());

    fs::remove_all(root, ec);
}

TEST(FileTransfer, ReceiveEntryWithUnsafeRelativePathFailsClosed) {
    const fs::path root = fs::temp_directory_path() / "remotex_unsafe_receive";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);

    const fs::path destination = root / "dest";
    const std::string job_id = "abababababababababababababababab";

    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":5,\"total_entries\":1,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd\",\"name\":\"escape.txt\",\"relative_path\":\"../escape.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);

    ASSERT_EQ(listener.failed.size(), 1u);
    EXPECT_NE(listener.last_error.find("unsafe relative path"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "escape.txt"));

    fs::remove_all(root, ec);
}

TEST(FileTransfer, ReceiveJobCreatesNestedFileAndCompletes) {
    const fs::path root = fs::temp_directory_path() / "remotex_test_receive";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, root);
    transfer.SetListener(&listener);

    const fs::path destination = root / "dest";
    const std::string job_id = "11111111111111111111111111111111";
    const std::string entry_id = "22222222222222222222222222222222";

    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":5,\"total_entries\":2,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"33333333333333333333333333333333\",\"name\":\"folder\",\"relative_path\":\"folder\",\"is_directory\":true,\"size\":0,\"chunks\":0}"), false);

    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"file.txt\",\"relative_path\":\"folder/file.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);

    transfer.OnDataReceived(MakeChunk(entry_id, 0, "hello"), true);
    transfer.OnDataReceived(ToTextMessage(
        "{\"type\":\"transfer_job_complete\",\"job_id\":\"" + job_id + "\"}"), false);

    const fs::path expected = destination / "folder" / "file.txt";
    ASSERT_TRUE(fs::exists(expected));

    std::ifstream in(expected, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello");
    ASSERT_EQ(listener.completed.size(), 1u);
    EXPECT_EQ(listener.last_destination, destination);

    fs::remove_all(root, ec);
}
