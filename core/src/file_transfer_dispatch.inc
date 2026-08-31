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
