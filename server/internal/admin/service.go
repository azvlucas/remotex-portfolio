package admin

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

var (
	ErrCompanyNotFound = errors.New("company not found")
	ErrUserNotFound    = errors.New("user not found")
	ErrMachineNotFound = errors.New("machine not found")
	ErrDuplicateName   = errors.New("duplicate name")
	ErrDuplicateEmail  = errors.New("duplicate email")
)

type CompanySummary struct {
	ID                     string    `json:"id"`
	Name                   string    `json:"name"`
	IsActive               bool      `json:"is_active"`
	UserCount              int       `json:"user_count"`
	MachineCount           int       `json:"machine_count"`
	OnlineMachineCount     int       `json:"online_machine_count"`
	ActiveSessionCount     int       `json:"active_session_count"`
	BlockAfterDays         *int      `json:"block_after_days"`
	ConcurrentSessionLimit *int      `json:"concurrent_session_limit"`
	MachineLimit           *int      `json:"machine_limit"`
	CreatedAt              time.Time `json:"created_at"`
	UpdatedAt              time.Time `json:"updated_at"`
}

func (c CompanySummary) BlockAfterDaysValue() int {
	if c.BlockAfterDays != nil {
		return *c.BlockAfterDays
	}
	return 0
}

func (c CompanySummary) ConcurrentSessionLimitValue() int {
	if c.ConcurrentSessionLimit != nil {
		return *c.ConcurrentSessionLimit
	}
	return 0
}

func (c CompanySummary) MachineLimitValue() int {
	if c.MachineLimit != nil {
		return *c.MachineLimit
	}
	return 0
}

func (c CompanySummary) ActiveSessionsLabel() string {
	if c.ConcurrentSessionLimit != nil && *c.ConcurrentSessionLimit > 0 {
		return fmt.Sprintf("%d/%d", c.ActiveSessionCount, *c.ConcurrentSessionLimit)
	}
	return fmt.Sprintf("%d", c.ActiveSessionCount)
}

func (c CompanySummary) ExpiryDate() string {
	if c.BlockAfterDays == nil {
		return ""
	}
	return c.CreatedAt.AddDate(0, 0, *c.BlockAfterDays).Format("02/01/2006")
}

type DashboardStats struct {
	TotalCompanies   int
	ActiveCompanies  int
	BlockedCompanies int
	TotalUsers       int
	TotalMachines    int
	OnlineMachines   int
	SessionsToday    int
	ActiveSessions   int
}

type CompanyUser struct {
	ID        string    `json:"id"`
	CompanyID string    `json:"company_id"`
	Name      string    `json:"name"`
	Email     string    `json:"email"`
	Role      auth.Role `json:"role"`
	IsActive  bool      `json:"is_active"`
	CreatedAt time.Time `json:"created_at"`
}

type CompanyDetail struct {
	Company CompanySummary `json:"company"`
	Users   []CompanyUser  `json:"users"`
}

type MachineSummary struct {
	ID            string                   `json:"id"`
	Name          string                   `json:"name"`
	MachineKey    string                   `json:"machine_key"`
	CompanyID     string                   `json:"company_id"`
	CompanyName   string                   `json:"company_name"`
	IsOnline      bool                     `json:"is_online"`
	LastHeartbeat *time.Time               `json:"last_heartbeat,omitempty"`
	UpdatedAt     time.Time                `json:"updated_at"`
	State         machinelimit.State       `json:"state,omitempty"`
	BlockReason   machinelimit.BlockReason `json:"block_reason,omitempty"`
}

func (m MachineSummary) LastHeartbeatLabel() string {
	if m.LastHeartbeat == nil {
		return "Nunca"
	}
	return m.LastHeartbeat.Local().Format("02/01/2006 15:04")
}

func (m MachineSummary) UpdatedAtLabel() string {
	return m.UpdatedAt.Local().Format("02/01/2006 15:04")
}

func (m MachineSummary) CanDetach() bool {
	return m.CompanyID != ""
}

func (m MachineSummary) IsBlocked() bool {
	return m.State == machinelimit.StateBlocked
}

func (m MachineSummary) StatusLabel() string {
	if m.IsBlocked() {
		return "Bloqueada"
	}
	return m.ConnectivityLabel()
}

func (m MachineSummary) StatusPillClass() string {
	if m.IsBlocked() {
		return "red"
	}
	return m.ConnectivityPillClass()
}

func (m MachineSummary) ConnectivityLabel() string {
	if m.IsOnline {
		return "Online"
	}
	return "Offline"
}

func (m MachineSummary) ConnectivityPillClass() string {
	if m.IsOnline {
		return "green"
	}
	return "red"
}

func (m MachineSummary) BlockReasonLabel() string {
	switch m.BlockReason {
	case machinelimit.BlockReasonMachineLimitExceeded:
		return "Limite de maquinas excedido"
	case machinelimit.BlockReasonNone:
		return ""
	default:
		return string(m.BlockReason)
	}
}

type adminStore interface {
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
}

type machineLimitEvaluator interface {
	EvaluateCompany(ctx context.Context, companyID string) (*machinelimit.CompanyEvaluation, error)
}

type Service struct {
	db              adminStore
	authSvc         *auth.Service
	machineLimitSvc machineLimitEvaluator
}

func NewService(db adminStore, authSvc *auth.Service, machineLimitSvc machineLimitEvaluator) *Service {
	return &Service{db: db, authSvc: authSvc, machineLimitSvc: machineLimitSvc}
}

const companySummaryBaseQuery = `
WITH user_counts AS MATERIALIZED (
	SELECT company_id,
	       COUNT(*) FILTER (WHERE role <> 'super_admin') AS user_count
	  FROM users
	 GROUP BY company_id
),
machine_counts AS MATERIALIZED (
	SELECT company_id,
	       COUNT(*) AS machine_count,
	       COUNT(*) FILTER (WHERE is_online = TRUE) AS online_machine_count
	  FROM machines
	 GROUP BY company_id
),
active_session_counts AS MATERIALIZED (
	SELECT company_id,
	       COUNT(*) AS active_session_count
	  FROM sessions
	 WHERE status IN ('active', 'pending_consent')
	 GROUP BY company_id
)
SELECT c.id,
       c.name,
       c.is_active,
       c.created_at,
       c.updated_at,
       c.block_after_days,
       c.concurrent_session_limit,
       c.machine_limit,
       COALESCE(uc.user_count, 0) AS user_count,
       COALESCE(mc.machine_count, 0) AS machine_count,
       COALESCE(mc.online_machine_count, 0) AS online_machine_count,
       COALESCE(sc.active_session_count, 0) AS active_session_count
  FROM companies c
  LEFT JOIN user_counts uc ON uc.company_id = c.id
  LEFT JOIN machine_counts mc ON mc.company_id = c.id
  LEFT JOIN active_session_counts sc ON sc.company_id = c.id`

const companySummaryListQuery = companySummaryBaseQuery + `
 ORDER BY c.name`

const companySummaryDetailQuery = companySummaryBaseQuery + `
 WHERE c.id = $1`

const dashboardStatsQuery = `
WITH company_counts AS MATERIALIZED (
	SELECT COUNT(*) AS total_companies,
	       COUNT(*) FILTER (WHERE is_active = TRUE) AS active_companies,
	       COUNT(*) FILTER (WHERE is_active = FALSE) AS blocked_companies
	  FROM companies
),
user_counts AS MATERIALIZED (
	SELECT COUNT(*) AS total_users
	  FROM users
	 WHERE role <> 'super_admin'
),
machine_counts AS MATERIALIZED (
	SELECT COUNT(*) AS total_machines,
	       COUNT(*) FILTER (WHERE is_online = TRUE) AS online_machines
	  FROM machines
),
session_counts AS MATERIALIZED (
	SELECT COUNT(*) FILTER (WHERE started_at >= (CURRENT_DATE AT TIME ZONE 'America/Sao_Paulo')) AS sessions_today,
	       COUNT(*) FILTER (WHERE status IN ('active', 'pending_consent')) AS active_sessions
	  FROM sessions
)
SELECT cc.total_companies,
       cc.active_companies,
       cc.blocked_companies,
       uc.total_users,
       mc.total_machines,
       mc.online_machines,
       sc.sessions_today,
       sc.active_sessions
  FROM company_counts cc
 CROSS JOIN user_counts uc
 CROSS JOIN machine_counts mc
 CROSS JOIN session_counts sc`

func (s *Service) ListCompanies(ctx context.Context) ([]CompanySummary, error) {
	rows, err := s.db.Query(ctx, companySummaryListQuery)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var companies []CompanySummary
	for rows.Next() {
		var item CompanySummary
		if err := rows.Scan(
			&item.ID,
			&item.Name,
			&item.IsActive,
			&item.CreatedAt,
			&item.UpdatedAt,
			&item.BlockAfterDays,
			&item.ConcurrentSessionLimit,
			&item.MachineLimit,
			&item.UserCount,
			&item.MachineCount,
			&item.OnlineMachineCount,
			&item.ActiveSessionCount,
		); err != nil {
			return nil, err
		}
		companies = append(companies, item)
	}
	if companies == nil {
		companies = []CompanySummary{}
	}
	return companies, rows.Err()
}

func (s *Service) GetCompanyDetail(ctx context.Context, companyID string) (*CompanyDetail, error) {
	var detail CompanyDetail
	err := s.db.QueryRow(ctx, companySummaryDetailQuery, companyID).Scan(
		&detail.Company.ID,
		&detail.Company.Name,
		&detail.Company.IsActive,
		&detail.Company.CreatedAt,
		&detail.Company.UpdatedAt,
		&detail.Company.BlockAfterDays,
		&detail.Company.ConcurrentSessionLimit,
		&detail.Company.MachineLimit,
		&detail.Company.UserCount,
		&detail.Company.MachineCount,
		&detail.Company.OnlineMachineCount,
		&detail.Company.ActiveSessionCount,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrCompanyNotFound
	}
	if err != nil {
		return nil, err
	}

	users, err := s.listCompanyUsers(ctx, companyID)
	if err != nil {
		return nil, err
	}
	detail.Users = users
	return &detail, nil
}

func (s *Service) SearchMachines(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
	query = strings.TrimSpace(query)
	if query == "" {
		return []MachineSummary{}, nil
	}

	if limit <= 0 || limit > 100 {
		limit = 100
	}

	like := "%" + query + "%"
	rows, err := s.db.Query(ctx,
		`SELECT m.id,
		        m.name,
		        m.machine_key,
		        COALESCE(m.company_id::text, ''),
		        COALESCE(c.name, 'Sem empresa'),
		        m.is_online,
		        m.last_heartbeat,
		        m.updated_at
		   FROM machines m
		   LEFT JOIN companies c ON c.id = m.company_id
		  WHERE m.id::text = $1
		     OR m.name ILIKE $2
		     OR m.machine_key ILIKE $2
		     OR c.name ILIKE $2
		  ORDER BY m.updated_at DESC
		  LIMIT $3`,
		query, like, limit,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var machines []MachineSummary
	for rows.Next() {
		var machine MachineSummary
		if err := rows.Scan(
			&machine.ID,
			&machine.Name,
			&machine.MachineKey,
			&machine.CompanyID,
			&machine.CompanyName,
			&machine.IsOnline,
			&machine.LastHeartbeat,
			&machine.UpdatedAt,
		); err != nil {
			return nil, err
		}
		machines = append(machines, machine)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if machines == nil {
		machines = []MachineSummary{}
	}

	return s.annotateMachineSearchResults(ctx, machines)
}

func (s *Service) annotateMachineSearchResults(ctx context.Context, machines []MachineSummary) ([]MachineSummary, error) {
	if len(machines) == 0 {
		return machines, nil
	}

	machineIndexesByCompany := make(map[string][]int)
	for idx, machine := range machines {
		if !machine.CanDetach() {
			continue
		}
		machineIndexesByCompany[machine.CompanyID] = append(machineIndexesByCompany[machine.CompanyID], idx)
	}
	if len(machineIndexesByCompany) == 0 {
		return machines, nil
	}
	if s.machineLimitSvc == nil {
		return nil, fmt.Errorf("machine limit evaluator unavailable for admin search annotation")
	}

	companyIDs := make([]string, 0, len(machineIndexesByCompany))
	for companyID := range machineIndexesByCompany {
		companyIDs = append(companyIDs, companyID)
	}
	sort.Strings(companyIDs)

	for _, companyID := range companyIDs {
		evaluation, err := s.machineLimitSvc.EvaluateCompany(ctx, companyID)
		if err != nil {
			return nil, fmt.Errorf("evaluate company %q for machine search: %w", companyID, err)
		}
		if evaluation == nil {
			return nil, fmt.Errorf("evaluate company %q for machine search: nil evaluation", companyID)
		}

		statesByID := make(map[string]machinelimit.MachineState, len(evaluation.Machines))
		for _, state := range evaluation.Machines {
			statesByID[state.MachineID] = state
		}

		for _, idx := range machineIndexesByCompany[companyID] {
			state, ok := statesByID[machines[idx].ID]
			if !ok {
				return nil, fmt.Errorf("machine %q missing from company %q evaluation", machines[idx].ID, companyID)
			}
			machines[idx].State = state.State
			machines[idx].BlockReason = state.BlockReason
		}
	}

	return machines, nil
}

func (s *Service) CreateCompany(ctx context.Context, name string) (*CompanySummary, error) {
	name = strings.TrimSpace(name)
	if name == "" {
		return nil, ErrDuplicateName
	}

	var item CompanySummary
	err := s.db.QueryRow(ctx,
		`INSERT INTO companies (name, is_active)
		 VALUES ($1, TRUE)
		 RETURNING id, name, is_active, created_at, updated_at`,
		name,
	).Scan(&item.ID, &item.Name, &item.IsActive, &item.CreatedAt, &item.UpdatedAt)
	if err != nil {
		if isUniqueViolation(err) {
			return nil, ErrDuplicateName
		}
		return nil, err
	}
	return &item, nil
}

func (s *Service) SetCompanyActive(ctx context.Context, companyID string, active bool) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE companies
		    SET is_active = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		companyID, active,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrCompanyNotFound
	}
	return nil
}

func (s *Service) DetachMachine(ctx context.Context, machineID string) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE machines
		    SET company_id = NULL,
		        user_id = NULL,
		        group_id = NULL,
		        subgroup_id = NULL,
		        last_heartbeat = NULL,
		        is_online = FALSE,
		        updated_at = NOW()
		  WHERE id = $1`,
		strings.TrimSpace(machineID),
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrMachineNotFound
	}
	return nil
}

func (s *Service) CreateCompanyUser(ctx context.Context, companyID, name, email, password string) (*CompanyUser, error) {
	return s.CreateCompanyUserWithRole(ctx, companyID, name, email, password, auth.RoleCompanyUser)
}

func (s *Service) CreateCompanyUserWithRole(ctx context.Context, companyID, name, email, password string, role auth.Role) (*CompanyUser, error) {
	if role != auth.RoleCompanyUser && role != auth.RoleCompanyManager {
		role = auth.RoleCompanyUser
	}

	hash, err := s.authSvc.HashPassword(password)
	if err != nil {
		return nil, err
	}

	var user CompanyUser
	err = s.db.QueryRow(ctx,
		`INSERT INTO users (company_id, email, password_hash, name, role, is_active)
		 SELECT c.id, $2, $3, $4, $5, TRUE
		   FROM companies c
		  WHERE c.id = $1
		 RETURNING id, company_id, name, email, role, is_active, created_at`,
		companyID,
		strings.TrimSpace(strings.ToLower(email)),
		hash,
		strings.TrimSpace(name),
		role,
	).Scan(&user.ID, &user.CompanyID, &user.Name, &user.Email, &user.Role, &user.IsActive, &user.CreatedAt)
	if err != nil {
		if isUniqueViolation(err) {
			return nil, ErrDuplicateEmail
		}
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrCompanyNotFound
		}
		return nil, err
	}
	return &user, nil
}

func (s *Service) SetUserActive(ctx context.Context, userID string, active bool) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE users
		    SET is_active = $2,
		        updated_at = NOW()
		  WHERE id = $1
		    AND role <> 'super_admin'`,
		userID, active,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrUserNotFound
	}
	return nil
}

func (s *Service) UpdateUserRole(ctx context.Context, userID string, role auth.Role) error {
	if role != auth.RoleCompanyUser && role != auth.RoleCompanyManager {
		return errors.New("invalid role")
	}

	tag, err := s.db.Exec(ctx,
		`UPDATE users
		    SET role = $2,
		        updated_at = NOW()
		  WHERE id = $1
		    AND role <> 'super_admin'`,
		userID, role,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrUserNotFound
	}
	return nil
}

func (s *Service) ListCompanyUsers(ctx context.Context, companyID string) ([]CompanyUser, error) {
	return s.listCompanyUsers(ctx, companyID)
}

func (s *Service) listCompanyUsers(ctx context.Context, companyID string) ([]CompanyUser, error) {
	rows, err := s.db.Query(ctx,
		`SELECT id, company_id, name, email, role, is_active, created_at
		   FROM users
		  WHERE company_id = $1
		    AND role <> 'super_admin'
		  ORDER BY email`,
		companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var users []CompanyUser
	for rows.Next() {
		var user CompanyUser
		if err := rows.Scan(&user.ID, &user.CompanyID, &user.Name, &user.Email, &user.Role, &user.IsActive, &user.CreatedAt); err != nil {
			return nil, err
		}
		users = append(users, user)
	}
	if users == nil {
		users = []CompanyUser{}
	}
	return users, rows.Err()
}

func (s *Service) UpdateCompanyBlockDays(ctx context.Context, companyID string, days *int) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE companies
		    SET block_after_days = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		companyID, days,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrCompanyNotFound
	}
	return nil
}

func (s *Service) UpdateConcurrentSessionLimit(ctx context.Context, companyID string, limit *int) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE companies
		    SET concurrent_session_limit = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		companyID, limit,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrCompanyNotFound
	}
	return nil
}

func (s *Service) UpdateMachineLimit(ctx context.Context, companyID string, limit *int) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE companies
		    SET machine_limit = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		companyID, limit,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrCompanyNotFound
	}
	return nil
}

func (s *Service) GetConcurrentSessionLimit(ctx context.Context, companyID string) (*int, error) {
	var limit *int
	err := s.db.QueryRow(ctx,
		`SELECT concurrent_session_limit FROM companies WHERE id = $1`,
		companyID,
	).Scan(&limit)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrCompanyNotFound
	}
	if err != nil {
		return nil, err
	}
	return limit, nil
}

func (s *Service) CountActiveCompanySessions(ctx context.Context, companyID string) (int, error) {
	var count int
	err := s.db.QueryRow(ctx,
		`SELECT COUNT(*) FROM sessions
		  WHERE company_id = $1
		    AND status IN ('active', 'pending_consent')`,
		companyID,
	).Scan(&count)
	return count, err
}

func (s *Service) AutoBlockExpiredCompanies(ctx context.Context) (int, error) {
	tag, err := s.db.Exec(ctx,
		`UPDATE companies
		    SET is_active = FALSE, updated_at = NOW()
		  WHERE is_active = TRUE
		    AND block_after_days IS NOT NULL
		    AND created_at + (block_after_days || ' days')::INTERVAL < NOW()`,
	)
	if err != nil {
		return 0, err
	}
	return int(tag.RowsAffected()), nil
}

func (s *Service) GetDashboardStats(ctx context.Context) (DashboardStats, error) {
	var stats DashboardStats
	err := s.db.QueryRow(ctx, dashboardStatsQuery).Scan(
		&stats.TotalCompanies,
		&stats.ActiveCompanies,
		&stats.BlockedCompanies,
		&stats.TotalUsers,
		&stats.TotalMachines,
		&stats.OnlineMachines,
		&stats.SessionsToday,
		&stats.ActiveSessions,
	)
	return stats, err
}

func isUniqueViolation(err error) bool {
	var pgErr *pgconn.PgError
	return errors.As(err, &pgErr) && pgErr.Code == "23505"
}
