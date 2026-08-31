// RemoteX - tests/integration/test_file_transfer.cpp

#include <gtest/gtest.h>

#include "core/file_transfer.h"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace remotex::core;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> Msg(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> Chunk(const std::string& entry_id_hex, uint32_t chunk_index,
                           const std::string& payload) {
    std::vector<uint8_t> chunk(kChunkHeaderSize + payload.size(), 0);
    auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(ch - 'A' + 10);
        return 0;
    };
    for (size_t i = 0; i < kFileIdSize && (i * 2 + 1) < entry_id_hex.size(); ++i) {
        chunk[i] = static_cast<uint8_t>((nibble(entry_id_hex[i * 2]) << 4) | nibble(entry_id_hex[i * 2 + 1]));
    }
    chunk[kFileIdSize + 0] = static_cast<uint8_t>(chunk_index & 0xFF);
    chunk[kFileIdSize + 1] = static_cast<uint8_t>((chunk_index >> 8) & 0xFF);
    chunk[kFileIdSize + 2] = static_cast<uint8_t>((chunk_index >> 16) & 0xFF);
    chunk[kFileIdSize + 3] = static_cast<uint8_t>((chunk_index >> 24) & 0xFF);
    std::memcpy(chunk.data() + kChunkHeaderSize, payload.data(), payload.size());
    return chunk;
}

class RecordingListener final : public FileTransferListener {
public:
    void OnRemoteListing(const RemoteFsListing&) override {}
    void OnTransferJobStarted(const TransferJobInfo& info) override { started.push_back(info); }
    void OnTransferJobProgress(const TransferJobInfo&, const TransferEntryInfo&) override {}
    void OnTransferJobCompleted(const TransferJobInfo& info,
                                const fs::path& destination_root) override {
        completed.push_back(info);
        last_destination = destination_root;
    }
    void OnTransferJobFailed(const TransferJobInfo& info,
                             const std::string& error) override {
        failed.push_back(info);
        last_error = error;
    }
    void OnTransferJobCancelled(const TransferJobInfo&) override {}

    std::vector<TransferJobInfo> started;
    std::vector<TransferJobInfo> completed;
    std::vector<TransferJobInfo> failed;
    fs::path last_destination;
    std::string last_error;
};

} // namespace

class FileTransferIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = fs::temp_directory_path() / "remotex_ft_job_test";
        fs::remove_all(base_dir_);
        fs::create_directories(base_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_dir_, ec);
    }

    fs::path base_dir_;
};

TEST_F(FileTransferIntegrationTest, ReceiveZeroByteFileCompletesJob) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "download";
    const std::string job_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string entry_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":0,\"total_entries\":1,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"empty.txt\",\"relative_path\":\"empty.txt\",\"is_directory\":false,\"size\":0,\"chunks\":0}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_complete\",\"job_id\":\"" + job_id + "\"}"), false);

    EXPECT_TRUE(fs::exists(destination / "empty.txt"));
    EXPECT_EQ(fs::file_size(destination / "empty.txt"), 0u);
    ASSERT_EQ(listener.completed.size(), 1u);
    EXPECT_EQ(listener.last_destination, destination);
}

TEST_F(FileTransferIntegrationTest, ReceiveZeroByteFileWithoutApprovalPreservesExistingDestination) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "download";
    fs::create_directories(destination);
    {
        std::ofstream out(destination / "empty.txt", std::ios::binary);
        out << "keep";
    }

    const std::string job_id = "11111111111111111111111111111111";
    const std::string entry_id = "22222222222222222222222222222222";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":0,\"total_entries\":1,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"empty.txt\",\"relative_path\":\"empty.txt\",\"is_directory\":false,\"size\":0,\"chunks\":0}"), false);

    ASSERT_EQ(listener.failed.size(), 1u);
    EXPECT_NE(listener.last_error.find("conflict-declined"), std::string::npos);

    std::ifstream in(destination / "empty.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "keep");
}

TEST_F(FileTransferIntegrationTest, ReceiveZeroByteFileWithApprovalReplacesExistingDestination) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "download";
    fs::create_directories(destination);
    {
        std::ofstream out(destination / "empty.txt", std::ios::binary);
        out << "keep";
    }

    const std::string job_id = "33333333333333333333333333333333";
    const std::string entry_id = "44444444444444444444444444444444";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":0,\"total_entries\":1,\"overwrite_allowed\":true}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"empty.txt\",\"relative_path\":\"empty.txt\",\"is_directory\":false,\"size\":0,\"chunks\":0}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_complete\",\"job_id\":\"" + job_id + "\"}"), false);

    EXPECT_TRUE(fs::exists(destination / "empty.txt"));
    EXPECT_EQ(fs::file_size(destination / "empty.txt"), 0u);
    EXPECT_TRUE(listener.failed.empty());
}

TEST_F(FileTransferIntegrationTest, ReceiveFileDirectoryTypeMismatchRequiresApproval) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "receive";
    fs::create_directories(destination / "collision.txt");

    const std::string job_id = "55555555555555555555555555555555";
    const std::string entry_id = "66666666666666666666666666666666";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":5,\"total_entries\":1,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"collision.txt\",\"relative_path\":\"collision.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);

    ASSERT_EQ(listener.failed.size(), 1u);
    EXPECT_NE(listener.last_error.find("conflict-declined"), std::string::npos);
    EXPECT_TRUE(fs::is_directory(destination / "collision.txt"));
}

TEST_F(FileTransferIntegrationTest, ReceiveFileDirectoryTypeMismatchWithApprovalReplacesDirectory) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "receive";
    fs::create_directories(destination / "collision.txt");
    {
        std::ofstream nested(destination / "collision.txt" / "old.txt", std::ios::binary);
        nested << "old";
    }

    const std::string job_id = "77777777777777777777777777777777";
    const std::string entry_id = "88888888888888888888888888888888";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":5,\"total_entries\":1,\"overwrite_allowed\":true}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"collision.txt\",\"relative_path\":\"collision.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);
    transfer.OnDataReceived(Chunk(entry_id, 0, "hello"), true);
    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_complete\",\"job_id\":\"" + job_id + "\"}"), false);

    ASSERT_TRUE(fs::exists(destination / "collision.txt"));
    EXPECT_TRUE(fs::is_regular_file(destination / "collision.txt"));
    std::ifstream in(destination / "collision.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello");
    EXPECT_TRUE(listener.failed.empty());
}

TEST_F(FileTransferIntegrationTest, ReceiveLateConflictWithoutApprovalPreservesExistingFile) {
    FileTransfer transfer;
    RecordingListener listener;
    transfer.Initialize(nullptr, base_dir_);
    transfer.SetListener(&listener);

    const fs::path destination = base_dir_ / "receive";
    fs::create_directories(destination);

    const std::string job_id = "99999999999999999999999999999999";
    const std::string entry_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const fs::path final_path = destination / "late.txt";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":5,\"total_entries\":1,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"" + entry_id +
        "\",\"name\":\"late.txt\",\"relative_path\":\"late.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);

    {
        std::ofstream out(final_path, std::ios::binary);
        out << "keep";
    }

    transfer.OnDataReceived(Chunk(entry_id, 0, "hello"), true);

    ASSERT_EQ(listener.failed.size(), 1u);
    EXPECT_NE(listener.last_error.find("late-conflict"), std::string::npos);

    std::ifstream in(final_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "keep");
}

TEST_F(FileTransferIntegrationTest, ReceiveMultipleEntriesPreservesHierarchy) {
    FileTransfer transfer;
    transfer.Initialize(nullptr, base_dir_);

    const fs::path destination = base_dir_ / "receive";
    const std::string job_id = "cccccccccccccccccccccccccccccccc";

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_start\",\"job_id\":\"" + job_id +
        "\",\"destination_root\":\"" + destination.generic_string() +
        "\",\"total_bytes\":11,\"total_entries\":3,\"overwrite_allowed\":false}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"dddddddddddddddddddddddddddddddd\",\"name\":\"docs\",\"relative_path\":\"docs\",\"is_directory\":true,\"size\":0,\"chunks\":0}"), false);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\",\"name\":\"a.txt\",\"relative_path\":\"docs/a.txt\",\"is_directory\":false,\"size\":5,\"chunks\":1}"), false);
    transfer.OnDataReceived(Chunk("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 0, "hello"), true);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_entry_start\",\"job_id\":\"" + job_id +
        "\",\"entry_id\":\"ffffffffffffffffffffffffffffffff\",\"name\":\"b.txt\",\"relative_path\":\"docs/sub/b.txt\",\"is_directory\":false,\"size\":6,\"chunks\":1}"), false);
    transfer.OnDataReceived(Chunk("ffffffffffffffffffffffffffffffff", 0, "world!"), true);

    transfer.OnDataReceived(Msg(
        "{\"type\":\"transfer_job_complete\",\"job_id\":\"" + job_id + "\"}"), false);

    EXPECT_TRUE(fs::exists(destination / "docs" / "a.txt"));
    EXPECT_TRUE(fs::exists(destination / "docs" / "sub" / "b.txt"));
}

TEST_F(FileTransferIntegrationTest, LargeFileChunkMathSupportsManyChunks) {
    const uint64_t size = static_cast<uint64_t>(kFileChunkSize) * 1024 + 17;
    const uint32_t chunks = static_cast<uint32_t>((size + kFileChunkSize - 1) / kFileChunkSize);
    EXPECT_EQ(chunks, 1025u);
}
