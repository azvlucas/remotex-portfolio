#pragma once
// RemoteX — core/log_redaction.h
// Helpers para mascarar/sumarizar dados sensíveis antes de escrever em logs.

#include <filesystem>
#include <string>
#include <string_view>

namespace remotex::core {

std::string RedactIdentifier(std::string_view value,
                             std::string_view label = "id");

std::string RedactEmail(std::string_view value);

std::string RedactPath(const std::filesystem::path& path,
                       std::string_view label = "path");

std::string SummarizePayload(std::string_view payload,
                             std::string_view label = "payload");

std::string SanitizeLogMessage(std::string_view message);

} // namespace remotex::core
