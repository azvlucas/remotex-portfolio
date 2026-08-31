std::string FileTransfer::StartSendJob(const std::vector<fs::path>& source_paths,
                                       const fs::path& peer_destination_root,
                                       const std::optional<FileId>& existing_job_id,
                                       TransferOverwritePolicy overwrite_policy) {
    uint64_t total_bytes = 0;
    auto entries = BuildManifest(source_paths, total_bytes);
    if (entries.empty()) {
        return {};
    }

    return StartPreparedSendJob(std::move(entries),
                                total_bytes,
                                peer_destination_root,
                                existing_job_id,
                                overwrite_policy);
}

std::string FileTransfer::StartPreparedSendJob(std::vector<ManifestEntry> entries,
                                               uint64_t total_bytes,
                                               const fs::path& peer_destination_root,
                                               const std::optional<FileId>& existing_job_id,
                                               TransferOverwritePolicy overwrite_policy) {
    if (!peer_ || entries.empty()) {
        return {};
    }

    const FileId job_id = existing_job_id.value_or(GenerateFileId());
    const std::string job_id_hex = FileIdToHex(job_id);

    SendJobState job_state;
    job_state.info.id = job_id;
    job_state.info.id_hex = job_id_hex;
    job_state.info.direction = TransferDirection::Send;
    job_state.info.state = TransferState::InProgress;
    job_state.info.destination_root = NormalizeSlashes(peer_destination_root.string());
    job_state.info.total_bytes = total_bytes;
    job_state.info.total_entries = static_cast<uint32_t>(entries.size());
    job_state.info.overwrite_allowed = overwrite_policy == TransferOverwritePolicy::Allow;
    job_state.destination_root = peer_destination_root;
    job_state.entries = std::move(entries);

    {
        std::lock_guard lock(mutex_);
        send_jobs_[job_id_hex] = job_state;
    }

    if (listener_) {
        listener_->OnTransferJobStarted(job_state.info);
    }

    SendJobPayload(job_state);

    {
        std::lock_guard lock(mutex_);
        if (auto it = send_jobs_.find(job_id_hex); it != send_jobs_.end()) {
            it->second = job_state;
        }
    }

    return job_id_hex;
}

std::string FileTransfer::RequestRemotePathsInternal(const std::vector<fs::path>& remote_paths,
                                                     const fs::path& local_destination_root,
                                                     const std::optional<FileId>& existing_job_id,
                                                     TransferOverwritePolicy overwrite_policy) {
    if (remote_paths.empty()) {
        return {};
    }

    const FileId job_id = existing_job_id.value_or(GenerateFileId());
    const std::string job_id_hex = FileIdToHex(job_id);

    {
        std::lock_guard lock(mutex_);
        pending_receive_roots_[job_id_hex] = local_destination_root;
    }

    json payload;
    payload["type"] = "transfer_job_request";
    payload["job_id"] = job_id_hex;
    payload["overwrite_allowed"] = overwrite_policy == TransferOverwritePolicy::Allow;
    payload["source_paths"] = json::array();
    for (const auto& path : remote_paths) {
        payload["source_paths"].push_back(NormalizeSlashes(path.string()));
    }

    SendControlRaw(payload.dump());
    return job_id_hex;
}

std::vector<FileTransfer::ManifestEntry> FileTransfer::BuildManifest(
    const std::vector<fs::path>& source_paths,
    uint64_t& total_bytes) const {

    std::vector<ManifestEntry> entries;
    total_bytes = 0;

    for (const auto& source : source_paths) {
        std::error_code ec;
        if (!fs::exists(source, ec)) {
            continue;
        }

        const fs::path root_name = source.filename().empty() ? source.root_name() : source.filename();

        if (fs::is_directory(source, ec)) {
            ManifestEntry root_entry;
            root_entry.id = GenerateFileId();
            root_entry.id_hex = FileIdToHex(root_entry.id);
            root_entry.absolute_path = source;
            root_entry.relative_path = RelativePathToWire(root_name);
            root_entry.is_directory = true;
            entries.push_back(std::move(root_entry));

            for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                ManifestEntry entry;
                entry.id = GenerateFileId();
                entry.id_hex = FileIdToHex(entry.id);
                entry.absolute_path = it->path();
                std::error_code rel_ec;
                entry.relative_path = RelativePathToWire(root_name / fs::relative(it->path(), source, rel_ec));
                entry.is_directory = it->is_directory(ec);
                if (!entry.is_directory) {
                    entry.size_bytes = fs::file_size(it->path(), ec);
                    entry.total_chunks = static_cast<uint32_t>((entry.size_bytes + kFileChunkSize - 1) / kFileChunkSize);
                    total_bytes += entry.size_bytes;
                }
                entries.push_back(std::move(entry));
            }
        } else {
            ManifestEntry entry;
            entry.id = GenerateFileId();
            entry.id_hex = FileIdToHex(entry.id);
            entry.absolute_path = source;
            entry.relative_path = RelativePathToWire(root_name);
            entry.is_directory = false;
            entry.size_bytes = fs::file_size(source, ec);
            entry.total_chunks = static_cast<uint32_t>((entry.size_bytes + kFileChunkSize - 1) / kFileChunkSize);
            total_bytes += entry.size_bytes;
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

void FileTransfer::SendJobPayload(SendJobState& job_state) {
    json job_start;
    job_start["type"] = "transfer_job_start";
    job_start["job_id"] = job_state.info.id_hex;
    job_start["destination_root"] = job_state.info.destination_root;
    job_start["total_bytes"] = job_state.info.total_bytes;
    job_start["total_entries"] = job_state.info.total_entries;
    job_start["overwrite_allowed"] = job_state.info.overwrite_allowed;
    SendControlRaw(job_start.dump());

    std::vector<uint8_t> buffer(kFileChunkSize);

    for (const auto& entry : job_state.entries) {
        json entry_start;
        entry_start["type"] = "transfer_entry_start";
        entry_start["job_id"] = job_state.info.id_hex;
        entry_start["entry_id"] = entry.id_hex;
        entry_start["name"] = entry.absolute_path.filename().string();
        entry_start["relative_path"] = entry.relative_path;
        entry_start["is_directory"] = entry.is_directory;
        entry_start["size"] = entry.size_bytes;
        entry_start["chunks"] = entry.total_chunks;
        SendControlRaw(entry_start.dump());

        TransferEntryInfo progress_entry;
        progress_entry.id = entry.id;
        progress_entry.id_hex = entry.id_hex;
        progress_entry.job_id_hex = job_state.info.id_hex;
        progress_entry.name = entry.absolute_path.filename().string();
        progress_entry.relative_path = entry.relative_path;
        progress_entry.is_directory = entry.is_directory;
        progress_entry.file_size = entry.size_bytes;
        progress_entry.total_chunks = entry.total_chunks;
        progress_entry.direction = TransferDirection::Send;
        progress_entry.state = TransferState::InProgress;

        job_state.info.current_item = entry.relative_path;

        if (entry.is_directory || entry.size_bytes == 0) {
            job_state.info.entries_done++;
            if (listener_) {
                progress_entry.state = TransferState::Completed;
                listener_->OnTransferJobProgress(job_state.info, progress_entry);
            }
            continue;
        }

        std::ifstream input(entry.absolute_path, std::ios::binary);
        if (!input.is_open()) {
            job_state.info.state = TransferState::Failed;
            job_state.info.error = "failed to open source file";
            if (listener_) {
                listener_->OnTransferJobFailed(job_state.info, job_state.info.error);
            }
            return;
        }

        for (uint32_t chunk_index = 0; chunk_index < entry.total_chunks; ++chunk_index) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<size_t>(input.gcount());
            SendChunk(entry.id, chunk_index, buffer.data(), bytes_read);

            job_state.info.bytes_done += bytes_read;
            progress_entry.chunks_done = chunk_index + 1;
            progress_entry.bytes_done += bytes_read;

            if (listener_) {
                listener_->OnTransferJobProgress(job_state.info, progress_entry);
            }
        }

        job_state.info.entries_done++;
    }

    job_state.info.state = TransferState::Pending;
    job_state.waiting_result = true;

    json job_complete;
    job_complete["type"] = "transfer_job_complete";
    job_complete["job_id"] = job_state.info.id_hex;
    SendControlRaw(job_complete.dump());
}

void FileTransfer::SendControlJson(const std::string& type,
                                   std::function<void(std::string&)> fill_fields) const {
    std::string payload = "{\"type\":\"" + EscapeJson(type) + "\"";
    if (fill_fields) {
        fill_fields(payload);
    }
    payload += "}";
    SendControlRaw(payload);
}

void FileTransfer::SendControlRaw(const std::string& payload) const {
    if (peer_) {
        peer_->SendDataChannelMessage(kFilesChannel, payload);
    }
}

void FileTransfer::SendChunk(const FileId& id, uint32_t chunk_index,
                             const uint8_t* data, size_t size) const {
    if (!peer_) {
        return;
    }

    std::vector<uint8_t> packet(kChunkHeaderSize + size);
    std::memcpy(packet.data(), id.data(), kFileIdSize);
    WriteU32LE(packet.data() + kFileIdSize, chunk_index);
    std::memcpy(packet.data() + kChunkHeaderSize, data, size);
    peer_->SendDataChannelMessage(kFilesChannel, packet.data(), packet.size(), true);
}
