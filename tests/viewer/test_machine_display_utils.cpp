#include <gtest/gtest.h>

#include <algorithm>
#include <QDateTime>

#include "viewer/machine_display_utils.h"

namespace {

QString OnlineStatusText() {
    return QString::fromUtf8("\xE2\x97\x8F Online");
}

QString OfflineStatusText() {
    return QString::fromUtf8("\xE2\x97\x8F Offline");
}

remotex::core::MachineInfo BuildMachine(std::string id,
                                        std::string name,
                                        std::string created_at,
                                        bool is_online = false,
                                        bool managers_only_access = false,
                                        std::string state = {},
                                        std::string block_reason = {}) {
    remotex::core::MachineInfo machine;
    machine.id = std::move(id);
    machine.name = std::move(name);
    machine.created_at = std::move(created_at);
    machine.is_online = is_online;
    machine.managers_only_access = managers_only_access;
    machine.state = std::move(state);
    machine.block_reason = std::move(block_reason);
    return machine;
}

TEST(MachineDisplayUtils, ReturnsNuncaWhenMissingTimestamp) {
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("", QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate)),
              QString("Nunca"));
}

TEST(MachineDisplayUtils, FormatsMinutesAndHoursBelowOneDay) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:34:00Z", Qt::ISODate);
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("2026-03-14T12:29:00Z", now), QString("00h05m"));
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("2026-03-14T01:04:00Z", now), QString("11h30m"));
}

TEST(MachineDisplayUtils, FormatsDaysAtOrAboveTwentyFourHours) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("2026-03-13T12:00:00Z", now), QString("1 dia"));
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("2026-03-11T11:59:59Z", now), QString("3 dias"));
}

TEST(MachineDisplayUtils, SupportsNanosecondTimestampsFromGo) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);
    EXPECT_EQ(remotex::viewer::FormatLastSeenText("2026-03-14T11:58:00.123456789Z", now), QString("00h01m"));
}

TEST(MachineDisplayUtils, ReturnsBlockedStatusReasonTooltipAndDeniedMessageForBlockedMachine) {
    const auto machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        true,
        false,
        "blocked",
        "machine_limit_exceeded");

    EXPECT_TRUE(remotex::viewer::IsMachineBlocked(machine));
    EXPECT_EQ(remotex::viewer::MachineBlockedReasonText(machine), QString("Limite de maquinas excedido"));
    EXPECT_EQ(remotex::viewer::MachineStatusText(machine), QString("Bloqueada"));
    EXPECT_EQ(remotex::viewer::MachineStatusTooltipText(machine),
              QString("Motivo: Limite de maquinas excedido\nConectividade: Online"));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(machine, false));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(machine, true));
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(machine, false),
              QString("Esta maquina esta bloqueada: Limite de maquinas excedido."));
}

TEST(MachineDisplayUtils, BlockedStateDominatesOfflineAndManagerOnlyAccess) {
    const auto machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        false,
        true,
        "blocked",
        "machine_limit_exceeded");

    EXPECT_EQ(remotex::viewer::MachineStatusText(machine), QString("Bloqueada"));
    EXPECT_EQ(remotex::viewer::MachineStatusTooltipText(machine),
              QString("Motivo: Limite de maquinas excedido\nConectividade: Offline"));
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(machine, false),
              QString("Esta maquina esta bloqueada: Limite de maquinas excedido."));
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(machine, true),
              QString("Esta maquina esta bloqueada: Limite de maquinas excedido."));
}

TEST(MachineDisplayUtils, IgnoresUnknownOrMissingBlockedMetadata) {
    const auto missing_reason = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        true,
        false,
        "blocked",
        "");
    const auto unknown_reason = BuildMachine(
        "2",
        "Desktop 02",
        "2026-03-14T10:00:00Z",
        true,
        false,
        "blocked",
        "unexpected_reason");
    const auto unknown_state = BuildMachine(
        "3",
        "Desktop 03",
        "2026-03-14T10:00:00Z",
        false,
        false,
        "unexpected_state",
        "machine_limit_exceeded");

    EXPECT_FALSE(remotex::viewer::IsMachineBlocked(missing_reason));
    EXPECT_TRUE(remotex::viewer::MachineBlockedReasonText(missing_reason).isEmpty());
    EXPECT_EQ(remotex::viewer::MachineStatusText(missing_reason), OnlineStatusText());
    EXPECT_TRUE(remotex::viewer::MachineStatusTooltipText(missing_reason).isEmpty());
    EXPECT_TRUE(remotex::viewer::MachineSessionAccessDeniedMessage(missing_reason, false).isEmpty());
    EXPECT_TRUE(remotex::viewer::CanStartMachineSession(missing_reason, false));

    EXPECT_FALSE(remotex::viewer::IsMachineBlocked(unknown_reason));
    EXPECT_TRUE(remotex::viewer::MachineBlockedReasonText(unknown_reason).isEmpty());
    EXPECT_EQ(remotex::viewer::MachineStatusText(unknown_reason), OnlineStatusText());
    EXPECT_TRUE(remotex::viewer::MachineSessionAccessDeniedMessage(unknown_reason, false).isEmpty());
    EXPECT_TRUE(remotex::viewer::CanStartMachineSession(unknown_reason, false));

    EXPECT_FALSE(remotex::viewer::IsMachineBlocked(unknown_state));
    EXPECT_TRUE(remotex::viewer::MachineBlockedReasonText(unknown_state).isEmpty());
    EXPECT_EQ(remotex::viewer::MachineStatusText(unknown_state), OfflineStatusText());
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(unknown_state, false),
              QString("A maquina esta offline."));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(unknown_state, false));
}

TEST(MachineDisplayUtils, ReleasedMachineRemovesBlockedAffordanceOnNextRefresh) {
    const auto blocked_machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        true,
        false,
        "blocked",
        "machine_limit_exceeded");
    const auto released_machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        true,
        false,
        "allowed",
        "");

    EXPECT_EQ(remotex::viewer::MachineStatusText(blocked_machine), QString("Bloqueada"));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(blocked_machine, false));

    EXPECT_FALSE(remotex::viewer::IsMachineBlocked(released_machine));
    EXPECT_EQ(remotex::viewer::MachineStatusText(released_machine), OnlineStatusText());
    EXPECT_TRUE(remotex::viewer::MachineStatusTooltipText(released_machine).isEmpty());
    EXPECT_TRUE(remotex::viewer::MachineSessionAccessDeniedMessage(released_machine, false).isEmpty());
    EXPECT_TRUE(remotex::viewer::CanStartMachineSession(released_machine, false));
}

TEST(MachineDisplayUtils, DeniesManagersOnlyMachinesForNonManagersAndAllowsManagers) {
    const auto machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        true,
        true);

    EXPECT_EQ(remotex::viewer::MachineStatusText(machine), OnlineStatusText());
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(machine, false),
              QString("Esta maquina permite acesso apenas para gerentes."));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(machine, false));
    EXPECT_TRUE(remotex::viewer::MachineSessionAccessDeniedMessage(machine, true).isEmpty());
    EXPECT_TRUE(remotex::viewer::CanStartMachineSession(machine, true));
}

TEST(MachineDisplayUtils, DeniesOfflineMachinesWhenNotBlocked) {
    const auto machine = BuildMachine(
        "1",
        "Desktop 01",
        "2026-03-14T10:00:00Z",
        false,
        false);

    EXPECT_EQ(remotex::viewer::MachineStatusText(machine), OfflineStatusText());
    EXPECT_EQ(remotex::viewer::MachineSessionAccessDeniedMessage(machine, false),
              QString("A maquina esta offline."));
    EXPECT_FALSE(remotex::viewer::CanStartMachineSession(machine, false));
}

TEST(MachineDisplayUtils, DetectsMachinesCreatedWithinTwentyFourHoursAsNew) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);
    const auto machine = BuildMachine("1", "Desktop 01", "2026-03-13T12:01:00Z");

    EXPECT_TRUE(remotex::viewer::IsMachineNew(machine, now));
}

TEST(MachineDisplayUtils, DoesNotMarkMachinesAsNewAtTwentyFourHoursOrMore) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);

    EXPECT_FALSE(remotex::viewer::IsMachineNew(
        BuildMachine("1", "Desktop 01", "2026-03-13T12:00:00Z"), now));
    EXPECT_FALSE(remotex::viewer::IsMachineNew(
        BuildMachine("2", "Desktop 02", "2026-03-12T12:00:00Z"), now));
}

TEST(MachineDisplayUtils, SupportsNanosecondCreatedAtForNewMachineDetection) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);
    const auto machine = BuildMachine("1", "Desktop 01", "2026-03-14T11:00:00.123456789Z");

    EXPECT_TRUE(remotex::viewer::IsMachineNew(machine, now));
    EXPECT_EQ(remotex::viewer::FormatMachineAgeText(machine.created_at, now), QString("00h59m"));
}

TEST(MachineDisplayUtils, SortsNewMachinesFirstAndKeepsOlderMachinesAlphabetical) {
    const QDateTime now = QDateTime::fromString("2026-03-14T12:00:00Z", Qt::ISODate);
    std::vector<remotex::core::MachineInfo> machines = {
        BuildMachine("4", "Zulu", "2026-03-11T08:00:00Z"),
        BuildMachine("2", "Bravo", "2026-03-14T11:30:00Z"),
        BuildMachine("3", "Alpha", "2026-03-10T08:00:00Z"),
        BuildMachine("1", "Delta", "2026-03-14T10:00:00Z"),
    };

    std::sort(machines.begin(), machines.end(), [&now](const auto& lhs, const auto& rhs) {
        return remotex::viewer::CompareMachinesForDisplay(lhs, rhs, now);
    });

    ASSERT_EQ(machines.size(), 4u);
    EXPECT_EQ(machines[0].id, "2");
    EXPECT_EQ(machines[1].id, "1");
    EXPECT_EQ(machines[2].id, "3");
    EXPECT_EQ(machines[3].id, "4");
}

remotex::core::MachineAuditEvent BuildAuditEvent(
    std::string session_id,
    std::string event_type,
    std::string created_at,
    std::string user_name = "admin",
    std::string status = "",
    std::string summary = "",
    int duration_sec = 0,
    bool has_duration = false,
    std::string transfer_direction = "",
    std::string transfer_item = "",
    uint64_t transfer_total_bytes = 0,
    uint32_t transfer_total_entries = 0,
    std::string error_message = "") {
    remotex::core::MachineAuditEvent event;
    event.session_id = std::move(session_id);
    event.event_type = std::move(event_type);
    event.created_at = std::move(created_at);
    event.user_name = std::move(user_name);
    event.status = std::move(status);
    event.summary = std::move(summary);
    event.duration_sec = duration_sec;
    event.has_duration = has_duration;
    event.transfer_direction = std::move(transfer_direction);
    event.transfer_item = std::move(transfer_item);
    event.transfer_total_bytes = transfer_total_bytes;
    event.transfer_total_entries = transfer_total_entries;
    event.error_message = std::move(error_message);
    return event;
}

TEST(ConsolidateAuditEvents, EmptyInputReturnsEmpty) {
    std::vector<remotex::core::MachineAuditEvent> events;
    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    EXPECT_TRUE(result.empty());
}

TEST(ConsolidateAuditEvents, PairsSessionStartAndEnd) {
    std::vector<remotex::core::MachineAuditEvent> events = {
        BuildAuditEvent("s1", "session_started", "2026-03-14T10:00:00Z", "admin", "started", "Interativo"),
        BuildAuditEvent("s1", "session_ended", "2026-03-14T11:00:00Z", "admin", "completed", "", 3600, true),
    };

    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].category, remotex::viewer::ConsolidatedAuditEntry::Category::Session);
    EXPECT_EQ(result[0].status, "completed");
    EXPECT_EQ(result[0].started_at, "2026-03-14T10:00:00Z");
    EXPECT_EQ(result[0].ended_at, "2026-03-14T11:00:00Z");
    EXPECT_TRUE(result[0].has_duration);
    EXPECT_EQ(result[0].duration_sec, 3600);
    EXPECT_EQ(result[0].summary, "Interativo");
}

TEST(ConsolidateAuditEvents, StartWithoutEndShowsStartedStatus) {
    std::vector<remotex::core::MachineAuditEvent> events = {
        BuildAuditEvent("s1", "session_started", "2026-03-14T10:00:00Z", "admin", "started", "Interativo"),
    };

    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].status, "started");
    EXPECT_FALSE(result[0].has_duration);
}

TEST(ConsolidateAuditEvents, EndWithoutStartCreatesStandaloneEntry) {
    std::vector<remotex::core::MachineAuditEvent> events = {
        BuildAuditEvent("s1", "session_ended", "2026-03-14T11:00:00Z", "admin", "completed", "", 3600, true),
    };

    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].status, "completed");
    EXPECT_EQ(result[0].ended_at, "2026-03-14T11:00:00Z");
    EXPECT_TRUE(result[0].started_at.empty());
    EXPECT_TRUE(result[0].has_duration);
    EXPECT_EQ(result[0].duration_sec, 3600);
}

TEST(ConsolidateAuditEvents, MultipleTransfersSameSessionAreSeparate) {
    std::vector<remotex::core::MachineAuditEvent> events = {
        BuildAuditEvent("s1", "file_upload_started", "2026-03-14T10:05:00Z", "admin", "started", "",
                         0, false, "upload", "docs.zip"),
        BuildAuditEvent("s1", "file_download_started", "2026-03-14T10:10:00Z", "admin", "started", "",
                         0, false, "download", "report.pdf"),
        BuildAuditEvent("s1", "file_upload_finished", "2026-03-14T10:06:00Z", "admin", "completed", "",
                         60, true, "upload", "docs.zip", 1024, 3),
        BuildAuditEvent("s1", "file_download_finished", "2026-03-14T10:12:00Z", "admin", "completed", "",
                         120, true, "download", "report.pdf", 2048, 1),
    };

    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    ASSERT_EQ(result.size(), 2u);
    for (const auto& entry : result) {
        EXPECT_EQ(entry.category, remotex::viewer::ConsolidatedAuditEntry::Category::FileTransfer);
        EXPECT_EQ(entry.status, "completed");
    }
}

TEST(ConsolidateAuditEvents, FailedTransferShowsFailedStatus) {
    std::vector<remotex::core::MachineAuditEvent> events = {
        BuildAuditEvent("s1", "file_upload_started", "2026-03-14T10:05:00Z", "admin", "started", "",
                         0, false, "upload", "big.zip"),
        BuildAuditEvent("s1", "file_transfer_failed", "2026-03-14T10:06:00Z", "admin", "failed", "",
                         0, false, "upload", "big.zip", 0, 0, "timeout"),
    };

    auto result = remotex::viewer::ConsolidateAuditEvents(events);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].status, "failed");
    EXPECT_EQ(result[0].error_message, "timeout");
}

} // namespace
