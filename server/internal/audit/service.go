package audit

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	ErrMachineNotFound = errors.New("machine not found")
	ErrInvalidEvent    = errors.New("invalid audit event")
)

type Event struct {
	ID                   string    `json:"id"`
	CompanyID            string    `json:"company_id"`
	MachineID            string    `json:"machine_id"`
	SessionID            *string   `json:"session_id,omitempty"`
	UserID               *string   `json:"user_id,omitempty"`
	UserName             *string   `json:"user_name,omitempty"`
	MachineName          *string   `json:"machine_name,omitempty"`
	UserNameSnapshot     *string   `json:"user_name_snapshot,omitempty"`
	EventType            string    `json:"event_type"`
	Status               *string   `json:"status,omitempty"`
	Summary              string    `json:"summary"`
	TransferDirection    *string   `json:"transfer_direction,omitempty"`
	TransferRoot         *string   `json:"transfer_root,omitempty"`
	TransferItem         *string   `json:"transfer_item,omitempty"`
	TransferTotalBytes   *int64    `json:"transfer_total_bytes,omitempty"`
	TransferTotalEntries *int      `json:"transfer_total_entries,omitempty"`
	DurationSec          *int      `json:"duration_sec,omitempty"`
	ErrorMessage         *string   `json:"error_message,omitempty"`
	CreatedAt            time.Time `json:"created_at"`
}

type RecordEventInput struct {
	CompanyID            string
	MachineID            string
	SessionID            *string
	UserID               *string
	MachineName          *string
	UserNameSnapshot     *string
	EventType            string
	Status               *string
	Summary              string
	TransferDirection    *string
	TransferRoot         *string
	TransferItem         *string
	TransferTotalBytes   *int64
	TransferTotalEntries *int
	DurationSec          *int
	ErrorMessage         *string
}

type Service struct {
	db *pgxpool.Pool
}

func NewService(db *pgxpool.Pool) *Service {
	return &Service{db: db}
}

func (s *Service) ListMachineEvents(ctx context.Context, companyID, machineID string, limit, offset int) ([]Event, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}

	rows, err := s.db.Query(ctx, `
		SELECT ae.id,
		       ae.company_id,
		       ae.machine_id,
		       ae.session_id,
		       ae.user_id,
		       COALESCE(u.name, ae.user_name_snapshot),
		       ae.machine_name,
		       ae.user_name_snapshot,
		       ae.event_type,
		       ae.status,
		       ae.summary,
		       ae.transfer_direction,
		       ae.transfer_root,
		       ae.transfer_item,
		       ae.transfer_total_bytes,
		       ae.transfer_total_entries,
		       ae.duration_sec,
		       ae.error_message,
		       ae.created_at
		  FROM audit_events ae
		  LEFT JOIN users u
		    ON u.id = ae.user_id
		 WHERE ae.company_id = $1
		   AND ae.machine_id = $2
		 ORDER BY ae.created_at DESC
		 LIMIT $3 OFFSET $4`,
		companyID, machineID, limit, offset,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var events []Event
	for rows.Next() {
		var event Event
		if err := rows.Scan(
			&event.ID,
			&event.CompanyID,
			&event.MachineID,
			&event.SessionID,
			&event.UserID,
			&event.UserName,
			&event.MachineName,
			&event.UserNameSnapshot,
			&event.EventType,
			&event.Status,
			&event.Summary,
			&event.TransferDirection,
			&event.TransferRoot,
			&event.TransferItem,
			&event.TransferTotalBytes,
			&event.TransferTotalEntries,
			&event.DurationSec,
			&event.ErrorMessage,
			&event.CreatedAt,
		); err != nil {
			return nil, err
		}
		events = append(events, event)
	}
	if events == nil {
		events = []Event{}
	}
	return events, rows.Err()
}

func (s *Service) RecordEvent(ctx context.Context, input RecordEventInput) (*Event, error) {
	normalized, err := normalizeInput(input)
	if err != nil {
		return nil, err
	}

	var event Event
	err = s.db.QueryRow(ctx, `
		INSERT INTO audit_events (
			company_id, machine_id, session_id, user_id,
			machine_name, user_name_snapshot,
			event_type, status, summary,
			transfer_direction, transfer_root, transfer_item,
			transfer_total_bytes, transfer_total_entries,
			duration_sec, error_message
		)
		SELECT $1, m.id, $3, $4,
		       m.name, $5,
		       $6, $7, $8, $9, $10, $11, $12, $13, $14, $15
		  FROM machines m
		 WHERE m.id = $2
		   AND m.company_id = $1
		RETURNING id, company_id, machine_id, session_id, user_id,
		          NULL::varchar(255) AS user_name,
		          machine_name, user_name_snapshot,
		          event_type, status, summary,
		          transfer_direction, transfer_root, transfer_item,
		          transfer_total_bytes, transfer_total_entries,
		          duration_sec, error_message, created_at`,
		normalized.CompanyID,
		normalized.MachineID,
		normalized.SessionID,
		normalized.UserID,
		normalized.UserNameSnapshot,
		normalized.EventType,
		normalized.Status,
		normalized.Summary,
		normalized.TransferDirection,
		normalized.TransferRoot,
		normalized.TransferItem,
		normalized.TransferTotalBytes,
		normalized.TransferTotalEntries,
		normalized.DurationSec,
		normalized.ErrorMessage,
	).Scan(
		&event.ID,
		&event.CompanyID,
		&event.MachineID,
		&event.SessionID,
		&event.UserID,
		&event.UserName,
		&event.MachineName,
		&event.UserNameSnapshot,
		&event.EventType,
		&event.Status,
		&event.Summary,
		&event.TransferDirection,
		&event.TransferRoot,
		&event.TransferItem,
		&event.TransferTotalBytes,
		&event.TransferTotalEntries,
		&event.DurationSec,
		&event.ErrorMessage,
		&event.CreatedAt,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrMachineNotFound
	}
	if err != nil {
		return nil, err
	}
	return &event, nil
}

func (s *Service) RecordSessionStarted(ctx context.Context, companyID, machineID, sessionID, userID, userName, accessMode string) error {
	_, err := s.RecordEvent(ctx, RecordEventInput{
		CompanyID:        companyID,
		MachineID:        machineID,
		SessionID:        stringPtr(sessionID),
		UserID:           stringPtr(userID),
		UserNameSnapshot: stringPtr(userName),
		EventType:        "session_started",
		Status:           stringPtr("started"),
		Summary:          sessionAuditSummary("Acesso remoto iniciado", accessMode, nil),
	})
	return err
}

func (s *Service) RecordSessionEnded(ctx context.Context, companyID, machineID, sessionID, userID, userName, accessMode string, durationSec *int) error {
	_, err := s.RecordEvent(ctx, RecordEventInput{
		CompanyID:        companyID,
		MachineID:        machineID,
		SessionID:        stringPtr(sessionID),
		UserID:           stringPtr(userID),
		UserNameSnapshot: stringPtr(userName),
		EventType:        "session_ended",
		Status:           stringPtr("completed"),
		Summary:          sessionAuditSummary("Acesso remoto encerrado", accessMode, durationSec),
		DurationSec:      durationSec,
	})
	return err
}

type CompanyEventsFilter struct {
	EventType string
	MachineID string
	UserID    string
	DateFrom  string // YYYY-MM-DD
	DateTo    string // YYYY-MM-DD
	Search    string
	Timezone  string // IANA timezone, e.g. "America/Sao_Paulo"
}

func (s *Service) ListCompanyEvents(ctx context.Context, companyID string, filter CompanyEventsFilter, limit, offset int) ([]Event, int, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}

	where := "WHERE ae.company_id = $1"
	args := []interface{}{companyID}
	argIdx := 2

	if filter.EventType != "" {
		where += fmt.Sprintf(" AND ae.event_type = $%d", argIdx)
		args = append(args, filter.EventType)
		argIdx++
	}
	if filter.MachineID != "" {
		where += fmt.Sprintf(" AND ae.machine_id = $%d", argIdx)
		args = append(args, filter.MachineID)
		argIdx++
	}
	if filter.UserID != "" {
		where += fmt.Sprintf(" AND ae.user_id = $%d", argIdx)
		args = append(args, filter.UserID)
		argIdx++
	}
	if filter.DateFrom != "" {
		tz := filter.Timezone
		if tz == "" {
			tz = "America/Sao_Paulo"
		}
		where += fmt.Sprintf(" AND ae.created_at >= ($%d::date AT TIME ZONE $%d)", argIdx, argIdx+1)
		args = append(args, filter.DateFrom, tz)
		argIdx += 2
	}
	if filter.DateTo != "" {
		tz := filter.Timezone
		if tz == "" {
			tz = "America/Sao_Paulo"
		}
		where += fmt.Sprintf(" AND ae.created_at < (($%d::date + INTERVAL '1 day') AT TIME ZONE $%d)", argIdx, argIdx+1)
		args = append(args, filter.DateTo, tz)
		argIdx += 2
	}
	if filter.Search != "" {
		where += fmt.Sprintf(" AND ae.summary ILIKE $%d", argIdx)
		args = append(args, "%"+filter.Search+"%")
		argIdx++
	}

	// Count total
	countQuery := fmt.Sprintf(`SELECT COUNT(*) FROM audit_events ae %s`, where)
	var total int
	if err := s.db.QueryRow(ctx, countQuery, args...).Scan(&total); err != nil {
		return nil, 0, err
	}

	// Fetch events
	query := fmt.Sprintf(`
		SELECT ae.id,
		       ae.company_id,
		       COALESCE(ae.machine_id::text, ''),
		       ae.session_id,
		       ae.user_id,
		       COALESCE(u.name, ae.user_name_snapshot),
		       COALESCE(m.name, ae.machine_name, '(deletado)'),
		       ae.user_name_snapshot,
		       ae.event_type,
		       ae.status,
		       ae.summary,
		       ae.transfer_direction,
		       ae.transfer_root,
		       ae.transfer_item,
		       ae.transfer_total_bytes,
		       ae.transfer_total_entries,
		       ae.duration_sec,
		       ae.error_message,
		       ae.created_at
		  FROM audit_events ae
		  LEFT JOIN users u ON u.id = ae.user_id
		  LEFT JOIN machines m ON m.id = ae.machine_id
		 %s
		 ORDER BY ae.created_at DESC
		 LIMIT $%d OFFSET $%d`,
		where, argIdx, argIdx+1,
	)
	args = append(args, limit, offset)

	rows, err := s.db.Query(ctx, query, args...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()

	var events []Event
	for rows.Next() {
		var event Event
		var machineIDStr string
		if err := rows.Scan(
			&event.ID,
			&event.CompanyID,
			&machineIDStr,
			&event.SessionID,
			&event.UserID,
			&event.UserName,
			&event.MachineName,
			&event.UserNameSnapshot,
			&event.EventType,
			&event.Status,
			&event.Summary,
			&event.TransferDirection,
			&event.TransferRoot,
			&event.TransferItem,
			&event.TransferTotalBytes,
			&event.TransferTotalEntries,
			&event.DurationSec,
			&event.ErrorMessage,
			&event.CreatedAt,
		); err != nil {
			return nil, 0, err
		}
		event.MachineID = machineIDStr
		events = append(events, event)
	}
	if events == nil {
		events = []Event{}
	}
	return events, total, rows.Err()
}

func normalizeInput(input RecordEventInput) (RecordEventInput, error) {
	input.CompanyID = strings.TrimSpace(input.CompanyID)
	input.MachineID = strings.TrimSpace(input.MachineID)
	input.EventType = strings.TrimSpace(input.EventType)
	input.Summary = strings.TrimSpace(input.Summary)
	input.Status = normalizeOptionalString(input.Status)
	input.SessionID = normalizeOptionalString(input.SessionID)
	input.UserID = normalizeOptionalString(input.UserID)
	input.TransferDirection = normalizeOptionalString(input.TransferDirection)
	input.TransferRoot = normalizeOptionalString(input.TransferRoot)
	input.TransferItem = normalizeOptionalString(input.TransferItem)
	input.ErrorMessage = normalizeOptionalString(input.ErrorMessage)

	if input.CompanyID == "" || input.MachineID == "" || input.EventType == "" {
		return RecordEventInput{}, ErrInvalidEvent
	}
	if input.Summary == "" {
		input.Summary = defaultSummary(input.EventType, input.TransferDirection)
	}
	if input.Summary == "" {
		return RecordEventInput{}, ErrInvalidEvent
	}

	return input, nil
}

func defaultSummary(eventType string, direction *string) string {
	switch eventType {
	case "session_started":
		return "Acesso remoto iniciado"
	case "session_ended":
		return "Acesso remoto encerrado"
	case "file_upload_started":
		return "Envio de arquivos iniciado"
	case "file_upload_finished":
		return "Envio de arquivos concluido"
	case "file_download_started":
		return "Recebimento de arquivos iniciado"
	case "file_download_finished":
		return "Recebimento de arquivos concluido"
	case "file_transfer_failed":
		if direction != nil && *direction == "download" {
			return "Falha ao receber arquivos"
		}
		return "Falha na transferencia de arquivos"
	default:
		return ""
	}
}

func normalizeOptionalString(value *string) *string {
	if value == nil {
		return nil
	}
	trimmed := strings.TrimSpace(*value)
	if trimmed == "" {
		return nil
	}
	return &trimmed
}

func stringPtr(value string) *string {
	if strings.TrimSpace(value) == "" {
		return nil
	}
	return &value
}

func sessionAuditSummary(prefix, accessMode string, durationSec *int) string {
	modeLabel := "controle remoto"
	if strings.TrimSpace(accessMode) == "watch_only" {
		modeLabel = "somente visualizacao"
	}
	if durationSec != nil {
		return fmt.Sprintf("%s (%s, %ds)", prefix, modeLabel, *durationSec)
	}
	return fmt.Sprintf("%s (%s)", prefix, modeLabel)
}
