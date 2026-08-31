package sessions

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

const (
	startMachineLookupQuery = `SELECT COALESCE(managers_only_access, FALSE),
		        COALESCE(require_client_consent, FALSE)
		   FROM machines
		  WHERE id = $1
		    AND company_id = $2`
	companyConcurrentLimitQuery = `SELECT concurrent_session_limit FROM companies WHERE id = $1`
	activeSessionCountQuery     = `SELECT COUNT(*) FROM sessions
			  WHERE company_id = $1
			    AND status IN ('active', 'pending_consent')`
	startInsertQuery = `INSERT INTO sessions (user_id, company_id, machine_id, ip_viewer, ip_host, access_mode, status)
		 SELECT $1, $2, m.id, $4::inet, $5::inet, $6, $7
		   FROM machines m
		  WHERE m.id = $3
		    AND m.company_id = $2
		 RETURNING id, user_id, company_id, machine_id, access_mode, status, started_at, ended_at, duration_sec, ip_viewer, ip_host,
		           COALESCE((SELECT name FROM users WHERE id = user_id), '')`
)

type sessionQueryRowExpectation struct {
	sql  string
	args []any
	row  pgx.Row
}

type fakeSessionDB struct {
	t                    *testing.T
	queryRowExpectations []sessionQueryRowExpectation
}

func (f *fakeSessionDB) Query(context.Context, string, ...any) (pgx.Rows, error) {
	f.t.Helper()
	f.t.Fatal("unexpected Query call")
	return nil, nil
}

func (f *fakeSessionDB) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	f.t.Helper()
	if len(f.queryRowExpectations) == 0 {
		f.t.Fatalf("unexpected QueryRow call:\n%s\nargs=%v", sql, args)
	}

	expected := f.queryRowExpectations[0]
	f.queryRowExpectations = f.queryRowExpectations[1:]

	if normalizeSQL(sql) != normalizeSQL(expected.sql) {
		f.t.Fatalf("unexpected QueryRow SQL:\n got: %s\nwant: %s", normalizeSQL(sql), normalizeSQL(expected.sql))
	}
	if !reflect.DeepEqual(args, expected.args) {
		f.t.Fatalf("unexpected QueryRow args: got %v want %v", args, expected.args)
	}
	return expected.row
}

func (f *fakeSessionDB) Exec(context.Context, string, ...any) (pgconn.CommandTag, error) {
	f.t.Helper()
	f.t.Fatal("unexpected Exec call")
	return pgconn.NewCommandTag("UPDATE 0"), nil
}

func (f *fakeSessionDB) assertExhausted() {
	f.t.Helper()
	if len(f.queryRowExpectations) != 0 {
		f.t.Fatalf("unconsumed QueryRow expectations: %d", len(f.queryRowExpectations))
	}
}

type fakeSessionRow struct {
	values []any
	err    error
}

func (r fakeSessionRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	return assignScan(dest, r.values)
}

type machineLimitCallExpectation struct {
	companyID string
	machineID string
	state     *machinelimit.MachineState
	err       error
}

type fakeMachineLimitEvaluator struct {
	t            *testing.T
	expectations []machineLimitCallExpectation
}

func (f *fakeMachineLimitEvaluator) GetMachineState(_ context.Context, companyID, machineID string) (*machinelimit.MachineState, error) {
	f.t.Helper()
	if len(f.expectations) == 0 {
		f.t.Fatalf("unexpected GetMachineState call: company=%q machine=%q", companyID, machineID)
	}

	expected := f.expectations[0]
	f.expectations = f.expectations[1:]
	if companyID != expected.companyID || machineID != expected.machineID {
		f.t.Fatalf("unexpected GetMachineState args: got company=%q machine=%q want company=%q machine=%q",
			companyID, machineID, expected.companyID, expected.machineID)
	}
	return expected.state, expected.err
}

func (f *fakeMachineLimitEvaluator) assertExhausted() {
	f.t.Helper()
	if len(f.expectations) != 0 {
		f.t.Fatalf("unconsumed machine limit expectations: %d", len(f.expectations))
	}
}

func normalizeSQL(sql string) string {
	return strings.Join(strings.Fields(sql), " ")
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

func sessionTestStringPtr(value string) *string {
	return &value
}

func allowedMachineState(companyID, machineID string) *machinelimit.MachineState {
	return &machinelimit.MachineState{
		MachineID:   machineID,
		CompanyID:   companyID,
		CreatedAt:   time.Date(2026, 4, 8, 0, 0, 0, 0, time.UTC),
		State:       machinelimit.StateAllowed,
		BlockReason: machinelimit.BlockReasonNone,
	}
}

func blockedMachineState(companyID, machineID string) *machinelimit.MachineState {
	return &machinelimit.MachineState{
		MachineID:   machineID,
		CompanyID:   companyID,
		CreatedAt:   time.Date(2026, 4, 8, 0, 0, 0, 0, time.UTC),
		State:       machinelimit.StateBlocked,
		BlockReason: machinelimit.BlockReasonMachineLimitExceeded,
	}
}

func TestNormalizeAccessMode(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		input   string
		want    string
		wantErr bool
	}{
		{name: "default interactive", input: "", want: AccessModeInteractive},
		{name: "interactive explicit", input: "interactive", want: AccessModeInteractive},
		{name: "watch only explicit", input: "watch_only", want: AccessModeWatchOnly},
		{name: "trim and lower", input: "  WATCH_ONLY  ", want: AccessModeWatchOnly},
		{name: "invalid", input: "observe", wantErr: true},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := normalizeAccessMode(tc.input)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("expected error, got mode %q", got)
				}
				return
			}

			if err != nil {
				t.Fatalf("normalizeAccessMode returned error: %v", err)
			}
			if got != tc.want {
				t.Fatalf("normalizeAccessMode(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

func TestNormalizeConsentDecision(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		input   string
		want    string
		wantErr bool
	}{
		{name: "approved", input: "approved", want: ConsentDecisionApproved},
		{name: "denied uppercase", input: "  DENIED ", want: ConsentDecisionDenied},
		{name: "expired", input: "expired", want: ConsentDecisionExpired},
		{name: "invalid", input: "allow", wantErr: true},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := normalizeConsentDecision(tc.input)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("expected error, got decision %q", got)
				}
				return
			}

			if err != nil {
				t.Fatalf("normalizeConsentDecision returned error: %v", err)
			}
			if got != tc.want {
				t.Fatalf("normalizeConsentDecision(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

func TestAuthorizeManagersOnlyAccess(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name               string
		managersOnlyAccess bool
		isManager          bool
		wantErr            bool
	}{
		{name: "open machine for user", managersOnlyAccess: false, isManager: false},
		{name: "open machine for manager", managersOnlyAccess: false, isManager: true},
		{name: "restricted machine for manager", managersOnlyAccess: true, isManager: true},
		{name: "restricted machine for user", managersOnlyAccess: true, isManager: false, wantErr: true},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			err := authorizeManagersOnlyAccess(tc.managersOnlyAccess, tc.isManager)
			if tc.wantErr {
				if err != ErrManagerAccessRequired {
					t.Fatalf("authorizeManagersOnlyAccess() error = %v, want %v", err, ErrManagerAccessRequired)
				}
				return
			}
			if err != nil {
				t.Fatalf("authorizeManagersOnlyAccess() unexpected error: %v", err)
			}
		})
	}
}

func TestResolveConsentUpdateFlags(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		target      string
		wantStarted bool
		wantEnded   bool
	}{
		{
			name:        "active starts session",
			target:      SessionStatusActive,
			wantStarted: true,
			wantEnded:   false,
		},
		{
			name:        "denied ends session",
			target:      SessionStatusDenied,
			wantStarted: false,
			wantEnded:   true,
		},
		{
			name:        "expired ends session",
			target:      SessionStatusExpired,
			wantStarted: false,
			wantEnded:   true,
		},
		{
			name:        "pending consent does not update timestamps",
			target:      SessionStatusPendingConsent,
			wantStarted: false,
			wantEnded:   false,
		},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			gotStarted, gotEnded := resolveConsentUpdateFlags(tc.target)
			if gotStarted != tc.wantStarted || gotEnded != tc.wantEnded {
				t.Fatalf("resolveConsentUpdateFlags(%q) = (%v, %v), want (%v, %v)",
					tc.target, gotStarted, gotEnded, tc.wantStarted, tc.wantEnded)
			}
		})
	}
}

func TestServiceStartBlockedMachineDeniesBeforeManagerOrConcurrentChecks(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		accessMode string
	}{
		{name: "interactive", accessMode: AccessModeInteractive},
		{name: "watch only", accessMode: AccessModeWatchOnly},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			db := &fakeSessionDB{
				t: t,
				queryRowExpectations: []sessionQueryRowExpectation{
					{
						sql:  startMachineLookupQuery,
						args: []any{"machine-1", "company-1"},
						row:  fakeSessionRow{values: []any{true, false}},
					},
				},
			}
			evaluator := &fakeMachineLimitEvaluator{
				t: t,
				expectations: []machineLimitCallExpectation{{
					companyID: "company-1",
					machineID: "machine-1",
					state:     blockedMachineState("company-1", "machine-1"),
				}},
			}
			svc := NewService(db, nil, evaluator)

			sess, err := svc.Start(context.Background(), "user-1", "company-1", false, StartRequest{
				MachineID:  "machine-1",
				AccessMode: tc.accessMode,
			})
			if !errors.Is(err, ErrMachineBlockedByLimit) {
				t.Fatalf("expected ErrMachineBlockedByLimit for %s, got %v", tc.accessMode, err)
			}
			if errors.Is(err, ErrManagerAccessRequired) {
				t.Fatalf("blocked denial should win before manager-only access for %s, got %v", tc.accessMode, err)
			}
			if sess != nil {
				t.Fatalf("expected no session for %s, got %+v", tc.accessMode, sess)
			}

			db.assertExhausted()
			evaluator.assertExhausted()
		})
	}
}

func TestServiceStartEvaluatorErrorsFailClosed(t *testing.T) {
	t.Parallel()

	db := &fakeSessionDB{
		t: t,
		queryRowExpectations: []sessionQueryRowExpectation{
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{false, false}},
			},
		},
	}
	evaluator := &fakeMachineLimitEvaluator{
		t: t,
		expectations: []machineLimitCallExpectation{{
			companyID: "company-1",
			machineID: "machine-1",
			err:       errors.New("boom"),
		}},
	}
	svc := NewService(db, nil, evaluator)

	sess, err := svc.Start(context.Background(), "user-1", "company-1", true, StartRequest{
		MachineID:  "machine-1",
		AccessMode: AccessModeInteractive,
	})
	if !errors.Is(err, ErrMachineBlockedByLimit) {
		t.Fatalf("expected ErrMachineBlockedByLimit, got %v", err)
	}
	if !strings.Contains(err.Error(), "machine limit lookup failed") {
		t.Fatalf("expected wrapped evaluator failure, got %v", err)
	}
	if sess != nil {
		t.Fatalf("expected no session, got %+v", sess)
	}

	db.assertExhausted()
	evaluator.assertExhausted()
}

func TestServiceStartMissingMachineLimitStateFailsClosed(t *testing.T) {
	t.Parallel()

	db := &fakeSessionDB{
		t: t,
		queryRowExpectations: []sessionQueryRowExpectation{
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{false, false}},
			},
		},
	}
	evaluator := &fakeMachineLimitEvaluator{
		t: t,
		expectations: []machineLimitCallExpectation{{
			companyID: "company-1",
			machineID: "machine-1",
			state:     nil,
		}},
	}
	svc := NewService(db, nil, evaluator)

	sess, err := svc.Start(context.Background(), "user-1", "company-1", true, StartRequest{
		MachineID:  "machine-1",
		AccessMode: AccessModeInteractive,
	})
	if !errors.Is(err, ErrMachineBlockedByLimit) {
		t.Fatalf("expected ErrMachineBlockedByLimit, got %v", err)
	}
	if !strings.Contains(err.Error(), "returned no state") {
		t.Fatalf("expected missing-state failure detail, got %v", err)
	}
	if sess != nil {
		t.Fatalf("expected no session, got %+v", sess)
	}

	db.assertExhausted()
	evaluator.assertExhausted()
}

func TestServiceStartReleasedMachineSucceedsOnNextAttemptWithoutCachedBlockedState(t *testing.T) {
	t.Parallel()

	startedAt := time.Date(2026, 4, 8, 13, 0, 0, 0, time.UTC)
	db := &fakeSessionDB{
		t: t,
		queryRowExpectations: []sessionQueryRowExpectation{
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{false, false}},
			},
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{false, false}},
			},
			{
				sql:  companyConcurrentLimitQuery,
				args: []any{"company-1"},
				row:  fakeSessionRow{values: []any{nil}},
			},
			{
				sql: startInsertQuery,
				args: []any{
					"user-1",
					"company-1",
					"machine-1",
					sessionTestStringPtr("203.0.113.10"),
					sessionTestStringPtr("203.0.113.11"),
					AccessModeInteractive,
					SessionStatusActive,
				},
				row: fakeSessionRow{values: []any{
					"session-1",
					"user-1",
					"company-1",
					"machine-1",
					AccessModeInteractive,
					SessionStatusActive,
					startedAt,
					nil,
					nil,
					"203.0.113.10",
					"203.0.113.11",
					"Operator",
				}},
			},
		},
	}
	evaluator := &fakeMachineLimitEvaluator{
		t: t,
		expectations: []machineLimitCallExpectation{
			{
				companyID: "company-1",
				machineID: "machine-1",
				state:     blockedMachineState("company-1", "machine-1"),
			},
			{
				companyID: "company-1",
				machineID: "machine-1",
				state:     allowedMachineState("company-1", "machine-1"),
			},
		},
	}
	svc := NewService(db, nil, evaluator)
	req := StartRequest{
		MachineID:  "machine-1",
		IPViewer:   "203.0.113.10",
		IPHost:     "203.0.113.11",
		AccessMode: AccessModeInteractive,
	}

	first, err := svc.Start(context.Background(), "user-1", "company-1", true, req)
	if !errors.Is(err, ErrMachineBlockedByLimit) {
		t.Fatalf("expected first attempt to fail closed with ErrMachineBlockedByLimit, got %v", err)
	}
	if first != nil {
		t.Fatalf("expected first attempt to return no session, got %+v", first)
	}

	second, err := svc.Start(context.Background(), "user-1", "company-1", true, req)
	if err != nil {
		t.Fatalf("expected second attempt to succeed, got %v", err)
	}
	if second == nil {
		t.Fatal("expected second attempt session, got nil")
	}
	if second.ID != "session-1" || second.Status != SessionStatusActive {
		t.Fatalf("unexpected session result: %+v", second)
	}

	db.assertExhausted()
	evaluator.assertExhausted()
}

func TestServiceStartAllowedMachineStillEnforcesManagerOnlyAccess(t *testing.T) {
	t.Parallel()

	db := &fakeSessionDB{
		t: t,
		queryRowExpectations: []sessionQueryRowExpectation{
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{true, false}},
			},
		},
	}
	evaluator := &fakeMachineLimitEvaluator{
		t: t,
		expectations: []machineLimitCallExpectation{{
			companyID: "company-1",
			machineID: "machine-1",
			state:     allowedMachineState("company-1", "machine-1"),
		}},
	}
	svc := NewService(db, nil, evaluator)

	sess, err := svc.Start(context.Background(), "user-1", "company-1", false, StartRequest{
		MachineID:  "machine-1",
		AccessMode: AccessModeInteractive,
	})
	if !errors.Is(err, ErrManagerAccessRequired) {
		t.Fatalf("expected ErrManagerAccessRequired, got %v", err)
	}
	if sess != nil {
		t.Fatalf("expected no session, got %+v", sess)
	}

	db.assertExhausted()
	evaluator.assertExhausted()
}

func TestServiceStartAllowedMachineStillEnforcesConcurrentSessionLimit(t *testing.T) {
	t.Parallel()

	db := &fakeSessionDB{
		t: t,
		queryRowExpectations: []sessionQueryRowExpectation{
			{
				sql:  startMachineLookupQuery,
				args: []any{"machine-1", "company-1"},
				row:  fakeSessionRow{values: []any{false, false}},
			},
			{
				sql:  companyConcurrentLimitQuery,
				args: []any{"company-1"},
				row:  fakeSessionRow{values: []any{1}},
			},
			{
				sql:  activeSessionCountQuery,
				args: []any{"company-1"},
				row:  fakeSessionRow{values: []any{1}},
			},
		},
	}
	evaluator := &fakeMachineLimitEvaluator{
		t: t,
		expectations: []machineLimitCallExpectation{{
			companyID: "company-1",
			machineID: "machine-1",
			state:     allowedMachineState("company-1", "machine-1"),
		}},
	}
	svc := NewService(db, nil, evaluator)

	sess, err := svc.Start(context.Background(), "user-1", "company-1", true, StartRequest{
		MachineID:  "machine-1",
		AccessMode: AccessModeInteractive,
	})
	if !errors.Is(err, ErrConcurrentSessionLimit) {
		t.Fatalf("expected ErrConcurrentSessionLimit, got %v", err)
	}
	if sess != nil {
		t.Fatalf("expected no session, got %+v", sess)
	}

	db.assertExhausted()
	evaluator.assertExhausted()
}
