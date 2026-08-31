#pragma once

#include "core/machine_client.h"

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

namespace remotex::viewer {

struct ConsolidatedAuditEntry {
    enum class Category { Session, FileTransfer };
    Category category = Category::Session;
    std::string session_id;
    std::string user_name;
    std::string started_at;
    std::string ended_at;
    int duration_sec = 0;
    bool has_duration = false;
    std::string status;           // started, completed, failed, cancelled
    std::string summary;          // "Interativo", "Apenas assistir", ou detalhes de arquivo
    std::string transfer_direction;
    std::string transfer_item;
    uint64_t transfer_total_bytes = 0;
    uint32_t transfer_total_entries = 0;
    std::string error_message;
};

std::vector<ConsolidatedAuditEntry> ConsolidateAuditEvents(
    const std::vector<remotex::core::MachineAuditEvent>& events);

QDateTime ParseIsoTimestampUtc(const std::string& iso_timestamp);
QString FormatLastSeenText(const std::string& iso_timestamp,
                           const QDateTime& now_utc = QDateTime::currentDateTimeUtc());
QString FormatMachineAgeText(const std::string& iso_timestamp,
                             const QDateTime& now_utc = QDateTime::currentDateTimeUtc());
bool IsMachineBlocked(const remotex::core::MachineInfo& machine);
QString MachineBlockedReasonText(const remotex::core::MachineInfo& machine);
QString MachineStatusText(const remotex::core::MachineInfo& machine);
QString MachineStatusTooltipText(const remotex::core::MachineInfo& machine);
QString MachineSessionAccessDeniedMessage(const remotex::core::MachineInfo& machine,
                                          bool current_user_is_manager);
bool CanStartMachineSession(const remotex::core::MachineInfo& machine,
                            bool current_user_is_manager);
bool IsMachineNew(const remotex::core::MachineInfo& machine,
                  const QDateTime& now_utc = QDateTime::currentDateTimeUtc());
bool CompareMachinesForDisplay(const remotex::core::MachineInfo& lhs,
                               const remotex::core::MachineInfo& rhs,
                               const QDateTime& now_utc = QDateTime::currentDateTimeUtc());

} // namespace remotex::viewer
