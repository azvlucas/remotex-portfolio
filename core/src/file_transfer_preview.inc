namespace {

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
