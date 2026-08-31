#pragma once

#include <filesystem>
#include <string>

namespace remotex::host {

struct EnsuredDirectory {
    std::filesystem::path path;
    std::string error;

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

inline EnsuredDirectory EnsureDirectoryExists(const std::filesystem::path& path,
                                              const char* context) {
    EnsuredDirectory result;
    if (path.empty()) {
        result.error = std::string(context) + ": path is empty";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        result.error = std::string(context) + ": " + ec.message();
        return result;
    }

    result.path = path;
    return result;
}

} // namespace remotex::host
