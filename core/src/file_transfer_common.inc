using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string ToHex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

uint8_t HexCharToNibble(char value) {
    if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<uint8_t>(value - 'A' + 10);
    return 0;
}

void WriteU32LE(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t ReadU32LE(const uint8_t* src) {
    return static_cast<uint32_t>(src[0])
         | (static_cast<uint32_t>(src[1]) << 8)
         | (static_cast<uint32_t>(src[2]) << 16)
         | (static_cast<uint32_t>(src[3]) << 24);
}

json EntryToJson(const RemoteFsEntry& entry) {
    return json{
        {"name", entry.name},
        {"absolute_path", entry.absolute_path},
        {"is_directory", entry.is_directory},
        {"is_access_denied", entry.is_access_denied},
        {"size_bytes", entry.size_bytes},
        {"modified_unix_sec", entry.modified_unix_sec}
    };
}

RemoteFsEntry JsonToEntry(const json& value) {
    RemoteFsEntry entry;
    entry.name              = value.value("name", "");
    entry.absolute_path     = value.value("absolute_path", "");
    entry.is_directory      = value.value("is_directory", false);
    entry.is_access_denied  = value.value("is_access_denied", false);
    entry.size_bytes        = value.value("size_bytes", uint64_t{0});
    entry.modified_unix_sec = value.value("modified_unix_sec", int64_t{0});
    return entry;
}

json ConflictToJson(const TransferConflictInfo& conflict) {
    return json{
        {"relative_path", conflict.relative_path},
        {"incoming_is_directory", conflict.incoming_is_directory},
        {"existing_is_directory", conflict.existing_is_directory},
        {"type_mismatch", conflict.type_mismatch}
    };
}

std::optional<TransferConflictInfo> JsonToConflict(const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    TransferConflictInfo conflict;
    conflict.relative_path = value.value("relative_path", "");
    const fs::path relative = fs::path(conflict.relative_path).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    conflict.incoming_is_directory = value.value("incoming_is_directory", false);
    conflict.existing_is_directory = value.value("existing_is_directory", false);
    conflict.type_mismatch = value.value("type_mismatch",
                                         conflict.incoming_is_directory != conflict.existing_is_directory);
    return conflict;
}

} // namespace

FileTransfer::FileTransfer() {
    preview_timeout_thread_ = std::thread([this]() {
        PreviewTimeoutLoop();
    });
}

FileTransfer::~FileTransfer() {
    Shutdown();

    {
        std::lock_guard lock(mutex_);
        preview_thread_shutdown_ = true;
    }
    preview_cv_.notify_all();

    if (preview_timeout_thread_.joinable()) {
        preview_timeout_thread_.join();
    }
}

void FileTransfer::Initialize(WebRTCPeer* peer, const fs::path& default_receive_dir) {
    std::lock_guard lock(mutex_);
    peer_ = peer;
    default_receive_dir_ = default_receive_dir;
    std::error_code ec;
    fs::create_directories(default_receive_dir_, ec);
}

void FileTransfer::Shutdown() {
    std::lock_guard lock(mutex_);

    for (auto& [_, entry] : active_entries_) {
        if (entry.file && entry.file->is_open()) {
            entry.file->close();
        }
        std::error_code ec;
        fs::remove(entry.temp_path, ec);
    }

    peer_ = nullptr;
    send_jobs_.clear();
    receive_jobs_.clear();
    active_entries_.clear();
    pending_previews_.clear();
    pending_receive_roots_.clear();
    preview_cv_.notify_all();
}

void FileTransfer::SetListener(FileTransferListener* listener) {
    std::lock_guard lock(mutex_);
    listener_ = listener;
}

void FileTransfer::SetPreviewResponseTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        timeout = std::chrono::milliseconds(1);
    }

    {
        std::lock_guard lock(mutex_);
        preview_response_timeout_ = timeout;
    }
    preview_cv_.notify_all();
}

void FileTransfer::RequestRemoteRoots(uint64_t request_id) {
    if (request_id == 0) {
        request_id = NextRequestId();
    }

    SendControlJson("fs_roots_request", [request_id](std::string& fields) {
        fields += ",\"request_id\":";
        fields += std::to_string(request_id);
    });
}

void FileTransfer::RequestRemoteListing(const fs::path& remote_path, uint64_t request_id) {
    if (request_id == 0) {
        request_id = NextRequestId();
    }

    const std::string path = NormalizeSlashes(remote_path.string());
    SendControlJson("fs_list_request", [request_id, &path](std::string& fields) {
        fields += ",\"request_id\":";
        fields += std::to_string(request_id);
        fields += ",\"path\":\"";
        fields += EscapeJson(path);
        fields += "\"";
    });
}

std::string FileTransfer::SendLocalPaths(const std::vector<fs::path>& source_paths,
                                         const fs::path& peer_destination_root,
                                         TransferOverwritePolicy overwrite_policy) {
    return StartSendJob(source_paths, peer_destination_root, std::nullopt, overwrite_policy);
}

std::string FileTransfer::RequestRemotePaths(const std::vector<fs::path>& remote_paths,
                                             const fs::path& local_destination_root,
                                             TransferOverwritePolicy overwrite_policy) {
    return RequestRemotePathsInternal(remote_paths,
                                      local_destination_root,
                                      std::nullopt,
                                      overwrite_policy);
}

std::string FileTransfer::PreviewSendLocalPaths(const std::vector<fs::path>& source_paths,
                                                const fs::path& peer_destination_root) {
    uint64_t total_bytes = 0;
    auto entries = BuildManifest(source_paths, total_bytes);
    if (entries.empty()) {
        return {};
    }

    const FileId job_id = GenerateFileId();
    const std::string job_id_hex = FileIdToHex(job_id);

    PendingPreviewState state;
    state.info.id = job_id;
    state.info.id_hex = job_id_hex;
    state.info.direction = TransferDirection::Send;
    state.info.state = TransferState::Pending;
    state.info.destination_root = NormalizeSlashes(peer_destination_root.string());
    state.info.total_bytes = total_bytes;
    state.info.total_entries = static_cast<uint32_t>(entries.size());
    state.kind = PreviewKind::Upload;
    state.destination_root = peer_destination_root;
    state.manifest = entries;

    {
        std::lock_guard lock(mutex_);
        state.deadline = std::chrono::steady_clock::now() + preview_response_timeout_;
        pending_previews_[job_id_hex] = state;
    }
    preview_cv_.notify_all();

    json payload;
    payload["type"] = "transfer_preview_request";
    payload["job_id"] = job_id_hex;
    payload["preview_kind"] = "upload";
    payload["destination_root"] = state.info.destination_root;
    payload["entries"] = json::array();
    for (const auto& entry : entries) {
        payload["entries"].push_back({
            {"relative_path", entry.relative_path},
            {"is_directory", entry.is_directory},
            {"size", entry.size_bytes},
            {"chunks", entry.total_chunks}
        });
    }

    SendControlRaw(payload.dump());
    return job_id_hex;
}

std::string FileTransfer::PreviewRemotePaths(const std::vector<fs::path>& remote_paths,
                                             const fs::path& local_destination_root) {
    if (remote_paths.empty()) {
        return {};
    }

    const FileId job_id = GenerateFileId();
    const std::string job_id_hex = FileIdToHex(job_id);

    PendingPreviewState state;
    state.info.id = job_id;
    state.info.id_hex = job_id_hex;
    state.info.direction = TransferDirection::Receive;
    state.info.state = TransferState::Pending;
    state.info.destination_root = NormalizeSlashes(local_destination_root.string());
    state.kind = PreviewKind::Download;
    state.destination_root = local_destination_root;
    state.source_paths = remote_paths;

    {
        std::lock_guard lock(mutex_);
        state.deadline = std::chrono::steady_clock::now() + preview_response_timeout_;
        pending_previews_[job_id_hex] = state;
    }
    preview_cv_.notify_all();

    json payload;
    payload["type"] = "transfer_preview_request";
    payload["job_id"] = job_id_hex;
    payload["preview_kind"] = "download";
    payload["source_paths"] = json::array();
    for (const auto& path : remote_paths) {
        payload["source_paths"].push_back(NormalizeSlashes(path.string()));
    }

    SendControlRaw(payload.dump());
    return job_id_hex;
}

bool FileTransfer::ResolveTransferPreview(const std::string& job_id_hex, bool allow_overwrite) {
    PendingPreviewState state;
    bool found = false;

    {
        std::lock_guard lock(mutex_);
        auto it = pending_previews_.find(job_id_hex);
        if (it == pending_previews_.end() || it->second.waiting_for_peer) {
            return false;
        }

        state = std::move(it->second);
        pending_previews_.erase(it);
        found = true;
    }

    if (!found) {
        return false;
    }

    if (!allow_overwrite) {
        state.info.state = TransferState::Failed;
        state.info.error = "conflict-declined: overwrite not approved";
        if (listener_) {
            listener_->OnTransferConflictPreviewFailed(state.info, state.info.error);
        }
        return true;
    }

    state.info.overwrite_allowed = true;

    std::string started_job_id;
    if (state.kind == PreviewKind::Upload) {
        started_job_id = StartPreparedSendJob(std::move(state.manifest),
                                              state.info.total_bytes,
                                              state.destination_root,
                                              state.info.id,
                                              TransferOverwritePolicy::Allow);
    } else {
        started_job_id = RequestRemotePathsInternal(state.source_paths,
                                                    state.destination_root,
                                                    state.info.id,
                                                    TransferOverwritePolicy::Allow);
    }

    if (!started_job_id.empty()) {
        return true;
    }

    state.info.state = TransferState::Failed;
    state.info.error = "preview-unsupported: failed to start approved transfer";
    if (listener_) {
        listener_->OnTransferConflictPreviewFailed(state.info, state.info.error);
    }
    return false;
}

void FileTransfer::OnDataReceived(const std::vector<uint8_t>& data, bool is_binary) {
    if (is_binary) {
        HandleChunk(data.data(), data.size());
        return;
    }

    const std::string payload(data.begin(), data.end());
    HandleControlMessage(payload);
}

void FileTransfer::Cancel(const std::string& job_id_hex, const std::string& reason) {
    TransferJobInfo cancelled;
    bool notify = false;

    {
        std::lock_guard lock(mutex_);

        if (auto it = pending_previews_.find(job_id_hex); it != pending_previews_.end()) {
            it->second.info.state = TransferState::Cancelled;
            it->second.info.error = reason;
            cancelled = it->second.info;
            pending_previews_.erase(it);
            notify = true;
        }

        if (auto it = send_jobs_.find(job_id_hex); it != send_jobs_.end()) {
            it->second.info.state = TransferState::Cancelled;
            it->second.info.error = reason;
            cancelled = it->second.info;
            notify = true;
        }

        if (auto it = receive_jobs_.find(job_id_hex); it != receive_jobs_.end()) {
            it->second.info.state = TransferState::Cancelled;
            it->second.info.error = reason;
            cancelled = it->second.info;
            notify = true;
        }
    }
    preview_cv_.notify_all();

    json payload;
    payload["type"] = "transfer_cancel";
    payload["job_id"] = job_id_hex;
    payload["reason"] = reason;
    SendControlRaw(payload.dump());

    if (notify && listener_) {
        listener_->OnTransferJobCancelled(cancelled);
    }
}

std::optional<TransferJobInfo> FileTransfer::GetJobInfo(const std::string& job_id_hex) const {
    std::lock_guard lock(mutex_);
    if (auto it = pending_previews_.find(job_id_hex); it != pending_previews_.end()) {
        return it->second.info;
    }
    if (auto it = send_jobs_.find(job_id_hex); it != send_jobs_.end()) {
        return it->second.info;
    }
    if (auto it = receive_jobs_.find(job_id_hex); it != receive_jobs_.end()) {
        return it->second.info;
    }
    return std::nullopt;
}

std::vector<TransferJobInfo> FileTransfer::GetActiveJobs() const {
    std::lock_guard lock(mutex_);
    std::vector<TransferJobInfo> jobs;

    for (const auto& [_, preview] : pending_previews_) {
        if (preview.info.state == TransferState::InProgress || preview.info.state == TransferState::Pending) {
            jobs.push_back(preview.info);
        }
    }
    for (const auto& [_, job] : send_jobs_) {
        if (job.info.state == TransferState::InProgress || job.info.state == TransferState::Pending) {
            jobs.push_back(job.info);
        }
    }
    for (const auto& [_, job] : receive_jobs_) {
        if (job.info.state == TransferState::InProgress || job.info.state == TransferState::Pending) {
            jobs.push_back(job.info);
        }
    }

    return jobs;
}

void FileTransfer::HandleControlMessage(const std::string& raw_json) {
    json payload;
    try {
        payload = json::parse(raw_json);
    } catch (...) {
        return;
    }

    const std::string type = payload.value("type", "");
    if (type == "fs_roots_request") {
        HandleRootsRequest(payload.value("request_id", uint64_t{0}));
    } else if (type == "fs_list_request") {
        HandleListRequest(payload.value("request_id", uint64_t{0}), payload.value("path", ""));
    } else if (type == "fs_roots_response" || type == "fs_list_response") {
        HandleListingResponse(raw_json);
    } else if (type == "transfer_preview_request") {
        HandleTransferPreviewRequest(raw_json);
    } else if (type == "transfer_preview_response") {
        HandleTransferPreviewResponse(raw_json);
    } else if (type == "transfer_job_request") {
        HandleTransferJobRequest(raw_json);
    } else if (type == "transfer_job_start") {
        HandleTransferJobStart(raw_json);
    } else if (type == "transfer_entry_start") {
        HandleTransferEntryStart(raw_json);
    } else if (type == "transfer_job_complete") {
        HandleTransferJobComplete(raw_json);
    } else if (type == "transfer_job_result") {
        HandleTransferJobResult(raw_json);
    } else if (type == "transfer_cancel") {
        HandleTransferCancel(raw_json);
    }
}

void FileTransfer::HandleChunk(const uint8_t* data, size_t size) {
    if (size < kChunkHeaderSize) {
        return;
    }

    FileId entry_id{};
    std::memcpy(entry_id.data(), data, kFileIdSize);
    const std::string entry_id_hex = FileIdToHex(entry_id);
    const uint32_t chunk_index = ReadU32LE(data + kFileIdSize);
    const uint8_t* payload = data + kChunkHeaderSize;
    const size_t payload_size = size - kChunkHeaderSize;

    TransferJobInfo job_info;
    TransferEntryInfo entry_info;
    bool notify_progress = false;
    bool should_finalize = false;
    std::string job_id_hex;

    bool write_failed = false;
    std::string failed_job_id;

    {
        std::lock_guard lock(mutex_);
        auto entry_it = active_entries_.find(entry_id_hex);
        if (entry_it == active_entries_.end()) {
            return;
        }

        auto& entry = entry_it->second;
        if (!entry.file || !entry.file->is_open()) {
            return;
        }

        const auto offset = static_cast<std::streamoff>(chunk_index) * static_cast<std::streamoff>(kFileChunkSize);
        entry.file->seekp(offset);
        entry.file->write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(payload_size));
        if (!entry.file->good()) {
            failed_job_id = entry.info.job_id_hex;
            entry.file->close();
            active_entries_.erase(entry_it);
            write_failed = true;
        } else {
            entry.info.chunks_done = std::max(entry.info.chunks_done, chunk_index + 1);
            entry.info.bytes_done += payload_size;
            entry.info.state = TransferState::InProgress;
            job_id_hex = entry.info.job_id_hex;

            auto job_it = receive_jobs_.find(job_id_hex);
            if (job_it == receive_jobs_.end()) {
                return;
            }

            auto& job = job_it->second;
            job.info.current_item = entry.info.relative_path;
            job.info.bytes_done += payload_size;
            job.info.state = TransferState::InProgress;
            job.entries[entry_id_hex] = entry.info;

            job_info = job.info;
            entry_info = entry.info;
            notify_progress = true;
            should_finalize = (entry.info.bytes_done >= entry.info.file_size);
        }
    }

    if (write_failed) {
        FailReceiveJob(failed_job_id, "failed to write received file");
        return;
    }

    if (notify_progress && listener_) {
        listener_->OnTransferJobProgress(job_info, entry_info);
    }

    if (should_finalize) {
        FinalizeReceiveEntry(entry_id_hex);
    }
}

void FileTransfer::HandleRootsRequest(uint64_t request_id) {
    RemoteFsListing listing;
    listing.request_id = request_id;
    listing.is_root_listing = true;
    listing.success = true;

    DWORD mask = GetLogicalDrives();
    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        const DWORD bit = 1u << (drive - 'A');
        if ((mask & bit) == 0) {
            continue;
        }

        std::string path;
        path.push_back(drive);
        path += ":\\";

        RemoteFsEntry entry;
        entry.name = path;
        entry.absolute_path = path;
        entry.is_directory = true;
        listing.entries.push_back(std::move(entry));
    }

    json payload;
    payload["type"] = "fs_roots_response";
    payload["request_id"] = request_id;
    payload["path"] = "";
    payload["is_root_listing"] = true;
    payload["success"] = true;
    payload["entries"] = json::array();
    for (const auto& entry : listing.entries) {
        payload["entries"].push_back(EntryToJson(entry));
    }
    SendControlRaw(payload.dump());
}

void FileTransfer::HandleListRequest(uint64_t request_id, const std::string& path) {
    RemoteFsListing listing;
    listing.request_id = request_id;
    listing.path = path;
    listing.is_root_listing = false;
    listing.success = true;

    std::error_code ec;
    fs::path dir_path(path);

    if (path.empty()) {
        HandleRootsRequest(request_id);
        return;
    }

    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        listing.success = false;
        listing.error = "directory not found";
    } else {
        std::vector<RemoteFsEntry> entries;
        for (fs::directory_iterator it(dir_path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            RemoteFsEntry entry;
            entry.name = it->path().filename().string();
            entry.absolute_path = NormalizeSlashes(it->path().string());
            entry.is_directory = it->is_directory(ec);
            entry.is_access_denied = false;
            if (!entry.is_directory) {
                std::error_code size_ec;
                entry.size_bytes = it->file_size(size_ec);
            }
            std::error_code time_ec;
            const auto last_write = it->last_write_time(time_ec);
            if (!time_ec) {
                entry.modified_unix_sec = FileTimeToUnixSeconds(last_write);
            }
            entries.push_back(std::move(entry));
        }

        std::sort(entries.begin(), entries.end(), [](const RemoteFsEntry& lhs, const RemoteFsEntry& rhs) {
            if (lhs.is_directory != rhs.is_directory) {
                return lhs.is_directory > rhs.is_directory;
            }
            return _stricmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
        });

        listing.entries = std::move(entries);
    }

    json payload;
    payload["type"] = "fs_list_response";
    payload["request_id"] = request_id;
    payload["path"] = path;
    payload["is_root_listing"] = false;
    payload["success"] = listing.success;
    payload["error"] = listing.error;
    payload["entries"] = json::array();
    for (const auto& entry : listing.entries) {
        payload["entries"].push_back(EntryToJson(entry));
    }
    SendControlRaw(payload.dump());
}

void FileTransfer::HandleListingResponse(const std::string& raw_json) {
    RemoteFsListing listing;

    try {
        const json payload = json::parse(raw_json);
        listing.request_id = payload.value("request_id", uint64_t{0});
        listing.path = payload.value("path", "");
        listing.is_root_listing = payload.value("is_root_listing", false);
        listing.success = payload.value("success", false);
        listing.error = payload.value("error", "");
        if (payload.contains("entries") && payload["entries"].is_array()) {
            for (const auto& entry : payload["entries"]) {
                listing.entries.push_back(JsonToEntry(entry));
            }
        }
    } catch (...) {
        return;
    }

    if (listener_) {
        listener_->OnRemoteListing(listing);
    }
}

void FileTransfer::HandleTransferPreviewRequest(const std::string& raw_json) {
    auto send_failure = [this](const std::string& job_id_hex,
                               const std::string& preview_kind,
                               const std::string& error) {
        json response;
        response["type"] = "transfer_preview_response";
        response["job_id"] = job_id_hex;
        response["preview_kind"] = preview_kind;
        response["success"] = false;
        response["error"] = error;
        SendControlRaw(response.dump());
    };

    try {
        const json payload = json::parse(raw_json);
        const std::string job_id_hex = payload.value("job_id", "");
        const std::string preview_kind = payload.value("preview_kind", "");
        if (job_id_hex.empty()) {
            return;
        }

        if (preview_kind == "upload") {
            const std::string destination_root = payload.value("destination_root", "");
            if (destination_root.empty() ||
                !payload.contains("entries") ||
                !payload["entries"].is_array()) {
                send_failure(job_id_hex,
                             preview_kind,
                             "preview-malformed: incomplete upload preview request");
                return;
            }

            std::vector<ManifestEntry> manifest;
            for (const auto& value : payload["entries"]) {
                const std::string relative_path = value.value("relative_path", "");
                const fs::path relative = WireToRelativePath(relative_path);
                if (!IsSafeRelativePath(relative)) {
                    send_failure(job_id_hex,
                                 preview_kind,
                                 "preview-malformed: unsafe relative path in upload preview");
                    return;
                }

                ManifestEntry entry;
                entry.relative_path = relative_path;
                entry.is_directory = value.value("is_directory", false);
                entry.size_bytes = value.value("size", uint64_t{0});
                entry.total_chunks = value.value("chunks", uint32_t{0});
                manifest.push_back(std::move(entry));
            }

            if (manifest.empty()) {
                send_failure(job_id_hex,
                             preview_kind,
                             "preview-malformed: empty upload preview request");
                return;
            }

            const auto conflicts = CollectConflicts(fs::path(destination_root), manifest);
            json response;
            response["type"] = "transfer_preview_response";
            response["job_id"] = job_id_hex;
            response["preview_kind"] = preview_kind;
            response["success"] = true;
            response["conflicts"] = json::array();
            for (const auto& conflict : conflicts) {
                response["conflicts"].push_back(ConflictToJson(conflict));
            }
            SendControlRaw(response.dump());
            return;
        }

        if (preview_kind == "download") {
            std::vector<fs::path> source_paths;
            if (!payload.contains("source_paths") || !payload["source_paths"].is_array()) {
                send_failure(job_id_hex,
                             preview_kind,
                             "preview-malformed: incomplete download preview request");
                return;
            }

            for (const auto& value : payload["source_paths"]) {
                if (!value.is_string()) {
                    send_failure(job_id_hex,
                                 preview_kind,
                                 "preview-malformed: invalid download preview path");
                    return;
                }
                source_paths.emplace_back(value.get<std::string>());
            }

            uint64_t total_bytes = 0;
            auto manifest = BuildManifest(source_paths, total_bytes);
            if (manifest.empty()) {
                send_failure(job_id_hex,
                             preview_kind,
                             "preview-malformed: empty preview result");
                return;
            }

            json response;
            response["type"] = "transfer_preview_response";
            response["job_id"] = job_id_hex;
            response["preview_kind"] = preview_kind;
            response["success"] = true;
            response["manifest"] = json::array();
            for (const auto& entry : manifest) {
                response["manifest"].push_back({
                    {"relative_path", entry.relative_path},
                    {"is_directory", entry.is_directory},
                    {"size", entry.size_bytes},
                    {"chunks", entry.total_chunks}
                });
            }
            SendControlRaw(response.dump());
            return;
        }

        send_failure(job_id_hex,
                     preview_kind,
                     "preview-unsupported: unsupported preview kind");
    } catch (...) {
    }
}

void FileTransfer::HandleTransferPreviewResponse(const std::string& raw_json) {
    PendingPreviewState start_state;
    std::vector<fs::path> start_paths;
    fs::path start_destination;
    bool start_upload = false;
    bool start_download = false;
    bool start_with_overwrite = false;

    TransferConflictPreview preview;
    bool notify_preview_ready = false;

    TransferJobInfo failed_info;
    std::string failed_error;
    bool notify_preview_failed = false;

    try {
        const json payload = json::parse(raw_json);
        const std::string job_id_hex = payload.value("job_id", "");
        const std::string preview_kind = payload.value("preview_kind", "");
        const bool success = payload.value("success", false);
        const std::string remote_error = payload.value("error", "");

        std::lock_guard lock(mutex_);
        auto it = pending_previews_.find(job_id_hex);
        if (it == pending_previews_.end() || !it->second.waiting_for_peer) {
            return;
        }

        auto fail_preview = [&](const std::string& error) {
            failed_info = it->second.info;
            failed_info.state = TransferState::Failed;
            failed_info.error = error;
            failed_error = error;
            pending_previews_.erase(it);
            notify_preview_failed = true;
        };

        const bool expecting_upload = it->second.kind == PreviewKind::Upload;
        if ((expecting_upload && preview_kind != "upload") ||
            (!expecting_upload && preview_kind != "download")) {
            fail_preview("preview-malformed: preview kind mismatch");
        } else if (!success) {
            fail_preview(remote_error.empty()
                             ? "preview-unsupported: peer rejected conflict preview"
                             : remote_error);
        } else if (expecting_upload) {
            if (!payload.contains("conflicts") || !payload["conflicts"].is_array()) {
                fail_preview("preview-malformed: incomplete upload preview response");
            } else {
                std::vector<TransferConflictInfo> conflicts;
                for (const auto& value : payload["conflicts"]) {
                    auto conflict = JsonToConflict(value);
                    if (!conflict.has_value()) {
                        fail_preview("preview-malformed: invalid upload preview conflict");
                        break;
                    }
                    conflicts.push_back(*conflict);
                }

                if (!notify_preview_failed) {
                    it->second.conflicts = conflicts;
                    it->second.waiting_for_peer = false;
                    if (conflicts.empty()) {
                        start_state = std::move(it->second);
                        pending_previews_.erase(it);
                        start_upload = true;
                        start_with_overwrite = false;
                    } else {
                        preview.job_id_hex = job_id_hex;
                        preview.direction = it->second.info.direction;
                        preview.total_bytes = it->second.info.total_bytes;
                        preview.total_entries = it->second.info.total_entries;
                        preview.conflicts = conflicts;
                        notify_preview_ready = true;
                    }
                }
            }
        } else {
            if (!payload.contains("manifest") || !payload["manifest"].is_array()) {
                fail_preview("preview-malformed: incomplete download preview response");
            } else {
                std::vector<ManifestEntry> manifest;
                uint64_t total_bytes = 0;
                for (const auto& value : payload["manifest"]) {
                    const std::string relative_path = value.value("relative_path", "");
                    const fs::path relative = WireToRelativePath(relative_path);
                    if (!IsSafeRelativePath(relative)) {
                        fail_preview("preview-malformed: invalid download preview manifest");
                        break;
                    }

                    ManifestEntry entry;
                    entry.relative_path = relative_path;
                    entry.is_directory = value.value("is_directory", false);
                    entry.size_bytes = value.value("size", uint64_t{0});
                    entry.total_chunks = value.value("chunks", uint32_t{0});
                    total_bytes += entry.size_bytes;
                    manifest.push_back(std::move(entry));
                }

                if (!notify_preview_failed && manifest.empty()) {
                    fail_preview("preview-malformed: empty preview result");
                }

                if (!notify_preview_failed) {
                    it->second.info.total_entries = static_cast<uint32_t>(manifest.size());
                    it->second.info.total_bytes = total_bytes;
                    auto conflicts = CollectConflicts(it->second.destination_root, manifest);
                    it->second.conflicts = conflicts;
                    it->second.waiting_for_peer = false;
                    if (conflicts.empty()) {
                        start_paths = it->second.source_paths;
                        start_destination = it->second.destination_root;
                        start_state = it->second;
                        pending_previews_.erase(it);
                        start_download = true;
                        start_with_overwrite = false;
                    } else {
                        preview.job_id_hex = job_id_hex;
                        preview.direction = it->second.info.direction;
                        preview.total_bytes = total_bytes;
                        preview.total_entries = static_cast<uint32_t>(manifest.size());
                        preview.conflicts = conflicts;
                        notify_preview_ready = true;
                    }
                }
            }
        }
    } catch (...) {
        return;
    }

    if (notify_preview_failed && listener_) {
        listener_->OnTransferConflictPreviewFailed(failed_info, failed_error);
        return;
    }

    if (notify_preview_ready && listener_) {
        listener_->OnTransferConflictPreviewReady(preview);
        return;
    }

    if (start_upload) {
        const auto started = StartPreparedSendJob(std::move(start_state.manifest),
                                                  start_state.info.total_bytes,
                                                  start_state.destination_root,
                                                  start_state.info.id,
                                                  start_with_overwrite
                                                      ? TransferOverwritePolicy::Allow
                                                      : TransferOverwritePolicy::Deny);
        if (started.empty() && listener_) {
            TransferJobInfo info = start_state.info;
            info.state = TransferState::Failed;
            info.error = "preview-unsupported: failed to start previewed upload";
            listener_->OnTransferConflictPreviewFailed(info, info.error);
        }
        return;
    }

    if (start_download) {
        const auto started = RequestRemotePathsInternal(start_paths,
                                                        start_destination,
                                                        start_state.info.id,
                                                        start_with_overwrite
                                                            ? TransferOverwritePolicy::Allow
                                                            : TransferOverwritePolicy::Deny);
        if (started.empty() && listener_) {
            TransferJobInfo info = start_state.info;
            info.state = TransferState::Failed;
            info.error = "preview-unsupported: failed to start previewed download";
            listener_->OnTransferConflictPreviewFailed(info, info.error);
        }
    }
}

void FileTransfer::HandleTransferJobRequest(const std::string& raw_json) {
    try {
        const json payload = json::parse(raw_json);
        std::vector<fs::path> source_paths;
        if (payload.contains("source_paths") && payload["source_paths"].is_array()) {
            for (const auto& value : payload["source_paths"]) {
                source_paths.emplace_back(value.get<std::string>());
            }
        }

        const auto job_id = HexToFileId(payload.value("job_id", ""));
        const bool overwrite_allowed = payload.value("overwrite_allowed", false);
        if (!source_paths.empty()) {
            StartSendJob(source_paths,
                         {},
                         job_id,
                         overwrite_allowed
                             ? TransferOverwritePolicy::Allow
                             : TransferOverwritePolicy::Deny);
        }
    } catch (...) {
    }
}

std::vector<TransferConflictInfo> FileTransfer::CollectConflicts(const fs::path& destination_root,
                                                                 const std::vector<ManifestEntry>& manifest) const {
    std::vector<TransferConflictInfo> conflicts;
    conflicts.reserve(manifest.size());

    for (const auto& entry : manifest) {
        TransferConflictInfo conflict;
        if (TryGetConflict(destination_root / WireToRelativePath(entry.relative_path),
                           entry.relative_path,
                           entry.is_directory,
                           conflict)) {
            conflicts.push_back(std::move(conflict));
        }
    }

    return conflicts;
}

bool FileTransfer::TryGetConflict(const fs::path& final_path,
                                  const std::string& relative_path,
                                  bool incoming_is_directory,
                                  TransferConflictInfo& conflict) const {
    std::error_code ec;
    if (!fs::exists(final_path, ec)) {
        return false;
    }

    const bool existing_is_directory = fs::is_directory(final_path, ec);
    if (incoming_is_directory && existing_is_directory) {
        return false;
    }

    conflict.relative_path = relative_path;
    conflict.incoming_is_directory = incoming_is_directory;
    conflict.existing_is_directory = existing_is_directory;
    conflict.type_mismatch = incoming_is_directory != existing_is_directory;
    return true;
}

bool FileTransfer::RemoveExistingDestination(const fs::path& path, bool existing_is_directory) const {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return true;
    }

    if (existing_is_directory) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }
    return !ec;
}

std::string FileTransfer::BuildConflictError(const TransferConflictInfo& conflict, bool late_conflict) {
    std::string error = late_conflict ? "late-conflict: " : "conflict-declined: ";
    if (!conflict.relative_path.empty()) {
        error += conflict.relative_path;
    } else {
        error += "destination";
    }

    if (conflict.type_mismatch) {
        error += conflict.incoming_is_directory
            ? " collides with an existing file"
            : " collides with an existing directory";
    } else {
        error += " already exists";
    }
    return error;
}

void FileTransfer::PreviewTimeoutLoop() {
    std::unique_lock lock(mutex_);

    auto has_waiting_preview = [this]() {
        for (const auto& [_, preview] : pending_previews_) {
            if (preview.waiting_for_peer) {
                return true;
            }
        }
        return false;
    };

    while (!preview_thread_shutdown_) {
        auto next_deadline = std::chrono::steady_clock::time_point::max();
        for (const auto& [_, preview] : pending_previews_) {
            if (preview.waiting_for_peer) {
                next_deadline = std::min(next_deadline, preview.deadline);
            }
        }

        if (next_deadline == std::chrono::steady_clock::time_point::max()) {
            preview_cv_.wait(lock, [&]() {
                return preview_thread_shutdown_ || has_waiting_preview();
            });
        } else {
            preview_cv_.wait_until(lock, next_deadline, [this, &has_waiting_preview, next_deadline]() {
                if (preview_thread_shutdown_) {
                    return true;
                }
                if (!has_waiting_preview()) {
                    return true;
                }
                for (const auto& [_, preview] : pending_previews_) {
                    if (preview.waiting_for_peer && preview.deadline < next_deadline) {
                        return true;
                    }
                }
                return false;
            });
        }

        if (preview_thread_shutdown_) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<TransferJobInfo> expired_jobs;
        for (auto it = pending_previews_.begin(); it != pending_previews_.end();) {
            if (it->second.waiting_for_peer && it->second.deadline <= now) {
                it->second.info.state = TransferState::Failed;
                it->second.info.error = "preview-timeout: peer did not answer preview request";
                expired_jobs.push_back(it->second.info);
                it = pending_previews_.erase(it);
            } else {
                ++it;
            }
        }

        if (expired_jobs.empty()) {
            continue;
        }

        auto* listener = listener_;
        lock.unlock();
        if (listener) {
            for (const auto& info : expired_jobs) {
                listener->OnTransferConflictPreviewFailed(info, info.error);
            }
        }
        lock.lock();
    }
}

FileId FileTransfer::GenerateFileId() {
    FileId id{};
    BCryptGenRandom(nullptr, id.data(), static_cast<ULONG>(id.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return id;
}

std::string FileTransfer::FileIdToHex(const FileId& id) {
    return ToHex(id.data(), id.size());
}

FileId FileTransfer::HexToFileId(const std::string& hex) {
    FileId id{};
    const size_t bytes = std::min(hex.size() / 2, id.size());
    for (size_t i = 0; i < bytes; ++i) {
        id[i] = static_cast<uint8_t>((HexCharToNibble(hex[i * 2]) << 4) |
                                     HexCharToNibble(hex[i * 2 + 1]));
    }
    return id;
}

uint64_t FileTransfer::NextRequestId() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1);
}

std::string FileTransfer::EscapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string FileTransfer::NormalizeSlashes(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}

std::string FileTransfer::RelativePathToWire(const fs::path& path) {
    std::string value = path.generic_string();
    if (value.empty()) {
        value = path.filename().generic_string();
    }
    return value;
}

fs::path FileTransfer::WireToRelativePath(const std::string& path) {
    return fs::path(path).lexically_normal();
}

bool FileTransfer::IsSafeRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }

    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

int64_t FileTransfer::FileTimeToUnixSeconds(const fs::file_time_type& tp) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto file_now = fs::file_time_type::clock::now();
    const auto adjusted = now + duration_cast<system_clock::duration>(tp - file_now);
    return duration_cast<seconds>(adjusted.time_since_epoch()).count();
}
