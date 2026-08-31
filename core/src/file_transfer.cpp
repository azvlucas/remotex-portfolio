// RemoteX - core/file_transfer.cpp

#include "core/file_transfer.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

namespace remotex::core {

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
#include "file_transfer_listing.inc"
#include "file_transfer_preview.inc"
#include "file_transfer_dispatch.inc"
#include "file_transfer_receive.inc"
#include "file_transfer_send.inc"

} // namespace remotex::core
