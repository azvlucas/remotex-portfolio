#pragma once
// RemoteX - core/machine_client.h
// Cliente HTTP para a API de gestao de maquinas e grupos.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remotex::core {

class AuthClient;

enum class SessionAccessMode {
    Interactive,
    WatchOnly,
};

inline constexpr std::string_view kSessionAccessModeInteractive = "interactive";
inline constexpr std::string_view kSessionAccessModeWatchOnly   = "watch_only";
inline constexpr std::string_view kSessionStatusPendingConsent  = "pending_consent";
inline constexpr std::string_view kSessionStatusActive          = "active";
inline constexpr std::string_view kSessionStatusDenied          = "denied";
inline constexpr std::string_view kSessionStatusExpired         = "expired";
inline constexpr std::string_view kSessionStatusEnded           = "ended";
inline constexpr std::string_view kSessionConsentApproved       = "approved";
inline constexpr std::string_view kSessionConsentDenied         = "denied";
inline constexpr std::string_view kSessionConsentExpired        = "expired";

inline constexpr std::string_view SessionAccessModeToString(SessionAccessMode mode) {
    switch (mode) {
        case SessionAccessMode::WatchOnly:
            return kSessionAccessModeWatchOnly;
        case SessionAccessMode::Interactive:
        default:
            return kSessionAccessModeInteractive;
    }
}

inline constexpr SessionAccessMode SessionAccessModeFromString(std::string_view value) {
    return value == kSessionAccessModeWatchOnly
        ? SessionAccessMode::WatchOnly
        : SessionAccessMode::Interactive;
}

struct MachineInfo {
    std::string id;
    std::string name;
    std::string machine_key;
    std::string os_info;
    std::string mac_address;
    std::string agent_version;
    std::string last_heartbeat;
    std::string created_at;
    std::string group_id;
    std::string group_name;
    std::string subgroup_id;
    std::string subgroup_name;
    std::string legal_name;
    std::string trade_name;
    std::string document_type;
    std::string document_number;
    std::string state;
    std::string block_reason;
    bool        managers_only_access = false;
    bool        require_client_consent = false;
    bool        is_online = false;
};

struct MachineGroupInfo {
    std::string id;
    std::string name;
};

struct MachineSubgroupInfo {
    std::string id;
    std::string group_id;
    std::string name;
};

struct MachineStatus {
    std::string id;
    bool        is_online = false;
    std::string last_heartbeat;
};

struct SessionInfo {
    std::string id;
    std::string user_id;
    std::string company_id;
    std::string machine_id;
    SessionAccessMode access_mode = SessionAccessMode::Interactive;
    std::string status;
    std::string started_at;
    std::string ended_at;
    std::string requester_name;
    int         duration_sec = 0;
    bool        has_duration = false;
    int         relay_protocol_version = 0;
    std::string relay_session_id;
    std::string relay_role;
    std::string relay_ticket;
    int64_t     relay_ticket_expires_unix = 0;
    std::string relay_host;
    uint16_t    relay_port = 0;
};

struct MachineAuditEvent {
    std::string id;
    std::string machine_id;
    std::string session_id;
    std::string user_id;
    std::string user_name;
    std::string event_type;
    std::string status;
    std::string summary;
    std::string transfer_direction;
    std::string transfer_root;
    std::string transfer_item;
    uint64_t    transfer_total_bytes = 0;
    uint32_t    transfer_total_entries = 0;
    int         duration_sec = 0;
    bool        has_duration = false;
    std::string error_message;
    std::string created_at;
};

struct MachineResult {
    bool        success = false;
    std::string error;
};

struct MachineInfoResult {
    std::optional<MachineInfo> machine;
    std::string                error;
    int                        http_code = 0;
};

struct MachineGroupResult {
    std::optional<MachineGroupInfo> group;
    std::string                     error;
    int                             http_code = 0;
};

struct MachineSubgroupResult {
    std::optional<MachineSubgroupInfo> subgroup;
    std::string                        error;
    int                                http_code = 0;
};

struct SessionInfoResult {
    std::optional<SessionInfo> session;
    std::string                error;
    int                        http_code = 0;
};

struct RecordingInfo {
    std::string id;
    std::string session_id;
    std::string machine_id;
    int64_t     file_size = 0;
    int         duration_sec = 0;
    std::string status;
    std::string created_at;
};

struct RecordingDownloadLinkResult {
    std::string url;
    std::string error;
    int         http_code = 0;
};

struct CompanyUserInfo {
    std::string id;
    std::string name;
    std::string email;
    std::string role;
    bool        is_active = false;
    std::string created_at;
};

// ── Report structs ──────────────────────────────────────────────────
struct ReportMachineStats {
    std::string machine_id;
    std::string machine_name;
    int         session_count = 0;
    int         total_duration_sec = 0;
};

struct ReportTechnicianStats {
    std::string user_id;
    std::string user_name;
    int         session_count = 0;
    int         total_duration_sec = 0;
};

struct ReportDailyStats {
    std::string day;  // YYYY-MM-DD
    int         session_count = 0;
    int         total_duration_sec = 0;
};

struct ReportDashboardData {
    int total_sessions = 0;
    int total_duration_sec = 0;
    int avg_duration_sec = 0;
    std::vector<ReportMachineStats>    by_machine;
    std::vector<ReportTechnicianStats> by_technician;
    std::vector<ReportDailyStats>      daily;
    std::string error;  // non-empty on failure
};

struct MachineClientConfig {
    std::string api_host;
    uint16_t    api_port = 8081;
    bool        use_tls  = false;
};

struct MachineUpdateRequest {
    std::string name;
    std::string os_info;
    std::string mac_address;
    std::string group_id;
    std::string subgroup_id;
    std::string legal_name;
    std::string trade_name;
    std::string document_type;
    std::string document_number;
    bool        managers_only_access = false;
    bool        require_client_consent = false;
};

struct MachineAuditEventRequest {
    std::string session_id;
    std::string event_type;
    std::string status;
    std::string summary;
    std::string transfer_direction;
    std::string transfer_root;
    std::string transfer_item;
    uint64_t    transfer_total_bytes = 0;
    uint32_t    transfer_total_entries = 0;
    int         duration_sec = 0;
    std::string error_message;
};

class MachineClient {
public:
    MachineClient() = default;
    ~MachineClient() = default;

    void Configure(const MachineClientConfig& config, AuthClient* auth);

    std::vector<MachineInfo> ListMachines();
    std::optional<MachineInfo> RegisterMachine(const std::string& name,
                                               const std::string& os_info = "");
    std::optional<MachineInfo> UpdateMachine(const std::string& id,
                                             const std::string& name,
                                             const std::string& os_info = "",
                                             const std::string& mac_address = "");
    MachineInfoResult UpdateMachineDetails(const std::string& id,
                                           const MachineUpdateRequest& request);
    MachineResult DeleteMachine(const std::string& id);

    std::vector<MachineGroupInfo> ListGroups();
    MachineGroupResult CreateGroup(const std::string& name);
    MachineGroupResult UpdateGroup(const std::string& id, const std::string& name);
    MachineResult DeleteGroup(const std::string& id);

    std::vector<MachineSubgroupInfo> ListSubgroups(const std::string& group_id);
    MachineSubgroupResult CreateSubgroup(const std::string& group_id, const std::string& name);
    MachineSubgroupResult UpdateSubgroup(const std::string& id, const std::string& name);
    MachineResult DeleteSubgroup(const std::string& id);

    SessionInfoResult StartSession(const std::string& machine_id,
                                   SessionAccessMode access_mode = SessionAccessMode::Interactive);
    SessionInfoResult GetSession(const std::string& session_id);
    SessionInfoResult ResolveSessionConsent(const std::string& session_id,
                                            const std::string& decision,
                                            const std::string& machine_key = "");
    MachineResult EndSession(const std::string& session_id);
    MachineResult SessionHeartbeat(const std::string& session_id);
    std::vector<MachineAuditEvent> ListMachineAudit(const std::string& machine_id,
                                                    int limit = 50,
                                                    int offset = 0);
    MachineResult CreateMachineAuditEvent(const std::string& machine_id,
                                          const MachineAuditEventRequest& request);

    std::optional<MachineStatus> GetMachineStatus(const std::string& id);

    struct StatusResult {
        std::optional<MachineStatus> status;
        int http_code = 0;
    };
    StatusResult GetMachineStatusEx(const std::string& id);

    struct HeartbeatResult {
        bool success = false;
        bool force_update = false;
        std::string error;
    };
    HeartbeatResult SendHeartbeat(const std::string& machine_id,
                                 const std::string& machine_key,
                                 int64_t uptime_sec = 0,
                                 const std::string& agent_version = "");

    MachineResult RequestForceUpdate(const std::string& machine_id);

    // ── Recordings API ──────────────────────────────────────────────────
    std::vector<RecordingInfo> ListRecordings(int limit = 50, int offset = 0);
    std::vector<RecordingInfo> ListRecordingsByMachine(const std::string& machine_id,
                                                       int limit = 50, int offset = 0);
    RecordingDownloadLinkResult GetRecordingDownloadURL(const std::string& id);
    MachineResult DeleteRecording(const std::string& id);

    // ── Company Users API ───────────────────────────────────────────────
    std::vector<CompanyUserInfo> ListCompanyUsers();
    std::optional<CompanyUserInfo> CreateCompanyUser(const std::string& name,
                                                      const std::string& email,
                                                      const std::string& password);
    MachineResult ActivateCompanyUser(const std::string& id);
    MachineResult DeactivateCompanyUser(const std::string& id);
    MachineResult ResetCompanyUserPassword(const std::string& id,
                                            const std::string& new_password);

    // ── Reports API ──────────────────────────────────────────────────────
    ReportDashboardData GetReportDashboard(const std::string& date_from,
                                           const std::string& date_to,
                                           const std::string& tz = "America/Sao_Paulo");

private:
    struct HttpResponse {
        int         status_code = 0;
        std::string body;
    };

    HttpResponse HttpRequest(const std::string& method,
                             const std::string& path,
                             const std::string& json_body = "");

    static MachineInfo ParseMachineJson(const std::string& json);
    static MachineGroupInfo ParseGroupJson(const std::string& json);
    static MachineSubgroupInfo ParseSubgroupJson(const std::string& json);
    static SessionInfo ParseSessionJson(const std::string& json);
    static MachineAuditEvent ParseAuditEventJson(const std::string& json);
    static RecordingInfo ParseRecordingJson(const std::string& json);
    static CompanyUserInfo ParseCompanyUserJson(const std::string& json);

    MachineClientConfig config_;
    AuthClient*         auth_ = nullptr;
};

} // namespace remotex::core
