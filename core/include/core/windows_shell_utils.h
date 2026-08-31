#pragma once

#include <filesystem>
#include <optional>

namespace remotex::core {

struct ClipboardPasteTarget {
    std::filesystem::path path;
    bool resolved_from_explorer = false;
    bool used_desktop_fallback = false;
};

namespace detail {

std::filesystem::path RemapSystemProfilePathToUserProfile(
    const std::filesystem::path& resolved_path,
    const std::filesystem::path& system_profile_path,
    const std::filesystem::path& user_profile_path);

} // namespace detail

std::optional<std::filesystem::path> DetectActiveExplorerDirectory();
std::optional<std::filesystem::path> GetDesktopDirectory();
ClipboardPasteTarget ResolveExplorerDirectoryOrDesktop();

} // namespace remotex::core
