package reports

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

type fakeDashboardDB struct {
	t             *testing.T
	expectedQuery string
	expectedArgs  []any
	row           pgx.Row
}

func (f *fakeDashboardDB) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	f.t.Helper()
	if strings.TrimSpace(sql) != strings.TrimSpace(f.expectedQuery) {
		f.t.Fatalf("unexpected query:\n%s", sql)
	}
	if !reflect.DeepEqual(args, f.expectedArgs) {
		f.t.Fatalf("unexpected args: got %v want %v", args, f.expectedArgs)
	}
	return f.row
}

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

func TestGetDashboardPreservesAggregatesAndBreakdowns(t *testing.T) {
	t.Parallel()

	repo := &Repository{db: &fakeDashboardDB{
		t:             t,
		expectedQuery: dashboardQuery,
		expectedArgs:  []any{"company-1", "2026-03-01", "2026-03-31", "America/Sao_Paulo"},
		row: fakeRow{values: []any{
			3,
			420,
			140,
			[]byte(`[{"machine_id":"machine-1","machine_name":"Workstation","session_count":2,"total_duration_sec":300}]`),
			[]byte(`[{"user_id":"user-1","user_name":"Ana","session_count":2,"total_duration_sec":300}]`),
			[]byte(`[{"day":"2026-03-10","session_count":1,"total_duration_sec":120},{"day":"2026-03-11","session_count":2,"total_duration_sec":300}]`),
		}},
	}}

	got, err := repo.GetDashboard(context.Background(), "company-1", "2026-03-01", "2026-03-31", "America/Sao_Paulo")
	if err != nil {
		t.Fatalf("GetDashboard returned error: %v", err)
	}

	if got.TotalSessions != 3 || got.TotalDurationSec != 420 || got.AvgDurationSec != 140 {
		t.Fatalf("unexpected summary: %+v", got)
	}
	if len(got.ByMachine) != 1 || got.ByMachine[0].MachineName != "Workstation" || got.ByMachine[0].TotalDurationSec != 300 {
		t.Fatalf("unexpected machine stats: %+v", got.ByMachine)
	}
	if len(got.ByTechnician) != 1 || got.ByTechnician[0].UserName != "Ana" || got.ByTechnician[0].SessionCount != 2 {
		t.Fatalf("unexpected technician stats: %+v", got.ByTechnician)
	}
	if len(got.Daily) != 2 || got.Daily[0].Day != "2026-03-10" || got.Daily[1].SessionCount != 2 {
		t.Fatalf("unexpected daily stats: %+v", got.Daily)
	}
}

func TestGetDashboardReturnsDeterministicEmptyAggregates(t *testing.T) {
	t.Parallel()

	repo := &Repository{db: &fakeDashboardDB{
		t:             t,
		expectedQuery: dashboardQuery,
		expectedArgs:  []any{"company-1", "2026-03-20", "2026-03-10", "America/Sao_Paulo"},
		row: fakeRow{values: []any{
			0,
			0,
			0,
			[]byte(`[]`),
			[]byte(`[]`),
			[]byte(`[]`),
		}},
	}}

	got, err := repo.GetDashboard(context.Background(), "company-1", "2026-03-20", "2026-03-10", "America/Sao_Paulo")
	if err != nil {
		t.Fatalf("GetDashboard returned error: %v", err)
	}

	if got.TotalSessions != 0 || got.TotalDurationSec != 0 || got.AvgDurationSec != 0 {
		t.Fatalf("unexpected empty summary: %+v", got)
	}
	if got.ByMachine == nil || len(got.ByMachine) != 0 {
		t.Fatalf("expected empty machine slice, got %+v", got.ByMachine)
	}
	if got.ByTechnician == nil || len(got.ByTechnician) != 0 {
		t.Fatalf("expected empty technician slice, got %+v", got.ByTechnician)
	}
	if got.Daily == nil || len(got.Daily) != 0 {
		t.Fatalf("expected empty daily slice, got %+v", got.Daily)
	}
}

func TestGetDashboardPropagatesRepositoryFailure(t *testing.T) {
	t.Parallel()

	wantErr := errors.New("query failed")
	repo := &Repository{db: &fakeDashboardDB{
		t:             t,
		expectedQuery: dashboardQuery,
		expectedArgs:  []any{"company-1", "2026-03-01", "2026-03-31", "America/Sao_Paulo"},
		row:           fakeRow{err: wantErr},
	}}

	_, err := repo.GetDashboard(context.Background(), "company-1", "2026-03-01", "2026-03-31", "America/Sao_Paulo")
	if !errors.Is(err, wantErr) {
		t.Fatalf("expected %v, got %v", wantErr, err)
	}
}

func TestDashboardQueryReusesSingleFilteredSessionSlice(t *testing.T) {
	t.Parallel()

	if !strings.Contains(dashboardQuery, "WITH filtered_sessions AS MATERIALIZED") {
		t.Fatalf("dashboardQuery is missing the materialized filtered session slice")
	}
	if strings.Count(dashboardQuery, "FROM sessions s") != 1 {
		t.Fatalf("dashboardQuery should scan sessions once, got %d scans", strings.Count(dashboardQuery, "FROM sessions s"))
	}
	if strings.Count(dashboardQuery, "FROM filtered_sessions") != 4 {
		t.Fatalf("dashboardQuery should reuse filtered_sessions four times, got %d", strings.Count(dashboardQuery, "FROM filtered_sessions"))
	}
}
