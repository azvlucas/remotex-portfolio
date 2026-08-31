package reports

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

const dashboardQuery = `
WITH filtered_sessions AS MATERIALIZED (
	SELECT s.machine_id,
	       s.user_id,
	       s.started_at,
	       CASE
	           WHEN s.status = 'ended' THEN COALESCE(s.duration_sec, 0)
	           ELSE LEAST(EXTRACT(EPOCH FROM (NOW() - s.started_at))::integer, 28800)
	       END AS effective_duration_sec
	  FROM sessions s
	 WHERE s.company_id = $1
	   AND s.status IN ('ended', 'active')
	   AND s.started_at IS NOT NULL
	   AND s.started_at >= ($2::date AT TIME ZONE $4)
	   AND s.started_at < (($3::date + INTERVAL '1 day') AT TIME ZONE $4)
),
summary AS MATERIALIZED (
	SELECT COUNT(*) AS total_sessions,
	       COALESCE(SUM(effective_duration_sec), 0) AS total_duration_sec,
	       COALESCE(AVG(effective_duration_sec)::integer, 0) AS avg_duration_sec
	  FROM filtered_sessions
),
machine_stats AS MATERIALIZED (
	SELECT COALESCE(fs.machine_id::text, '') AS machine_id,
	       COALESCE(m.name, '(deletado)') AS machine_name,
	       COUNT(*) AS session_count,
	       COALESCE(SUM(fs.effective_duration_sec), 0) AS total_duration_sec
	  FROM filtered_sessions fs
	  LEFT JOIN machines m ON m.id = fs.machine_id
	 GROUP BY fs.machine_id, m.name
	 ORDER BY session_count DESC, machine_name, machine_id
	 LIMIT 50
),
technician_stats AS MATERIALIZED (
	SELECT COALESCE(fs.user_id::text, '') AS user_id,
	       COALESCE(u.name, '(deletado)') AS user_name,
	       COUNT(*) AS session_count,
	       COALESCE(SUM(fs.effective_duration_sec), 0) AS total_duration_sec
	  FROM filtered_sessions fs
	  LEFT JOIN users u ON u.id = fs.user_id
	 GROUP BY fs.user_id, u.name
	 ORDER BY session_count DESC, user_name, user_id
	 LIMIT 50
),
daily_stats AS MATERIALIZED (
	SELECT TO_CHAR((fs.started_at AT TIME ZONE $4)::date, 'YYYY-MM-DD') AS day,
	       COUNT(*) AS session_count,
	       COALESCE(SUM(fs.effective_duration_sec), 0) AS total_duration_sec
	  FROM filtered_sessions fs
	 GROUP BY (fs.started_at AT TIME ZONE $4)::date
	 ORDER BY (fs.started_at AT TIME ZONE $4)::date
)
SELECT s.total_sessions,
       s.total_duration_sec,
       s.avg_duration_sec,
       COALESCE((
	       SELECT json_agg(
	           json_build_object(
	               'machine_id', ms.machine_id,
	               'machine_name', ms.machine_name,
	               'session_count', ms.session_count,
	               'total_duration_sec', ms.total_duration_sec
	           )
	           ORDER BY ms.session_count DESC, ms.machine_name, ms.machine_id
	       )
	         FROM machine_stats ms
       ), '[]'::json) AS by_machine,
       COALESCE((
	       SELECT json_agg(
	           json_build_object(
	               'user_id', ts.user_id,
	               'user_name', ts.user_name,
	               'session_count', ts.session_count,
	               'total_duration_sec', ts.total_duration_sec
	           )
	           ORDER BY ts.session_count DESC, ts.user_name, ts.user_id
	       )
	         FROM technician_stats ts
       ), '[]'::json) AS by_technician,
       COALESCE((
	       SELECT json_agg(
	           json_build_object(
	               'day', ds.day,
	               'session_count', ds.session_count,
	               'total_duration_sec', ds.total_duration_sec
	           )
	           ORDER BY ds.day
	       )
	         FROM daily_stats ds
       ), '[]'::json) AS daily
  FROM summary s
`

type dashboardQuerier interface {
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
}

// DashboardData agrega estatisticas de sessoes para uma empresa.
type DashboardData struct {
	TotalSessions    int               `json:"total_sessions"`
	TotalDurationSec int               `json:"total_duration_sec"`
	AvgDurationSec   int               `json:"avg_duration_sec"`
	ByMachine        []MachineStats    `json:"by_machine"`
	ByTechnician     []TechnicianStats `json:"by_technician"`
	Daily            []DailyStats      `json:"daily"`
}

// MachineStats agrega sessoes por maquina.
type MachineStats struct {
	MachineID        string `json:"machine_id"`
	MachineName      string `json:"machine_name"`
	SessionCount     int    `json:"session_count"`
	TotalDurationSec int    `json:"total_duration_sec"`
}

// TechnicianStats agrega sessoes por tecnico.
type TechnicianStats struct {
	UserID           string `json:"user_id"`
	UserName         string `json:"user_name"`
	SessionCount     int    `json:"session_count"`
	TotalDurationSec int    `json:"total_duration_sec"`
}

// DailyStats agrega sessoes por dia.
type DailyStats struct {
	Day              string `json:"day"`
	SessionCount     int    `json:"session_count"`
	TotalDurationSec int    `json:"total_duration_sec"`
}

// Repository acessa o banco para relatorios.
type Repository struct {
	db dashboardQuerier
}

// NewRepository cria um novo repository.
func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

// GetDashboard retorna estatisticas agregadas de sessoes.
func (r *Repository) GetDashboard(ctx context.Context, companyID, dateFrom, dateTo, tz string) (*DashboardData, error) {
	data := &DashboardData{}

	var byMachineJSON []byte
	var byTechnicianJSON []byte
	var dailyJSON []byte

	err := r.db.QueryRow(ctx, dashboardQuery, companyID, dateFrom, dateTo, tz).Scan(
		&data.TotalSessions,
		&data.TotalDurationSec,
		&data.AvgDurationSec,
		&byMachineJSON,
		&byTechnicianJSON,
		&dailyJSON,
	)
	if err != nil {
		return nil, err
	}

	if err := decodeDashboardSlice(byMachineJSON, &data.ByMachine); err != nil {
		return nil, fmt.Errorf("decode by_machine: %w", err)
	}
	if err := decodeDashboardSlice(byTechnicianJSON, &data.ByTechnician); err != nil {
		return nil, fmt.Errorf("decode by_technician: %w", err)
	}
	if err := decodeDashboardSlice(dailyJSON, &data.Daily); err != nil {
		return nil, fmt.Errorf("decode daily: %w", err)
	}

	return data, nil
}

func decodeDashboardSlice[T any](raw []byte, dest *[]T) error {
	if len(raw) == 0 {
		*dest = []T{}
		return nil
	}
	if err := json.Unmarshal(raw, dest); err != nil {
		return err
	}
	if *dest == nil {
		*dest = []T{}
	}
	return nil
}
