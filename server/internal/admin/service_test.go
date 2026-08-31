package admin

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

type fakeAdminDB struct {
	t                 *testing.T
	expectedQuery     string
	expectedQueryArgs []any
	queryRows         pgx.Rows
	queryErr          error
	expectedRowQuery  string
	expectedRowArgs   []any
	queryRow          pgx.Row
}

func (f *fakeAdminDB) Query(_ context.Context, sql string, args ...any) (pgx.Rows, error) {
	f.t.Helper()
	if strings.TrimSpace(sql) != strings.TrimSpace(f.expectedQuery) {
		f.t.Fatalf("unexpected query:\n%s", sql)
	}
	if !reflect.DeepEqual(args, f.expectedQueryArgs) {
		f.t.Fatalf("unexpected query args: got %v want %v", args, f.expectedQueryArgs)
	}
	return f.queryRows, f.queryErr
}

func (f *fakeAdminDB) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	f.t.Helper()
	if strings.TrimSpace(sql) != strings.TrimSpace(f.expectedRowQuery) {
		f.t.Fatalf("unexpected row query:\n%s", sql)
	}
	if !reflect.DeepEqual(args, f.expectedRowArgs) {
		f.t.Fatalf("unexpected row args: got %v want %v", args, f.expectedRowArgs)
	}
	return f.queryRow
}

func (f *fakeAdminDB) Exec(context.Context, string, ...any) (pgconn.CommandTag, error) {
	return pgconn.NewCommandTag("UPDATE 0"), nil
}

type fakeRows struct {
	values [][]any
	idx    int
	err    error
}

func (r *fakeRows) Close()                                       {}
func (r *fakeRows) Err() error                                   { return r.err }
func (r *fakeRows) CommandTag() pgconn.CommandTag                { return pgconn.NewCommandTag("SELECT 0") }
func (r *fakeRows) FieldDescriptions() []pgconn.FieldDescription { return nil }
func (r *fakeRows) Next() bool {
	if r.idx >= len(r.values) {
		return false
	}
	r.idx++
	return true
}
func (r *fakeRows) Scan(dest ...any) error {
	if r.idx == 0 || r.idx > len(r.values) {
		return errors.New("Scan called without a current row")
	}
	return assignScan(dest, r.values[r.idx-1])
}
func (r *fakeRows) Values() ([]any, error) {
	if r.idx == 0 || r.idx > len(r.values) {
		return nil, errors.New("Values called without a current row")
	}
	return r.values[r.idx-1], nil
}
func (r *fakeRows) RawValues() [][]byte { return nil }
func (r *fakeRows) Conn() *pgx.Conn     { return nil }

type fakeRow struct {
	values []any
	err    error
}

func (r fakeRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	return assignScan(dest, r.values)
}

func assignScan(dest []any, values []any) error {
	if len(dest) != len(values) {
		return fmt.Errorf("scan arg mismatch: got %d destinations for %d values", len(dest), len(values))
	}
	for i := range dest {
		if err := assignValue(dest[i], values[i]); err != nil {
			return fmt.Errorf("assign column %d: %w", i, err)
		}
	}
	return nil
}

func assignValue(dest any, value any) error {
	rv := reflect.ValueOf(dest)
	if rv.Kind() != reflect.Pointer || rv.IsNil() {
		return errors.New("destination must be a non-nil pointer")
	}

	target := rv.Elem()
	if value == nil {
		target.Set(reflect.Zero(target.Type()))
		return nil
	}

	valueRef := reflect.ValueOf(value)
	if target.Kind() == reflect.Pointer {
		ptr := reflect.New(target.Type().Elem())
		if valueRef.Type().AssignableTo(target.Type().Elem()) {
			ptr.Elem().Set(valueRef)
			target.Set(ptr)
			return nil
		}
		if valueRef.Type().ConvertibleTo(target.Type().Elem()) {
			ptr.Elem().Set(valueRef.Convert(target.Type().Elem()))
			target.Set(ptr)
			return nil
		}
	}

	if valueRef.Type().AssignableTo(target.Type()) {
		target.Set(valueRef)
		return nil
	}
	if valueRef.Type().ConvertibleTo(target.Type()) {
		target.Set(valueRef.Convert(target.Type()))
		return nil
	}

	return fmt.Errorf("cannot assign %T to %s", value, target.Type())
}

type fakeMachineLimitService struct {
	evaluations  map[string]*machinelimit.CompanyEvaluation
	errByCompany map[string]error
	calls        map[string]int
}

func (f *fakeMachineLimitService) EvaluateCompany(_ context.Context, companyID string) (*machinelimit.CompanyEvaluation, error) {
	if f.calls == nil {
		f.calls = make(map[string]int)
	}
	f.calls[companyID]++
	if err, ok := f.errByCompany[companyID]; ok {
		return nil, err
	}
	if evaluation, ok := f.evaluations[companyID]; ok {
		return evaluation, nil
	}
	return nil, machinelimit.ErrCompanyNotFound
}

func TestListCompaniesPreservesAggregatedCounts(t *testing.T) {
	t.Parallel()

	createdAt := time.Date(2026, 3, 1, 12, 0, 0, 0, time.UTC)
	updatedAt := createdAt.Add(2 * time.Hour)
	blockDays := 30
	limit := 5
	machineLimit := 12

	svc := NewService(&fakeAdminDB{
		t:             t,
		expectedQuery: companySummaryListQuery,
		queryRows: &fakeRows{values: [][]any{
			{"company-1", "Alpha", true, createdAt, updatedAt, 30, 5, 12, 2, 3, 1, 2},
			{"company-2", "Beta", false, createdAt, updatedAt, nil, nil, nil, 0, 1, 0, 0},
		}},
	}, nil, nil)

	got, err := svc.ListCompanies(context.Background())
	if err != nil {
		t.Fatalf("ListCompanies returned error: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("expected 2 companies, got %d", len(got))
	}
	if got[0].Name != "Alpha" || got[0].UserCount != 2 || got[0].MachineCount != 3 || got[0].OnlineMachineCount != 1 || got[0].ActiveSessionCount != 2 {
		t.Fatalf("unexpected first company: %+v", got[0])
	}
	if got[0].BlockAfterDays == nil || *got[0].BlockAfterDays != blockDays {
		t.Fatalf("expected block_after_days=%d, got %+v", blockDays, got[0].BlockAfterDays)
	}
	if got[0].ConcurrentSessionLimit == nil || *got[0].ConcurrentSessionLimit != limit {
		t.Fatalf("expected concurrent_session_limit=%d, got %+v", limit, got[0].ConcurrentSessionLimit)
	}
	if got[0].MachineLimit == nil || *got[0].MachineLimit != machineLimit {
		t.Fatalf("expected machine_limit=%d, got %+v", machineLimit, got[0].MachineLimit)
	}
	if got[1].BlockAfterDays != nil || got[1].ConcurrentSessionLimit != nil || got[1].MachineLimit != nil {
		t.Fatalf("expected nil optional limits, got %+v", got[1])
	}
}

func TestGetCompanyDetailUnknownCompanyFailsClosed(t *testing.T) {
	t.Parallel()

	svc := NewService(&fakeAdminDB{
		t:                t,
		expectedRowQuery: companySummaryDetailQuery,
		expectedRowArgs:  []any{"missing-company"},
		queryRow:         fakeRow{err: pgx.ErrNoRows},
	}, nil, nil)

	_, err := svc.GetCompanyDetail(context.Background(), "missing-company")
	if !errors.Is(err, ErrCompanyNotFound) {
		t.Fatalf("expected ErrCompanyNotFound, got %v", err)
	}
}

func TestGetDashboardStatsPreservesMixedSessionCounts(t *testing.T) {
	t.Parallel()

	svc := NewService(&fakeAdminDB{
		t:                t,
		expectedRowQuery: dashboardStatsQuery,
		queryRow: fakeRow{values: []any{
			4,
			3,
			1,
			8,
			6,
			2,
			5,
			2,
		}},
	}, nil, nil)

	got, err := svc.GetDashboardStats(context.Background())
	if err != nil {
		t.Fatalf("GetDashboardStats returned error: %v", err)
	}
	if got.TotalCompanies != 4 || got.ActiveCompanies != 3 || got.BlockedCompanies != 1 {
		t.Fatalf("unexpected company counts: %+v", got)
	}
	if got.TotalUsers != 8 || got.TotalMachines != 6 || got.OnlineMachines != 2 {
		t.Fatalf("unexpected user/machine counts: %+v", got)
	}
	if got.SessionsToday != 5 || got.ActiveSessions != 2 {
		t.Fatalf("unexpected session counts: %+v", got)
	}
}

func TestSearchMachinesGroupsCompanyEvaluationAndAnnotatesBlockedState(t *testing.T) {
	t.Parallel()

	now := time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC)
	heartbeat := now.Add(-2 * time.Minute)
	limitSvc := &fakeMachineLimitService{
		evaluations: map[string]*machinelimit.CompanyEvaluation{
			"company-1": {
				CompanyID: "company-1",
				Machines: []machinelimit.MachineState{
					{MachineID: "machine-1", CompanyID: "company-1", State: machinelimit.StateAllowed},
					{MachineID: "machine-2", CompanyID: "company-1", State: machinelimit.StateBlocked, BlockReason: machinelimit.BlockReasonMachineLimitExceeded},
				},
			},
			"company-2": {
				CompanyID: "company-2",
				Machines: []machinelimit.MachineState{
					{MachineID: "machine-3", CompanyID: "company-2", State: machinelimit.StateAllowed},
				},
			},
		},
	}

	svc := NewService(&fakeAdminDB{
		t:                 t,
		expectedQueryArgs: []any{"host", "%host%", 100},
		expectedQuery: `SELECT m.id,
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
		queryRows: &fakeRows{values: [][]any{
			{"machine-1", "Host A", "key-a", "company-1", "Empresa A", true, &heartbeat, now},
			{"machine-2", "Host B", "key-b", "company-1", "Empresa A", true, nil, now.Add(-1 * time.Minute)},
			{"machine-3", "Host C", "key-c", "company-2", "Empresa B", false, nil, now.Add(-2 * time.Minute)},
			{"machine-detached", "Host Solto", "key-detached", "", "Sem empresa", false, nil, now.Add(-3 * time.Minute)},
		}},
	}, nil, limitSvc)

	machines, err := svc.SearchMachines(context.Background(), "host", 0)
	if err != nil {
		t.Fatalf("SearchMachines returned error: %v", err)
	}
	if len(machines) != 4 {
		t.Fatalf("len(machines) = %d, want 4", len(machines))
	}
	if limitSvc.calls["company-1"] != 1 || limitSvc.calls["company-2"] != 1 {
		t.Fatalf("evaluation calls = %+v, want one call per company", limitSvc.calls)
	}
	if _, ok := limitSvc.calls[""]; ok {
		t.Fatalf("detached machines should not trigger evaluator calls: %+v", limitSvc.calls)
	}
	if machines[0].State != machinelimit.StateAllowed || machines[0].BlockReason != machinelimit.BlockReasonNone {
		t.Fatalf("machine-1 state = %q/%q, want allowed/empty", machines[0].State, machines[0].BlockReason)
	}
	if machines[1].State != machinelimit.StateBlocked || machines[1].BlockReason != machinelimit.BlockReasonMachineLimitExceeded {
		t.Fatalf("machine-2 state = %q/%q, want blocked/machine_limit_exceeded", machines[1].State, machines[1].BlockReason)
	}
	if machines[3].State != "" || machines[3].BlockReason != "" {
		t.Fatalf("detached machine annotation = %q/%q, want empty state and reason", machines[3].State, machines[3].BlockReason)
	}
}

func TestSearchMachinesFailsClosedWhenEvaluationMissesReturnedMachine(t *testing.T) {
	t.Parallel()

	limitSvc := &fakeMachineLimitService{
		evaluations: map[string]*machinelimit.CompanyEvaluation{
			"company-1": {
				CompanyID: "company-1",
				Machines:  []machinelimit.MachineState{{MachineID: "other-machine", CompanyID: "company-1", State: machinelimit.StateAllowed}},
			},
		},
	}

	svc := NewService(&fakeAdminDB{
		t:                 t,
		expectedQueryArgs: []any{"host", "%host%", 100},
		expectedQuery: `SELECT m.id,
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
		queryRows: &fakeRows{values: [][]any{{"machine-1", "Host A", "key-a", "company-1", "Empresa A", true, nil, time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC)}}},
	}, nil, limitSvc)

	_, err := svc.SearchMachines(context.Background(), "host", 100)
	if err == nil {
		t.Fatal("expected SearchMachines to fail when evaluation misses a returned machine")
	}
	if !strings.Contains(err.Error(), `machine "machine-1" missing from company "company-1" evaluation`) {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestSearchMachinesAllowsEmptyQueryAndDetachedRowsWithoutEvaluator(t *testing.T) {
	t.Parallel()

	svc := NewService(&fakeAdminDB{
		t:                 t,
		expectedQueryArgs: []any{"orfao", "%orfao%", 100},
		expectedQuery: `SELECT m.id,
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
		queryRows: &fakeRows{values: [][]any{{"machine-detached", "Host Solto", "key-detached", "", "Sem empresa", false, nil, time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC)}}},
	}, nil, nil)

	empty, err := svc.SearchMachines(context.Background(), "   ", 100)
	if err != nil {
		t.Fatalf("SearchMachines blank query returned error: %v", err)
	}
	if len(empty) != 0 {
		t.Fatalf("len(empty) = %d, want 0", len(empty))
	}

	machines, err := svc.SearchMachines(context.Background(), "orfao", 100)
	if err != nil {
		t.Fatalf("SearchMachines detached row returned error: %v", err)
	}
	if len(machines) != 1 {
		t.Fatalf("len(machines) = %d, want 1", len(machines))
	}
	if machines[0].CompanyName != "Sem empresa" || machines[0].CanDetach() {
		t.Fatalf("detached machine = %+v, want detached semantics preserved", machines[0])
	}
	if machines[0].State != "" || machines[0].BlockReason != "" {
		t.Fatalf("detached machine annotation = %q/%q, want empty state and reason", machines[0].State, machines[0].BlockReason)
	}
}

func TestCompanyAndDashboardQueriesUseAggregatedSessionShapes(t *testing.T) {
	t.Parallel()

	if strings.Contains(companySummaryBaseQuery, "COUNT(DISTINCT") {
		t.Fatalf("companySummaryBaseQuery should not use COUNT(DISTINCT):\n%s", companySummaryBaseQuery)
	}
	if !strings.Contains(companySummaryBaseQuery, "active_session_counts AS MATERIALIZED") {
		t.Fatalf("companySummaryBaseQuery is missing the active session aggregate CTE")
	}
	if strings.Count(companySummaryBaseQuery, "FROM sessions") != 1 {
		t.Fatalf("companySummaryBaseQuery should scan sessions once, got %d scans", strings.Count(companySummaryBaseQuery, "FROM sessions"))
	}
	if strings.Count(dashboardStatsQuery, "FROM sessions") != 1 {
		t.Fatalf("dashboardStatsQuery should scan sessions once, got %d scans", strings.Count(dashboardStatsQuery, "FROM sessions"))
	}
}

func TestSessionIndexesStayAlignedAcrossBootstrapPaths(t *testing.T) {
	t.Parallel()

	_, currentFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}

	bootstrapPath := filepath.Join(filepath.Dir(currentFile), "..", "bootstrap", "bootstrap.go")
	initSQLPath := filepath.Join(filepath.Dir(currentFile), "..", "..", "..", "deploy", "postgres", "init.sql")

	bootstrapContent, err := os.ReadFile(bootstrapPath)
	if err != nil {
		t.Fatalf("read bootstrap.go: %v", err)
	}
	initSQLContent, err := os.ReadFile(initSQLPath)
	if err != nil {
		t.Fatalf("read init.sql: %v", err)
	}

	requiredIndexes := []string{
		"idx_sessions_company_started_at_report ON sessions(company_id, started_at DESC) WHERE started_at IS NOT NULL AND status IN ('active', 'ended')",
		"idx_sessions_started_at_dashboard ON sessions(started_at DESC) WHERE started_at IS NOT NULL",
	}

	for _, indexSpec := range requiredIndexes {
		if !strings.Contains(string(bootstrapContent), indexSpec) {
			t.Fatalf("bootstrap.go is missing index %q", indexSpec)
		}
		if !strings.Contains(string(initSQLContent), indexSpec) {
			t.Fatalf("init.sql is missing index %q", indexSpec)
		}
	}
}
