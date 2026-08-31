namespace {

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

} // namespace
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
