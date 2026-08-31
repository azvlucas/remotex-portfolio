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

void FileTransfer::HandleTransferJobStart(const std::string& raw_json) {
    TransferJobInfo job_info;
    fs::path destination_root;

    try {
        const json payload = json::parse(raw_json);
        job_info.id = HexToFileId(payload.value("job_id", ""));
        job_info.id_hex = payload.value("job_id", "");
        job_info.direction = TransferDirection::Receive;
        job_info.state = TransferState::InProgress;
        job_info.total_bytes = payload.value("total_bytes", uint64_t{0});
        job_info.total_entries = payload.value("total_entries", uint32_t{0});
        job_info.overwrite_allowed = payload.value("overwrite_allowed", false);

        const auto explicit_destination = payload.value("destination_root", "");

        std::lock_guard lock(mutex_);
        if (auto it = pending_receive_roots_.find(job_info.id_hex); it != pending_receive_roots_.end()) {
            destination_root = it->second;
            pending_receive_roots_.erase(it);
        } else if (!explicit_destination.empty()) {
            destination_root = fs::path(explicit_destination);
        } else {
            destination_root = default_receive_dir_;
        }

        std::error_code ec;
        fs::create_directories(destination_root, ec);

        job_info.destination_root = NormalizeSlashes(destination_root.string());
        ReceiveJobState state;
        state.info = job_info;
        state.destination_root = destination_root;
        receive_jobs_[job_info.id_hex] = std::move(state);
    } catch (...) {
        return;
    }

    if (listener_) {
        listener_->OnTransferJobStarted(job_info);
    }
}

void FileTransfer::HandleTransferEntryStart(const std::string& raw_json) {
    TransferJobInfo job_info;
    TransferEntryInfo entry_info;
    bool notify_progress = false;
    bool should_finalize = false;
    std::string finalize_entry_id;
    std::string fail_job_id;
    std::string fail_error;

    try {
        const json payload = json::parse(raw_json);
        const std::string entry_id_hex = payload.value("entry_id", "");
        const std::string job_id_hex = payload.value("job_id", "");

        std::lock_guard lock(mutex_);
        auto job_it = receive_jobs_.find(job_id_hex);
        if (job_it == receive_jobs_.end()) {
            return;
        }

        auto& job = job_it->second;
        fs::path relative = WireToRelativePath(payload.value("relative_path", ""));
        if (!IsSafeRelativePath(relative)) {
            fail_job_id = job_id_hex;
            fail_error = "conflict-declined: unsafe relative path";
            job.entry_errors[entry_id_hex] = fail_error;
            job.info.error = fail_error;
        } else {
            entry_info.id = HexToFileId(entry_id_hex);
            entry_info.id_hex = entry_id_hex;
            entry_info.job_id_hex = job_id_hex;
            entry_info.name = payload.value("name", relative.filename().string());
            entry_info.relative_path = payload.value("relative_path", "");
            entry_info.is_directory = payload.value("is_directory", false);
            entry_info.file_size = payload.value("size", uint64_t{0});
            entry_info.total_chunks = payload.value("chunks", uint32_t{0});
            entry_info.direction = TransferDirection::Receive;
            entry_info.state = TransferState::InProgress;

            const fs::path final_path = job.destination_root / relative;
            std::error_code ec;
            fs::create_directories(final_path.parent_path(), ec);
            if (ec) {
                fail_job_id = job_id_hex;
                fail_error = "failed to create destination parent directory";
                job.entry_errors[entry_id_hex] = fail_error;
                job.info.error = fail_error;
            } else {
                TransferConflictInfo conflict;
                if (TryGetConflict(final_path,
                                   entry_info.relative_path,
                                   entry_info.is_directory,
                                   conflict) &&
                    !job.info.overwrite_allowed) {
                    fail_job_id = job_id_hex;
                    fail_error = BuildConflictError(conflict, false);
                    job.entry_errors[entry_id_hex] = fail_error;
                    job.info.error = fail_error;
                } else if (entry_info.is_directory) {
                    if (conflict.type_mismatch && job.info.overwrite_allowed &&
                        !RemoveExistingDestination(final_path, conflict.existing_is_directory)) {
                        fail_job_id = job_id_hex;
                        fail_error = "late-conflict: failed to replace existing file for directory";
                        job.entry_errors[entry_id_hex] = fail_error;
                        job.info.error = fail_error;
                    } else {
                        fs::create_directories(final_path, ec);
                        if (ec) {
                            fail_job_id = job_id_hex;
                            fail_error = "failed to create directory";
                            job.entry_errors[entry_id_hex] = fail_error;
                            job.info.error = fail_error;
                        } else {
                            entry_info.state = TransferState::Completed;
                            job.info.entries_done++;
                            job.entries[entry_id_hex] = entry_info;
                            job.completed_paths[entry_id_hex] = final_path;
                            job.info.current_item = entry_info.relative_path;
                            job_info = job.info;
                            notify_progress = true;
                        }
                    }
                } else {
                    const fs::path temp_path = final_path.string() + ".remotex.part";
                    auto stream = std::make_unique<std::ofstream>(temp_path,
                                                                  std::ios::binary | std::ios::trunc);
                    if (!stream->is_open()) {
                        fail_job_id = job_id_hex;
                        fail_error = "failed to create temp file";
                        job.entry_errors[entry_id_hex] = fail_error;
                        job.info.error = fail_error;
                    } else {
                        ActiveReceiveEntry active;
                        active.info = entry_info;
                        active.temp_path = temp_path;
                        active.final_path = final_path;
                        active.file = std::move(stream);
                        if (entry_info.file_size == 0 && active.file && active.file->is_open()) {
                            active.file->close();
                        }
                        active_entries_[entry_id_hex] = std::move(active);
                        job.entries[entry_id_hex] = entry_info;
                        if (entry_info.file_size == 0) {
                            should_finalize = true;
                            finalize_entry_id = entry_id_hex;
                        }
                    }
                }
            }
        }
    } catch (...) {
        return;
    }

    if (!fail_error.empty()) {
        FailReceiveJob(fail_job_id, fail_error);
        return;
    }

    if (notify_progress && listener_) {
        listener_->OnTransferJobProgress(job_info, entry_info);
    }

    if (should_finalize) {
        FinalizeReceiveEntry(finalize_entry_id);
    }
}

void FileTransfer::HandleTransferJobComplete(const std::string& raw_json) {
    std::string job_id_hex;
    {
        std::lock_guard lock(mutex_);
        try {
            const json payload = json::parse(raw_json);
            job_id_hex = payload.value("job_id", "");
            if (auto it = receive_jobs_.find(job_id_hex); it != receive_jobs_.end()) {
                it->second.sender_announced_complete = true;
            }
        } catch (...) {
            return;
        }
    }

    CompleteReceiveJobIfReady(job_id_hex);
}

void FileTransfer::HandleTransferJobResult(const std::string& raw_json) {
    TransferJobInfo info;
    bool success = false;
    bool notify = false;
    std::string error;

    {
        std::lock_guard lock(mutex_);
        try {
            const json payload = json::parse(raw_json);
            const std::string job_id_hex = payload.value("job_id", "");
            auto it = send_jobs_.find(job_id_hex);
            if (it == send_jobs_.end()) {
                return;
            }

            success = payload.value("success", false);
            error = payload.value("error", "");
            it->second.info.state = success ? TransferState::Completed : TransferState::Failed;
            it->second.info.error = error;
            it->second.info.bytes_done = it->second.info.total_bytes;
            it->second.info.entries_done = it->second.info.total_entries;
            info = it->second.info;
            notify = true;
        } catch (...) {
            return;
        }
    }

    if (!notify || !listener_) {
        return;
    }

    if (success) {
        listener_->OnTransferJobCompleted(info, fs::path(info.destination_root));
    } else {
        listener_->OnTransferJobFailed(info, error);
    }
}

void FileTransfer::HandleTransferCancel(const std::string& raw_json) {
    TransferJobInfo info;
    bool notify = false;

    {
        std::lock_guard lock(mutex_);
        try {
            const json payload = json::parse(raw_json);
            const std::string job_id_hex = payload.value("job_id", "");
            const std::string reason = payload.value("reason", "cancelled");

            if (auto it = send_jobs_.find(job_id_hex); it != send_jobs_.end()) {
                it->second.info.state = TransferState::Cancelled;
                it->second.info.error = reason;
                info = it->second.info;
                notify = true;
            }

            if (auto it = receive_jobs_.find(job_id_hex); it != receive_jobs_.end()) {
                it->second.info.state = TransferState::Cancelled;
                it->second.info.error = reason;
                info = it->second.info;
                notify = true;
            }
        } catch (...) {
            return;
        }
    }

    if (notify && listener_) {
        listener_->OnTransferJobCancelled(info);
    }
}

void FileTransfer::FinalizeReceiveEntry(const std::string& entry_id_hex) {
    TransferJobInfo job_info;
    TransferEntryInfo entry_info;
    bool notify_progress = false;
    std::string job_id_hex;
    std::string fail_error;

    {
        std::lock_guard lock(mutex_);
        auto active_it = active_entries_.find(entry_id_hex);
        if (active_it == active_entries_.end()) {
            return;
        }

        auto& active = active_it->second;
        job_id_hex = active.info.job_id_hex;

        if (active.file && active.file->is_open()) {
            active.file->close();
        }

        auto job_it = receive_jobs_.find(job_id_hex);
        if (job_it == receive_jobs_.end()) {
            std::error_code cleanup_ec;
            fs::remove(active.temp_path, cleanup_ec);
            active_entries_.erase(active_it);
            return;
        }

        auto& job = job_it->second;
        TransferConflictInfo conflict;
        if (TryGetConflict(active.final_path,
                           active.info.relative_path,
                           false,
                           conflict)) {
            if (!job.info.overwrite_allowed) {
                fail_error = BuildConflictError(conflict, true);
            } else if (!RemoveExistingDestination(active.final_path, conflict.existing_is_directory)) {
                fail_error = "late-conflict: failed to replace existing destination";
            }
        }

        if (!fail_error.empty()) {
            std::error_code cleanup_ec;
            fs::remove(active.temp_path, cleanup_ec);
            active_entries_.erase(active_it);
            job.entry_errors[entry_id_hex] = fail_error;
            job.info.error = fail_error;
        } else {
            std::error_code ec;
            fs::rename(active.temp_path, active.final_path, ec);
            if (ec) {
                fs::copy_file(active.temp_path,
                              active.final_path,
                              fs::copy_options::overwrite_existing,
                              ec);
                if (!ec) {
                    fs::remove(active.temp_path, ec);
                }
            }

            if (ec) {
                fail_error = "failed to finalize file";
                std::error_code cleanup_ec;
                fs::remove(active.temp_path, cleanup_ec);
                active_entries_.erase(active_it);
                job.entry_errors[entry_id_hex] = fail_error;
                job.info.error = fail_error;
            } else {
                active.info.state = TransferState::Completed;
                job.entries[entry_id_hex] = active.info;
                job.completed_paths[entry_id_hex] = active.final_path;
                job.info.entries_done++;
                job.info.current_item = active.info.relative_path;

                job_info = job.info;
                entry_info = active.info;
                notify_progress = true;
                active_entries_.erase(active_it);
            }
        }
    }

    if (!fail_error.empty()) {
        FailReceiveJob(job_id_hex, fail_error);
        return;
    }

    if (notify_progress && listener_) {
        listener_->OnTransferJobProgress(job_info, entry_info);
    }

    CompleteReceiveJobIfReady(job_id_hex);
}

void FileTransfer::CompleteReceiveJobIfReady(const std::string& job_id_hex) {
    TransferJobInfo completed_info;
    fs::path destination_root;
    bool success = false;
    bool notify = false;
    std::string error;

    {
        std::lock_guard lock(mutex_);
        auto it = receive_jobs_.find(job_id_hex);
        if (it == receive_jobs_.end()) {
            return;
        }

        auto& job = it->second;
        if (!job.sender_announced_complete) {
            return;
        }

        for (const auto& [entry_id, entry] : active_entries_) {
            if (entry.info.job_id_hex == job_id_hex) {
                return;
            }
        }

        const bool done = job.info.entries_done >= job.info.total_entries;
        if (!done) {
            return;
        }

        success = job.info.error.empty() && job.entry_errors.empty();
        job.info.state = success ? TransferState::Completed : TransferState::Failed;
        completed_info = job.info;
        destination_root = job.destination_root;
        error = job.info.error.empty() && !job.entry_errors.empty()
            ? job.entry_errors.begin()->second
            : job.info.error;
        notify = true;
    }

    json result;
    result["type"] = "transfer_job_result";
    result["job_id"] = job_id_hex;
    result["success"] = success;
    result["error"] = error;
    SendControlRaw(result.dump());

    if (!notify || !listener_) {
        return;
    }

    if (success) {
        listener_->OnTransferJobCompleted(completed_info, destination_root);
    } else {
        listener_->OnTransferJobFailed(completed_info, error);
    }
}

void FileTransfer::FailReceiveJob(const std::string& job_id_hex, const std::string& error) {
    TransferJobInfo info;
    bool notify = false;

    {
        std::lock_guard lock(mutex_);
        auto it = receive_jobs_.find(job_id_hex);
        if (it == receive_jobs_.end()) {
            return;
        }

        it->second.info.state = TransferState::Failed;
        it->second.info.error = error;
        info = it->second.info;
        notify = true;

        for (auto active_it = active_entries_.begin(); active_it != active_entries_.end();) {
            if (active_it->second.info.job_id_hex == job_id_hex) {
                if (active_it->second.file && active_it->second.file->is_open()) {
                    active_it->second.file->close();
                }
                std::error_code cleanup_ec;
                fs::remove(active_it->second.temp_path, cleanup_ec);
                active_it = active_entries_.erase(active_it);
            } else {
                ++active_it;
            }
        }
    }

    json result;
    result["type"] = "transfer_job_result";
    result["job_id"] = job_id_hex;
    result["success"] = false;
    result["error"] = error;
    SendControlRaw(result.dump());

    if (notify && listener_) {
        listener_->OnTransferJobFailed(info, error);
    }
}
