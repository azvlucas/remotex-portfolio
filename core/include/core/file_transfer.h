#pragma once
// RemoteX - core/file_transfer.h
// Gerenciador de transferencias e navegacao remota via WebRTC Data Channel "files".

#include "core/webrtc_peer.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace remotex::core {

inline constexpr size_t kFileChunkSize   = 64 * 1024;
inline constexpr size_t kFileIdSize      = 16;
inline constexpr size_t kChunkHeaderSize = kFileIdSize + 4;
inline constexpr const char* kFilesChannel = "files";

enum class TransferDirection {
    Send,
    Receive
};

enum class TransferState {
    Pending,
    InProgress,
    Completed,
    Failed,
    Cancelled
};

enum class TransferOverwritePolicy {
    Deny,
    Allow
};

using FileId = std::array<uint8_t, kFileIdSize>;

struct RemoteFsEntry {
    std::string name;
    std::string absolute_path;
    bool        is_directory      = false;
    bool        is_access_denied  = false;
    uint64_t    size_bytes        = 0;
    int64_t     modified_unix_sec = 0;
};

struct RemoteFsListing {
    uint64_t                    request_id      = 0;
    std::string                 path;
    bool                        is_root_listing = false;
    bool                        success         = true;
    std::string                 error;
    std::vector<RemoteFsEntry>  entries;
};

struct TransferEntryInfo {
    FileId            id{};
    std::string       id_hex;
    std::string       job_id_hex;
    std::string       name;
    std::string       relative_path;
    bool              is_directory = false;
    uint64_t          file_size    = 0;
    uint32_t          total_chunks = 0;
    uint32_t          chunks_done  = 0;
    uint64_t          bytes_done   = 0;
    TransferDirection direction    = TransferDirection::Send;
    TransferState     state        = TransferState::Pending;
    std::string       error;
};

struct TransferConflictInfo {
    std::string relative_path;
    bool        incoming_is_directory = false;
    bool        existing_is_directory = false;
    bool        type_mismatch = false;
};

struct TransferConflictPreview {
    std::string                    job_id_hex;
    TransferDirection              direction = TransferDirection::Send;
    uint64_t                       total_bytes = 0;
    uint32_t                       total_entries = 0;
    std::vector<TransferConflictInfo> conflicts;
};

struct TransferJobInfo {
    FileId            id{};
    std::string       id_hex;
    TransferDirection direction      = TransferDirection::Send;
    TransferState     state          = TransferState::Pending;
    std::string       destination_root;
    uint64_t          total_bytes    = 0;
    uint64_t          bytes_done     = 0;
    uint32_t          total_entries  = 0;
    uint32_t          entries_done   = 0;
    std::string       current_item;
    std::string       error;
    bool              overwrite_allowed = false;
};

class FileTransferListener {
public:
    virtual ~FileTransferListener() = default;

    virtual void OnRemoteListing(const RemoteFsListing& listing) = 0;
    virtual void OnTransferJobStarted(const TransferJobInfo& info) = 0;
    virtual void OnTransferJobProgress(const TransferJobInfo& job,
                                       const TransferEntryInfo& entry) = 0;
    virtual void OnTransferJobCompleted(const TransferJobInfo& info,
                                        const std::filesystem::path& destination_root) = 0;
    virtual void OnTransferJobFailed(const TransferJobInfo& info,
                                     const std::string& error) = 0;
    virtual void OnTransferJobCancelled(const TransferJobInfo& info) = 0;
    virtual void OnTransferConflictPreviewReady(const TransferConflictPreview& preview) {}
    virtual void OnTransferConflictPreviewFailed(const TransferJobInfo& info,
                                                 const std::string& error) {}
};

class FileTransfer {
public:
    FileTransfer();
    ~FileTransfer();

    FileTransfer(const FileTransfer&)            = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    void Initialize(WebRTCPeer* peer, const std::filesystem::path& default_receive_dir);
    void Shutdown();

    void SetListener(FileTransferListener* listener);

    void RequestRemoteRoots(uint64_t request_id = 0);
    void RequestRemoteListing(const std::filesystem::path& remote_path, uint64_t request_id = 0);

    std::string SendLocalPaths(const std::vector<std::filesystem::path>& source_paths,
                               const std::filesystem::path& peer_destination_root,
                               TransferOverwritePolicy overwrite_policy = TransferOverwritePolicy::Deny);
    std::string RequestRemotePaths(const std::vector<std::filesystem::path>& remote_paths,
                                   const std::filesystem::path& local_destination_root,
                                   TransferOverwritePolicy overwrite_policy = TransferOverwritePolicy::Deny);
    std::string PreviewSendLocalPaths(const std::vector<std::filesystem::path>& source_paths,
                                      const std::filesystem::path& peer_destination_root);
    std::string PreviewRemotePaths(const std::vector<std::filesystem::path>& remote_paths,
                                   const std::filesystem::path& local_destination_root);
    bool ResolveTransferPreview(const std::string& job_id_hex, bool allow_overwrite);
    void SetPreviewResponseTimeout(std::chrono::milliseconds timeout);

    void OnDataReceived(const std::vector<uint8_t>& data, bool is_binary);
    void Cancel(const std::string& job_id_hex, const std::string& reason = "user cancelled");

    std::optional<TransferJobInfo> GetJobInfo(const std::string& job_id_hex) const;
    std::vector<TransferJobInfo> GetActiveJobs() const;

private:
    struct ManifestEntry {
        FileId                  id{};
        std::string             id_hex;
        std::filesystem::path   absolute_path;
        std::string             relative_path;
        bool                    is_directory = false;
        uint64_t                size_bytes   = 0;
        uint32_t                total_chunks = 0;
    };

    struct SendJobState {
        TransferJobInfo            info;
        std::filesystem::path      destination_root;
        std::vector<ManifestEntry> entries;
        bool                       waiting_result = false;
    };

    struct ReceiveJobState {
        TransferJobInfo                         info;
        std::filesystem::path                   destination_root;
        bool                                    sender_announced_complete = false;
        std::map<std::string, TransferEntryInfo> entries;
        std::map<std::string, std::filesystem::path> completed_paths;
        std::map<std::string, std::string>      entry_errors;
    };

    enum class PreviewKind {
        Upload,
        Download
    };

    struct PendingPreviewState {
        TransferJobInfo                    info;
        PreviewKind                        kind = PreviewKind::Upload;
        std::filesystem::path              destination_root;
        std::vector<std::filesystem::path> source_paths;
        std::vector<ManifestEntry>         manifest;
        std::vector<TransferConflictInfo>  conflicts;
        std::chrono::steady_clock::time_point deadline{};
        bool                               waiting_for_peer = true;
    };

    struct ActiveReceiveEntry {
        TransferEntryInfo       info;
        std::filesystem::path   temp_path;
        std::filesystem::path   final_path;
        std::unique_ptr<std::ofstream> file;
    };

    void HandleControlMessage(const std::string& json);
    void HandleChunk(const uint8_t* data, size_t size);

    void HandleRootsRequest(uint64_t request_id);
    void HandleListRequest(uint64_t request_id, const std::string& path);
    void HandleListingResponse(const std::string& json);

    void HandleTransferJobRequest(const std::string& json);
    void HandleTransferPreviewRequest(const std::string& json);
    void HandleTransferPreviewResponse(const std::string& json);
    void HandleTransferJobStart(const std::string& json);
    void HandleTransferEntryStart(const std::string& json);
    void HandleTransferJobComplete(const std::string& json);
    void HandleTransferJobResult(const std::string& json);
    void HandleTransferCancel(const std::string& json);

    std::string StartSendJob(const std::vector<std::filesystem::path>& source_paths,
                             const std::filesystem::path& peer_destination_root,
                             const std::optional<FileId>& existing_job_id,
                             TransferOverwritePolicy overwrite_policy);
    std::string StartPreparedSendJob(std::vector<ManifestEntry> entries,
                                     uint64_t total_bytes,
                                     const std::filesystem::path& peer_destination_root,
                                     const std::optional<FileId>& existing_job_id,
                                     TransferOverwritePolicy overwrite_policy);
    std::string RequestRemotePathsInternal(const std::vector<std::filesystem::path>& remote_paths,
                                           const std::filesystem::path& local_destination_root,
                                           const std::optional<FileId>& existing_job_id,
                                           TransferOverwritePolicy overwrite_policy);
    std::vector<ManifestEntry> BuildManifest(const std::vector<std::filesystem::path>& source_paths,
                                             uint64_t& total_bytes) const;
    void SendJobPayload(SendJobState& job_state);

    std::vector<TransferConflictInfo> CollectConflicts(const std::filesystem::path& destination_root,
                                                       const std::vector<ManifestEntry>& manifest) const;
    bool TryGetConflict(const std::filesystem::path& final_path,
                        const std::string& relative_path,
                        bool incoming_is_directory,
                        TransferConflictInfo& conflict) const;
    bool RemoveExistingDestination(const std::filesystem::path& path, bool existing_is_directory) const;
    static std::string BuildConflictError(const TransferConflictInfo& conflict, bool late_conflict);
    void PreviewTimeoutLoop();

    void FinalizeReceiveEntry(const std::string& entry_id_hex);
    void CompleteReceiveJobIfReady(const std::string& job_id_hex);
    void FailReceiveJob(const std::string& job_id_hex, const std::string& error);

    void SendControlJson(const std::string& type,
                         std::function<void(std::string&)> fill_fields) const;
    void SendControlRaw(const std::string& payload) const;
    void SendChunk(const FileId& id, uint32_t chunk_index,
                   const uint8_t* data, size_t size) const;

    static FileId GenerateFileId();
    static std::string FileIdToHex(const FileId& id);
    static FileId HexToFileId(const std::string& hex);
    static uint64_t NextRequestId();
    static std::string EscapeJson(const std::string& value);
    static std::string NormalizeSlashes(std::string value);
    static std::string RelativePathToWire(const std::filesystem::path& path);
    static std::filesystem::path WireToRelativePath(const std::string& path);
    static bool IsSafeRelativePath(const std::filesystem::path& path);
    static int64_t FileTimeToUnixSeconds(const std::filesystem::file_time_type& tp);

    WebRTCPeer*                  peer_ = nullptr;
    FileTransferListener*        listener_ = nullptr;
    std::filesystem::path        default_receive_dir_;

    mutable std::mutex           mutex_;
    std::condition_variable      preview_cv_;
    std::map<std::string, SendJobState>     send_jobs_;
    std::map<std::string, ReceiveJobState>  receive_jobs_;
    std::map<std::string, ActiveReceiveEntry> active_entries_;
    std::map<std::string, PendingPreviewState> pending_previews_;
    std::map<std::string, std::filesystem::path> pending_receive_roots_;
    std::chrono::milliseconds    preview_response_timeout_{std::chrono::seconds(3)};
    std::thread                  preview_timeout_thread_;
    bool                         preview_thread_shutdown_ = false;
};

} // namespace remotex::core
