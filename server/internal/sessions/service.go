package sessions

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/audit"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

var ErrNotFound = errors.New("session not found")
var ErrInvalidAccessMode = errors.New("invalid access mode")
var ErrManagerAccessRequired = errors.New("manager access required")
var ErrMachineAccessDenied = errors.New("machine access denied")
var ErrMachineBlockedByLimit = errors.New("machine blocked by limit")
var ErrConcurrentSessionLimit = errors.New("concurrent session limit reached")
var ErrInvalidConsentDecision = errors.New("invalid consent decision")
var ErrSessionConsentNotPending = errors.New("session consent not pending")
var ErrHeartbeatAccessDenied = errors.New("session heartbeat access denied")

const (
	AccessModeInteractive = "interactive"
	AccessModeWatchOnly   = "watch_only"

	SessionStatusPendingConsent = "pending_consent"
	SessionStatusActive         = "active"
	SessionStatusDenied         = "denied"
	SessionStatusExpired        = "expired"
	SessionStatusEnded          = "ended"

	ConsentDecisionApproved = "approved"
	ConsentDecisionDenied   = "denied"
	ConsentDecisionExpired  = "expired"
)

// Session representa uma sessao de acesso remoto.
type Session struct {
	ID            string     `json:"id"`
	UserID        string     `json:"user_id"`
	CompanyID     string     `json:"company_id"`
	MachineID     string     `json:"machine_id"`
	AccessMode    string     `json:"access_mode"`
	Status        string     `json:"status"`
	StartedAt     time.Time  `json:"started_at"`
	EndedAt       *time.Time `json:"ended_at,omitempty"`
	DurationSec   *int       `json:"duration_sec,omitempty"`
	IPViewer      *string    `json:"ip_viewer,omitempty"`
	IPHost        *string    `json:"ip_host,omitempty"`
	RequesterName string     `json:"requester_name,omitempty"`
}

type sessionStore interface {
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
}

type machineLimitEvaluator interface {
	GetMachineState(ctx context.Context, companyID, machineID string) (*machinelimit.MachineState, error)
}

type Service struct {
	db              sessionStore
	auditSvc        *audit.Service
	machineLimitSvc machineLimitEvaluator
}

func NewService(db sessionStore, auditSvc *audit.Service, machineLimitSvc machineLimitEvaluator) *Service {
	if db == nil {
		panic("sessions: db is required")
	}
	if machineLimitSvc == nil {
		panic("sessions: machine limit evaluator is required")
	}
	return &Service{db: db, auditSvc: auditSvc, machineLimitSvc: machineLimitSvc}
}

func (s *Service) List(ctx context.Context, companyID string, limit, offset int) ([]Session, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}

	rows, err := s.db.Query(ctx,
		`SELECT s.id, s.user_id, s.company_id, s.machine_id, s.access_mode, s.status,
		        s.started_at, s.ended_at, s.duration_sec, s.ip_viewer, s.ip_host,
		        COALESCE(u.name, '')
		   FROM sessions s
		   LEFT JOIN users u ON u.id = s.user_id
		  WHERE s.company_id = $1
		  ORDER BY s.started_at DESC
		  LIMIT $2 OFFSET $3`,
		companyID, limit, offset,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var sessions []Session
	for rows.Next() {
		var sess Session
		if err := scanSession(rows, &sess); err != nil {
			return nil, err
		}
		sessions = append(sessions, sess)
	}
	if sessions == nil {
		sessions = []Session{}
	}
	return sessions, rows.Err()
}

func (s *Service) Get(ctx context.Context, sessionID, companyID string) (*Session, error) {
	var sess Session
	err := scanSession(s.db.QueryRow(ctx,
		`SELECT s.id, s.user_id, s.company_id, s.machine_id, s.access_mode, s.status,
		        s.started_at, s.ended_at, s.duration_sec, s.ip_viewer, s.ip_host,
		        COALESCE(u.name, '')
		   FROM sessions s
		   LEFT JOIN users u ON u.id = s.user_id
		  WHERE s.id = $1
		    AND s.company_id = $2`,
		sessionID, companyID,
	), &sess)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &sess, nil
}

type StartRequest struct {
	MachineID  string
	IPViewer   string
	IPHost     string
	AccessMode string
}

func (s *Service) authorizeMachineLimitState(ctx context.Context, companyID, machineID string) error {
	if s.machineLimitSvc == nil {
		return fmt.Errorf("%w: machine limit evaluator unavailable", ErrMachineBlockedByLimit)
	}

	state, err := s.machineLimitSvc.GetMachineState(ctx, companyID, machineID)
	if err != nil {
		return fmt.Errorf("%w: machine limit lookup failed: %v", ErrMachineBlockedByLimit, err)
	}
	if state == nil {
		return fmt.Errorf("%w: machine limit lookup returned no state", ErrMachineBlockedByLimit)
	}
	if strings.TrimSpace(state.CompanyID) != strings.TrimSpace(companyID) {
		return fmt.Errorf("%w: machine limit lookup returned company %q for %q", ErrMachineBlockedByLimit, state.CompanyID, companyID)
	}
	if strings.TrimSpace(state.MachineID) != strings.TrimSpace(machineID) {
		return fmt.Errorf("%w: machine limit lookup returned machine %q for %q", ErrMachineBlockedByLimit, state.MachineID, machineID)
	}
	if state.State == machinelimit.StateAllowed && state.BlockReason == machinelimit.BlockReasonNone {
		return nil
	}
	if state.State == machinelimit.StateBlocked {
		return ErrMachineBlockedByLimit
	}
	return fmt.Errorf("%w: machine limit lookup returned malformed state=%q reason=%q", ErrMachineBlockedByLimit, state.State, state.BlockReason)
}

func (s *Service) Start(ctx context.Context, userID, companyID string, isManager bool, req StartRequest) (*Session, error) {
	accessMode, err := normalizeAccessMode(req.AccessMode)
	if err != nil {
		return nil, err
	}

	var managersOnlyAccess bool
	var requireClientConsent bool
	err = s.db.QueryRow(ctx,
		`SELECT COALESCE(managers_only_access, FALSE),
		        COALESCE(require_client_consent, FALSE)
		   FROM machines
		  WHERE id = $1
		    AND company_id = $2`,
		req.MachineID, companyID,
	).Scan(&managersOnlyAccess, &requireClientConsent)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	if err := s.authorizeMachineLimitState(ctx, companyID, req.MachineID); err != nil {
		return nil, err
	}
	if err := authorizeManagersOnlyAccess(managersOnlyAccess, isManager); err != nil {
		return nil, err
	}

	// Check concurrent session limit for the company.
	var concurrentLimit *int
	_ = s.db.QueryRow(ctx,
		`SELECT concurrent_session_limit FROM companies WHERE id = $1`,
		companyID,
	).Scan(&concurrentLimit)
	if concurrentLimit != nil && *concurrentLimit > 0 {
		var activeCount int
		if err := s.db.QueryRow(ctx,
			`SELECT COUNT(*) FROM sessions
			  WHERE company_id = $1
			    AND status IN ('active', 'pending_consent')`,
			companyID,
		).Scan(&activeCount); err != nil {
			return nil, err
		}
		if activeCount >= *concurrentLimit {
			return nil, ErrConcurrentSessionLimit
		}
	}

	status := SessionStatusActive
	shouldAuditStart := true
	if accessMode == AccessModeInteractive && requireClientConsent {
		status = SessionStatusPendingConsent
		shouldAuditStart = false
	}

	var sess Session
	err = s.db.QueryRow(ctx,
		`INSERT INTO sessions (user_id, company_id, machine_id, ip_viewer, ip_host, access_mode, status)
		 SELECT $1, $2, m.id, $4::inet, $5::inet, $6, $7
		   FROM machines m
		  WHERE m.id = $3
		    AND m.company_id = $2
		 RETURNING id, user_id, company_id, machine_id, access_mode, status, started_at, ended_at, duration_sec, ip_viewer, ip_host,
		           COALESCE((SELECT name FROM users WHERE id = user_id), '')`,
		userID, companyID, req.MachineID, nilIfEmpty(req.IPViewer), nilIfEmpty(req.IPHost), accessMode, status,
	).Scan(&sess.ID, &sess.UserID, &sess.CompanyID, &sess.MachineID, &sess.AccessMode, &sess.Status, &sess.StartedAt, &sess.EndedAt, &sess.DurationSec, &sess.IPViewer, &sess.IPHost, &sess.RequesterName)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}

	if shouldAuditStart && s.auditSvc != nil {
		if err := s.auditSvc.RecordSessionStarted(ctx, companyID, sess.MachineID, sess.ID, userID, sess.RequesterName, sess.AccessMode); err != nil {
			return nil, err
		}
	}

	return &sess, nil
}

func (s *Service) ResolveConsent(ctx context.Context, sessionID, companyID, machineID, decision string) (*Session, error) {
	normalizedDecision, err := normalizeConsentDecision(decision)
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(machineID) == "" {
		return nil, ErrMachineAccessDenied
	}

	current, err := s.getSessionForMachine(ctx, sessionID, companyID, machineID)
	if err != nil {
		slog.Error("resolve consent: session lookup failed",
			"err", err, "session_id", sessionID, "machine_id", machineID)
		if errors.Is(err, ErrNotFound) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("session lookup: %w", err)
	}

	targetStatus := consentStatusFromDecision(normalizedDecision)
	if current.Status == targetStatus {
		return current, nil
	}
	if current.Status != SessionStatusPendingConsent {
		return nil, ErrSessionConsentNotPending
	}

	markStarted, markEnded := resolveConsentUpdateFlags(targetStatus)

	slog.Info("resolve consent: updating session",
		"session_id", sessionID, "target_status", targetStatus,
		"current_status", current.Status, "machine_id", machineID)

	var sess Session
	err = s.db.QueryRow(ctx,
		`UPDATE sessions
		    SET status = $4,
		        started_at = CASE WHEN $5 THEN NOW() ELSE started_at END,
		        ended_at = CASE WHEN $6 THEN NOW() ELSE ended_at END,
		        duration_sec = CASE WHEN $6 THEN NULL ELSE duration_sec END,
		        last_activity = CASE WHEN $5 THEN NOW() ELSE last_activity END
		  WHERE id = $1
		    AND company_id = $2
		    AND machine_id = $3
		 RETURNING id, user_id, company_id, machine_id, access_mode, status, started_at, ended_at, duration_sec, ip_viewer, ip_host,
		           COALESCE((SELECT name FROM users WHERE id = user_id), '')`,
		sessionID, companyID, machineID, targetStatus,
		markStarted, markEnded,
	).Scan(&sess.ID, &sess.UserID, &sess.CompanyID, &sess.MachineID, &sess.AccessMode, &sess.Status, &sess.StartedAt, &sess.EndedAt, &sess.DurationSec, &sess.IPViewer, &sess.IPHost, &sess.RequesterName)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		slog.Error("resolve consent: update/scan failed",
			"err", err, "session_id", sessionID, "machine_id", machineID, "target_status", targetStatus)
		return nil, fmt.Errorf("session update: %w", err)
	}

	if targetStatus == SessionStatusActive && s.auditSvc != nil {
		if err := s.auditSvc.RecordSessionStarted(ctx, sess.CompanyID, sess.MachineID, sess.ID, sess.UserID, sess.RequesterName, sess.AccessMode); err != nil {
			slog.Error("resolve consent: audit recording failed",
				"err", err, "session_id", sess.ID, "machine_id", sess.MachineID)
			return nil, fmt.Errorf("audit recording: %w", err)
		}
	}

	return &sess, nil
}

func (s *Service) End(ctx context.Context, sessionID, companyID string) (*Session, error) {
	current, err := s.Get(ctx, sessionID, companyID)
	if err != nil {
		return nil, err
	}

	if current.EndedAt != nil && (current.Status == SessionStatusEnded || current.Status == SessionStatusDenied || current.Status == SessionStatusExpired) {
		return current, nil
	}

	nextStatus := current.Status
	if current.Status == SessionStatusActive || current.Status == SessionStatusPendingConsent {
		nextStatus = SessionStatusEnded
	}

	var sess Session
	err = s.db.QueryRow(ctx,
		`UPDATE sessions
		    SET status = $3,
		        ended_at = COALESCE(ended_at, NOW()),
		        duration_sec = CASE
		            WHEN $4 AND started_at IS NOT NULL THEN EXTRACT(EPOCH FROM (NOW() - started_at))::integer
		            ELSE duration_sec
		        END
		  WHERE id = $1
		    AND company_id = $2
		 RETURNING id, user_id, company_id, machine_id, access_mode, status, started_at, ended_at, duration_sec, ip_viewer, ip_host,
		           COALESCE((SELECT name FROM users WHERE id = user_id), '')`,
		sessionID, companyID, nextStatus, current.Status == SessionStatusActive,
	).Scan(&sess.ID, &sess.UserID, &sess.CompanyID, &sess.MachineID, &sess.AccessMode, &sess.Status, &sess.StartedAt, &sess.EndedAt, &sess.DurationSec, &sess.IPViewer, &sess.IPHost, &sess.RequesterName)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}

	if current.Status == SessionStatusActive && s.auditSvc != nil {
		if err := s.auditSvc.RecordSessionEnded(ctx, companyID, sess.MachineID, sess.ID, sess.UserID, sess.RequesterName, sess.AccessMode, sess.DurationSec); err != nil {
			return nil, err
		}
	}

	return &sess, nil
}

func nilIfEmpty(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

func normalizeAccessMode(mode string) (string, error) {
	normalized := strings.TrimSpace(strings.ToLower(mode))
	if normalized == "" {
		return AccessModeInteractive, nil
	}
	switch normalized {
	case AccessModeInteractive, AccessModeWatchOnly:
		return normalized, nil
	default:
		return "", ErrInvalidAccessMode
	}
}

func normalizeConsentDecision(decision string) (string, error) {
	normalized := strings.TrimSpace(strings.ToLower(decision))
	switch normalized {
	case ConsentDecisionApproved, ConsentDecisionDenied, ConsentDecisionExpired:
		return normalized, nil
	default:
		return "", ErrInvalidConsentDecision
	}
}

func consentStatusFromDecision(decision string) string {
	switch decision {
	case ConsentDecisionDenied:
		return SessionStatusDenied
	case ConsentDecisionExpired:
		return SessionStatusExpired
	default:
		return SessionStatusActive
	}
}

func resolveConsentUpdateFlags(targetStatus string) (markStarted bool, markEnded bool) {
	switch targetStatus {
	case SessionStatusActive:
		return true, false
	case SessionStatusDenied, SessionStatusExpired:
		return false, true
	default:
		return false, false
	}
}

func authorizeManagersOnlyAccess(managersOnlyAccess bool, isManager bool) error {
	if managersOnlyAccess && !isManager {
		return ErrManagerAccessRequired
	}
	return nil
}

type sessionScanner interface {
	Scan(dest ...interface{}) error
}

func scanSession(scanner sessionScanner, sess *Session) error {
	return scanner.Scan(
		&sess.ID,
		&sess.UserID,
		&sess.CompanyID,
		&sess.MachineID,
		&sess.AccessMode,
		&sess.Status,
		&sess.StartedAt,
		&sess.EndedAt,
		&sess.DurationSec,
		&sess.IPViewer,
		&sess.IPHost,
		&sess.RequesterName,
	)
}

// Heartbeat updates the last_activity timestamp for an active session owned by the authenticated actor.
func (s *Service) Heartbeat(ctx context.Context, sessionID, companyID, userID, machineID string, isMachine bool) error {
	var ownerUserID string
	var ownerMachineID string
	var status string

	err := s.db.QueryRow(ctx,
		`SELECT user_id, machine_id, status
		   FROM sessions
		  WHERE id = $1
		    AND company_id = $2`,
		sessionID, companyID,
	).Scan(&ownerUserID, &ownerMachineID, &status)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}

	if isMachine {
		if machineID == "" || ownerMachineID != machineID {
			return ErrHeartbeatAccessDenied
		}
	} else {
		if userID == "" || ownerUserID != userID {
			return ErrHeartbeatAccessDenied
		}
	}

	if status != SessionStatusActive && status != SessionStatusPendingConsent {
		return ErrNotFound
	}

	tag, err := s.db.Exec(ctx,
		`UPDATE sessions
		    SET last_activity = NOW()
		  WHERE id = $1
		    AND company_id = $2
		    AND status IN ('active', 'pending_consent')`,
		sessionID, companyID,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}

	return nil
}

// CloseOrphanSessions closes active/pending sessions whose machine is offline
// OR whose viewer has not sent a heartbeat within the given timeout.
func (s *Service) CloseOrphanSessions(ctx context.Context, viewerTimeout time.Duration) (int, error) {
	rows, err := s.db.Query(ctx,
		`UPDATE sessions s
		    SET status = 'ended',
		        ended_at = COALESCE(s.ended_at, NOW()),
		        duration_sec = CASE
		            WHEN s.status = 'active' AND s.started_at IS NOT NULL
		            THEN EXTRACT(EPOCH FROM (NOW() - s.started_at))::integer
		            ELSE s.duration_sec
		        END
		   FROM machines m
		  WHERE s.machine_id = m.id
		    AND s.status IN ('active', 'pending_consent')
		    AND (m.is_online = FALSE
		         OR (s.last_activity IS NOT NULL AND s.last_activity < NOW() - $1::interval))
		 RETURNING s.id, s.company_id, s.machine_id, s.user_id, s.access_mode, s.status, s.duration_sec,
		           COALESCE((SELECT name FROM users WHERE id = s.user_id), '')`,
		viewerTimeout.String())
	if err != nil {
		return 0, err
	}
	defer rows.Close()

	var count int
	for rows.Next() {
		var id, companyID, machineID, userID, accessMode, status, requesterName string
		var durationSec *int
		if err := rows.Scan(&id, &companyID, &machineID, &userID, &accessMode, &status, &durationSec, &requesterName); err != nil {
			return count, err
		}
		count++

		if s.auditSvc != nil {
			_ = s.auditSvc.RecordSessionEnded(ctx, companyID, machineID, id, userID, requesterName, accessMode, durationSec)
		}
	}
	return count, rows.Err()
}

func (s *Service) getSessionForMachine(ctx context.Context, sessionID, companyID, machineID string) (*Session, error) {
	var sess Session
	err := scanSession(s.db.QueryRow(ctx,
		`SELECT s.id, s.user_id, s.company_id, s.machine_id, s.access_mode, s.status,
		        s.started_at, s.ended_at, s.duration_sec, s.ip_viewer, s.ip_host,
		        COALESCE(u.name, '')
		   FROM sessions s
		   LEFT JOIN users u ON u.id = s.user_id
		  WHERE s.id = $1
		    AND s.company_id = $2
		    AND s.machine_id = $3`,
		sessionID, companyID, machineID,
	), &sess)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &sess, nil
}

func (s *Service) getSessionForMachineKey(ctx context.Context, sessionID, companyID, machineKey string) (*Session, error) {
	var sess Session
	err := scanSession(s.db.QueryRow(ctx,
		`SELECT s.id, s.user_id, s.company_id, s.machine_id, s.access_mode, s.status,
		        s.started_at, s.ended_at, s.duration_sec, s.ip_viewer, s.ip_host,
		        COALESCE(u.name, '')
		   FROM sessions s
		   JOIN machines m ON m.id = s.machine_id
		   LEFT JOIN users u ON u.id = s.user_id
		  WHERE s.id = $1
		    AND s.company_id = $2
		    AND m.machine_key = $3`,
		sessionID, companyID, machineKey,
	), &sess)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &sess, nil
}
