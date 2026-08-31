// RemoteX — core/log_redaction.cpp
// Sanitização best-effort de logs para Host/Viewer/Updater.

#include "core/log_redaction.h"

#include "core/crypto_utils.h"

#include <cctype>
#include <functional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace remotex::core {
namespace {

constexpr size_t kHashChars = 12;

std::string ToUtf8String(const fs::path& path) {
#if defined(__cpp_char8_t)
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
}

std::string StableHash(std::string_view value) {
    if (value.empty()) {
        return "empty";
    }

    const auto bytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(value.data()),
        value.size());
    std::string hex = SHA256Hex(bytes);
    if (hex.size() > kHashChars) {
        hex.resize(kHashChars);
    }
    return hex;
}

bool IsAlreadySanitized(std::string_view value) {
    return value.find("sha256=") != std::string_view::npos &&
           value.find('[') != std::string_view::npos &&
           value.find(']') != std::string_view::npos;
}

bool IsValueTerminator(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) ||
           ch == ',' ||
           ch == ')' ||
           ch == ']' ||
           ch == '}' ||
           ch == ';';
}

size_t PathDepth(const fs::path& path) {
    size_t depth = 0;
    for (const auto& part : path) {
        const std::string component = ToUtf8String(part);
        if (component.empty() || component == "/" || component == "\\" || component == ".") {
            continue;
        }
        ++depth;
    }
    return depth;
}

std::string ReplaceEmails(const std::string& input) {
    static const std::regex kEmailPattern(
        R"(([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}))");

    std::string out;
    out.reserve(input.size());

    std::sregex_iterator it(input.begin(), input.end(), kEmailPattern);
    std::sregex_iterator end;
    size_t last = 0;
    for (; it != end; ++it) {
        const auto& match = *it;
        out.append(input, last, static_cast<size_t>(match.position()) - last);
        out += RedactEmail(match.str());
        last = static_cast<size_t>(match.position() + match.length());
    }
    out.append(input, last, std::string::npos);
    return out;
}

std::string ReplaceKeyedValue(const std::string& input,
                              std::string_view key,
                              const std::function<std::string(std::string_view)>& redactor) {
    std::string out;
    out.reserve(input.size());

    size_t cursor = 0;
    while (cursor < input.size()) {
        const size_t pos = input.find(key, cursor);
        if (pos == std::string::npos) {
            out.append(input, cursor, std::string::npos);
            break;
        }

        out.append(input, cursor, pos - cursor);
        out.append(key);

        size_t value_start = pos + key.size();
        if (value_start >= input.size()) {
            cursor = value_start;
            continue;
        }

        const char first = input[value_start];
        if (first == '\'' || first == '"') {
            const char quote = first;
            size_t value_end = value_start + 1;
            while (value_end < input.size()) {
                if (input[value_end] == quote && input[value_end - 1] != '\\') {
                    break;
                }
                ++value_end;
            }

            if (value_end >= input.size()) {
                out.append(input, value_start, std::string::npos);
                break;
            }

            std::string_view value(input.data() + value_start + 1,
                                   value_end - value_start - 1);
            out.push_back(quote);
            out += IsAlreadySanitized(value) ? std::string(value) : redactor(value);
            out.push_back(quote);
            cursor = value_end + 1;
            continue;
        }

        size_t value_end = value_start;
        while (value_end < input.size() && !IsValueTerminator(input[value_end])) {
            ++value_end;
        }

        std::string_view value(input.data() + value_start, value_end - value_start);
        out += IsAlreadySanitized(value) ? std::string(value) : redactor(value);
        cursor = value_end;
    }

    return out;
}

} // namespace

std::string RedactIdentifier(std::string_view value, std::string_view label) {
    if (value.empty()) {
        return std::string(label) + "[empty]";
    }
    return std::string(label) + "[len=" + std::to_string(value.size()) +
           ",sha256=" + StableHash(value) + "]";
}

std::string RedactEmail(std::string_view value) {
    return RedactIdentifier(value, "email");
}

std::string RedactPath(const fs::path& path, std::string_view label) {
    const std::string utf8_path = ToUtf8String(path);
    if (utf8_path.empty()) {
        return std::string(label) + "[empty]";
    }

    std::string ext;
    if (path.has_extension()) {
        ext = ToUtf8String(path.extension());
    }
    if (ext.empty()) {
        ext = "<none>";
    }

    return std::string(label) + "[depth=" + std::to_string(PathDepth(path)) +
           ",ext=" + ext + ",sha256=" + StableHash(utf8_path) + "]";
}

std::string SummarizePayload(std::string_view payload, std::string_view label) {
    if (payload.empty()) {
        return std::string(label) + "[empty]";
    }
    return std::string(label) + "[bytes=" + std::to_string(payload.size()) +
           ",sha256=" + StableHash(payload) + "]";
}

std::string SanitizeLogMessage(std::string_view message) {
    std::string sanitized(message);
    if (sanitized.empty()) {
        return sanitized;
    }

    sanitized = ReplaceEmails(sanitized);

    const std::vector<std::string_view> identifier_keys = {
        "email=",
        "machine=",
        "machine_id=",
        "user=",
        "user_id=",
        "session_id=",
        "backend_session=",
        "relay_session=",
        "machine_session=",
        "requester=",
        "token=",
        "job=",
    };
    for (const auto key : identifier_keys) {
        sanitized = ReplaceKeyedValue(sanitized, key, [key](std::string_view value) {
            std::string label(key);
            if (!label.empty() && label.back() == '=') {
                label.pop_back();
            }
            return RedactIdentifier(value, label);
        });
    }

    const std::vector<std::string_view> path_keys = {
        "path=",
        "destination=",
        "destination_root=",
        "file=",
        "log file=",
    };
    for (const auto key : path_keys) {
        sanitized = ReplaceKeyedValue(sanitized, key, [key](std::string_view value) {
            std::string label(key);
            if (!label.empty() && label.back() == '=') {
                label.pop_back();
            }
            return RedactPath(fs::path(std::string(value)), label);
        });
    }

    const std::vector<std::string_view> payload_keys = {
        "payload=",
        "json=",
        "body=",
    };
    for (const auto key : payload_keys) {
        sanitized = ReplaceKeyedValue(sanitized, key, [key](std::string_view value) {
            std::string label(key);
            if (!label.empty() && label.back() == '=') {
                label.pop_back();
            }
            return SummarizePayload(value, label);
        });
    }

    return sanitized;
}

} // namespace remotex::core
