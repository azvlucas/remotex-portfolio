#include "viewer/machine_display_utils.h"

#include <algorithm>
#include <unordered_map>

namespace remotex::viewer {

namespace {

constexpr qint64 kNewMachineWindowSeconds = 24 * 60 * 60;
constexpr char kMachineStateBlocked[] = "blocked";
constexpr char kMachineBlockReasonMachineLimitExceeded[] = "machine_limit_exceeded";

QString NormalizeIsoTimestamp(QString value) {
    value = value.trimmed();
    const int t_pos = value.indexOf('T');
    const int dot_pos = value.indexOf('.', t_pos);
    if (dot_pos < 0) {
        return value;
    }

    int tz_pos = value.indexOf('Z', dot_pos);
    const int plus_pos = value.indexOf('+', dot_pos);
    const int minus_pos = value.indexOf('-', dot_pos + 1);
    if (tz_pos < 0 || (plus_pos >= 0 && plus_pos < tz_pos)) tz_pos = plus_pos;
    if (tz_pos < 0 || (minus_pos >= 0 && minus_pos < tz_pos)) tz_pos = minus_pos;
    if (tz_pos < 0) {
        return value;
    }

    QString fractional = value.mid(dot_pos + 1, tz_pos - dot_pos - 1);
    if (fractional.size() > 3) {
        fractional = fractional.left(3);
    }

    return value.left(dot_pos + 1) + fractional + value.mid(tz_pos);
}

QDateTime ParseHeartbeatTimestamp(const std::string& iso_timestamp) {
    QString text = QString::fromStdString(iso_timestamp).trimmed();
    if (text.isEmpty()) {
        return {};
    }

    QDateTime dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(text, Qt::ISODate);
    }
    if (!dt.isValid()) {
        dt = QDateTime::fromString(NormalizeIsoTimestamp(text), Qt::ISODateWithMs);
    }
    if (!dt.isValid()) {
        dt = QDateTime::fromString(NormalizeIsoTimestamp(text), Qt::ISODate);
    }
    return dt.isValid() ? dt.toUTC() : QDateTime{};
}

QDateTime ResolveReferenceTime(const QDateTime& now_utc) {
    return now_utc.isValid() ? now_utc.toUTC() : QDateTime::currentDateTimeUtc();
}

QString ConnectivityLabel(const remotex::core::MachineInfo& machine) {
    return machine.is_online ? "Online" : "Offline";
}

QString ConnectivityStatusText(const remotex::core::MachineInfo& machine) {
    return machine.is_online
        ? QString::fromUtf8("\xE2\x97\x8F Online")
        : QString::fromUtf8("\xE2\x97\x8F Offline");
}

bool HasKnownBlockedState(const remotex::core::MachineInfo& machine) {
    return machine.state == kMachineStateBlocked
        && machine.block_reason == kMachineBlockReasonMachineLimitExceeded;
}

QString FormatElapsedText(const QDateTime& timestamp,
                          const QDateTime& now_utc,
                          const QString& invalid_text) {
    if (!timestamp.isValid()) {
        return invalid_text;
    }

    const QDateTime reference = ResolveReferenceTime(now_utc);
    qint64 seconds = timestamp.secsTo(reference);
    if (seconds < 0) {
        seconds = 0;
    }

    const qint64 days = seconds / (24 * 60 * 60);
    if (days >= 1) {
        return days == 1 ? "1 dia" : QString("%1 dias").arg(days);
    }

    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    return QString("%1h%2m")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'));
}

bool IsSessionStartEvent(const std::string& event_type) {
    return event_type == "session_started";
}

bool IsSessionEndEvent(const std::string& event_type) {
    return event_type == "session_ended";
}

bool IsFileStartEvent(const std::string& event_type) {
    return event_type == "file_upload_started" || event_type == "file_download_started";
}

bool IsFileEndEvent(const std::string& event_type) {
    return event_type == "file_upload_finished" || event_type == "file_download_finished";
}

bool IsFileFailEvent(const std::string& event_type) {
    return event_type == "file_transfer_failed";
}

std::string MakeConsolidationKey(const remotex::core::MachineAuditEvent& event) {
    if (IsSessionStartEvent(event.event_type) || IsSessionEndEvent(event.event_type)) {
        return event.session_id + ":session";
    }
    return event.session_id + ":file:" + event.transfer_direction + ":" + event.transfer_item;
}

std::string TransferDirectionLabel(const std::string& direction) {
    if (direction == "upload") return "Envio";
    if (direction == "download") return "Recebimento";
    return direction;
}

} // namespace

std::vector<ConsolidatedAuditEntry> ConsolidateAuditEvents(
    const std::vector<remotex::core::MachineAuditEvent>& events) {

    std::unordered_map<std::string, size_t> key_to_index;
    std::vector<ConsolidatedAuditEntry> result;

    for (const auto& event : events) {
        const std::string key = MakeConsolidationKey(event);
        const bool is_session = IsSessionStartEvent(event.event_type) || IsSessionEndEvent(event.event_type);
        const bool is_start = IsSessionStartEvent(event.event_type) || IsFileStartEvent(event.event_type);
        const bool is_end = IsSessionEndEvent(event.event_type) || IsFileEndEvent(event.event_type);
        const bool is_fail = IsFileFailEvent(event.event_type);

        auto it = key_to_index.find(key);
        if (it != key_to_index.end()) {
            auto& entry = result[it->second];

            if (is_end || is_fail) {
                entry.ended_at = event.created_at;
                if (event.has_duration) {
                    entry.duration_sec = event.duration_sec;
                    entry.has_duration = true;
                }
                entry.status = is_fail ? "failed" : "completed";
                if (is_fail && !event.error_message.empty()) {
                    entry.error_message = event.error_message;
                }
                if (!is_session && event.transfer_total_bytes > 0) {
                    entry.transfer_total_bytes = event.transfer_total_bytes;
                }
                if (!is_session && event.transfer_total_entries > 0) {
                    entry.transfer_total_entries = event.transfer_total_entries;
                }
            } else if (is_start) {
                entry.started_at = event.created_at;
                if (entry.user_name.empty() && !event.user_name.empty()) {
                    entry.user_name = event.user_name;
                }
                if (!event.summary.empty()) {
                    entry.summary = event.summary;
                }
            }
            continue;
        }

        ConsolidatedAuditEntry entry;
        entry.category = is_session ? ConsolidatedAuditEntry::Category::Session
                                     : ConsolidatedAuditEntry::Category::FileTransfer;
        entry.session_id = event.session_id;
        entry.user_name = event.user_name;

        if (is_start) {
            entry.started_at = event.created_at;
            entry.status = "started";
            entry.summary = event.summary;
        } else if (is_end) {
            entry.ended_at = event.created_at;
            entry.status = "completed";
            if (event.has_duration) {
                entry.duration_sec = event.duration_sec;
                entry.has_duration = true;
            }
        } else if (is_fail) {
            entry.ended_at = event.created_at;
            entry.status = "failed";
            entry.error_message = event.error_message;
        }

        if (!is_session) {
            entry.transfer_direction = event.transfer_direction;
            entry.transfer_item = event.transfer_item;
            entry.transfer_total_bytes = event.transfer_total_bytes;
            entry.transfer_total_entries = event.transfer_total_entries;
            if (entry.summary.empty()) {
                entry.summary = TransferDirectionLabel(event.transfer_direction);
            }
        }

        key_to_index[key] = result.size();
        result.push_back(std::move(entry));
    }

    std::sort(result.begin(), result.end(), [](const ConsolidatedAuditEntry& a, const ConsolidatedAuditEntry& b) {
        const std::string& a_time = !a.started_at.empty() ? a.started_at : a.ended_at;
        const std::string& b_time = !b.started_at.empty() ? b.started_at : b.ended_at;
        return a_time > b_time;
    });

    return result;
}

QDateTime ParseIsoTimestampUtc(const std::string& iso_timestamp) {
    return ParseHeartbeatTimestamp(iso_timestamp);
}

QString FormatLastSeenText(const std::string& iso_timestamp, const QDateTime& now_utc) {
    return FormatElapsedText(ParseIsoTimestampUtc(iso_timestamp), now_utc, "Nunca");
}

QString FormatMachineAgeText(const std::string& iso_timestamp, const QDateTime& now_utc) {
    return FormatElapsedText(ParseIsoTimestampUtc(iso_timestamp), now_utc, QString());
}

bool IsMachineBlocked(const remotex::core::MachineInfo& machine) {
    return HasKnownBlockedState(machine);
}

QString MachineBlockedReasonText(const remotex::core::MachineInfo& machine) {
    if (!HasKnownBlockedState(machine)) {
        return {};
    }
    return "Limite de maquinas excedido";
}

QString MachineStatusText(const remotex::core::MachineInfo& machine) {
    if (IsMachineBlocked(machine)) {
        return "Bloqueada";
    }
    return ConnectivityStatusText(machine);
}

QString MachineStatusTooltipText(const remotex::core::MachineInfo& machine) {
    const QString block_reason = MachineBlockedReasonText(machine);
    if (block_reason.isEmpty()) {
        return {};
    }

    return QString("Motivo: %1\nConectividade: %2")
        .arg(block_reason, ConnectivityLabel(machine));
}

QString MachineSessionAccessDeniedMessage(const remotex::core::MachineInfo& machine,
                                          bool current_user_is_manager) {
    if (const QString block_reason = MachineBlockedReasonText(machine); !block_reason.isEmpty()) {
        return QString("Esta maquina esta bloqueada: %1.").arg(block_reason);
    }
    if (!machine.is_online) {
        return "A maquina esta offline.";
    }
    if (machine.managers_only_access && !current_user_is_manager) {
        return "Esta maquina permite acesso apenas para gerentes.";
    }
    return {};
}

bool CanStartMachineSession(const remotex::core::MachineInfo& machine,
                            bool current_user_is_manager) {
    return MachineSessionAccessDeniedMessage(machine, current_user_is_manager).isEmpty();
}

bool IsMachineNew(const remotex::core::MachineInfo& machine, const QDateTime& now_utc) {
    const QDateTime created_at = ParseIsoTimestampUtc(machine.created_at);
    if (!created_at.isValid()) {
        return false;
    }

    const qint64 age_seconds = created_at.secsTo(ResolveReferenceTime(now_utc));
    return age_seconds >= 0 && age_seconds < kNewMachineWindowSeconds;
}

bool CompareMachinesForDisplay(const remotex::core::MachineInfo& lhs,
                               const remotex::core::MachineInfo& rhs,
                               const QDateTime& now_utc) {
    const bool lhs_is_new = IsMachineNew(lhs, now_utc);
    const bool rhs_is_new = IsMachineNew(rhs, now_utc);
    if (lhs_is_new != rhs_is_new) {
        return lhs_is_new;
    }

    if (lhs_is_new && rhs_is_new) {
        const QDateTime lhs_created_at = ParseIsoTimestampUtc(lhs.created_at);
        const QDateTime rhs_created_at = ParseIsoTimestampUtc(rhs.created_at);
        if (lhs_created_at != rhs_created_at) {
            return lhs_created_at > rhs_created_at;
        }
    }

    const QString lhs_name = QString::fromStdString(lhs.name).trimmed();
    const QString rhs_name = QString::fromStdString(rhs.name).trimmed();
    const int name_compare = QString::compare(lhs_name, rhs_name, Qt::CaseInsensitive);
    if (name_compare != 0) {
        return name_compare < 0;
    }

    return lhs.id < rhs.id;
}

} // namespace remotex::viewer
