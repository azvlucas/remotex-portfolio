package machinelimit

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

type fakeRepository struct {
	limit              *int
	limitErr           error
	candidates         []MachineCandidate
	candidatesErr      error
	limitCalls         int
	candidateListCalls int
	lastLimitCompanyID string
	lastListCompanyID  string
}

func (f *fakeRepository) GetCompanyMachineLimit(_ context.Context, companyID string) (*int, error) {
	f.limitCalls++
	f.lastLimitCompanyID = companyID
	if f.limitErr != nil {
		return nil, f.limitErr
	}
	if f.limit == nil {
		return nil, nil
	}
	limit := *f.limit
	return &limit, nil
}

func (f *fakeRepository) ListCompanyMachineCandidates(_ context.Context, companyID string) ([]MachineCandidate, error) {
	f.candidateListCalls++
	f.lastListCompanyID = companyID
	if f.candidatesErr != nil {
		return nil, f.candidatesErr
	}
	if f.candidates == nil {
		return nil, nil
	}
	cloned := make([]MachineCandidate, len(f.candidates))
	copy(cloned, f.candidates)
	return cloned, nil
}

func TestEvaluateCompany_AllowsUnlimitedAndEmptyCompany(t *testing.T) {
	t.Parallel()

	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	zero := 0

	tests := []struct {
		name       string
		limit      *int
		candidates []MachineCandidate
		wantIDs    []string
	}{
		{
			name:  "nil_limit_keeps_all_registered_machines_allowed",
			limit: nil,
			candidates: []MachineCandidate{
				candidate("machine-2", "company-1", base.Add(2*time.Minute)),
				candidate("machine-1", "company-1", base.Add(1*time.Minute)),
			},
			wantIDs: []string{"machine-1", "machine-2"},
		},
		{
			name:  "zero_limit_is_canonical_unlimited",
			limit: &zero,
			candidates: []MachineCandidate{
				candidate("machine-1", "company-1", base),
			},
			wantIDs: []string{"machine-1"},
		},
		{
			name:       "empty_company_returns_empty_state",
			limit:      nil,
			candidates: nil,
			wantIDs:    []string{},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			evaluation, err := evaluateCompany("company-1", tc.limit, tc.candidates)
			if err != nil {
				t.Fatalf("evaluateCompany returned error: %v", err)
			}
			if evaluation.MachineLimit != nil {
				t.Fatalf("MachineLimit = %v, want nil canonical unlimited", evaluation.MachineLimit)
			}
			if evaluation.TotalMachines != len(tc.wantIDs) {
				t.Fatalf("TotalMachines = %d, want %d", evaluation.TotalMachines, len(tc.wantIDs))
			}
			if got := machineIDs(evaluation.Machines); !equalStrings(got, tc.wantIDs) {
				t.Fatalf("machine order = %v, want %v", got, tc.wantIDs)
			}
			if blocked := evaluation.BlockedMachines(); len(blocked) != 0 {
				t.Fatalf("blocked machines = %v, want none", machineIDs(blocked))
			}
			for _, machine := range evaluation.Machines {
				if machine.State != StateAllowed || machine.BlockReason != BlockReasonNone {
					t.Fatalf("machine state = %+v, want allowed without reason", machine)
				}
			}
		})
	}
}

func TestEvaluateCompany_WithinLimitBlocksNone(t *testing.T) {
	t.Parallel()

	limit := 3
	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	evaluation, err := evaluateCompany("company-1", &limit, []MachineCandidate{
		candidate("machine-3", "company-1", base.Add(3*time.Minute)),
		candidate("machine-1", "company-1", base.Add(1*time.Minute)),
		candidate("machine-2", "company-1", base.Add(2*time.Minute)),
	})
	if err != nil {
		t.Fatalf("evaluateCompany returned error: %v", err)
	}

	if got := machineIDs(evaluation.AllowedMachines()); !equalStrings(got, []string{"machine-1", "machine-2", "machine-3"}) {
		t.Fatalf("allowed machines = %v, want [machine-1 machine-2 machine-3]", got)
	}
	if blocked := evaluation.BlockedMachines(); len(blocked) != 0 {
		t.Fatalf("blocked machines = %v, want none", machineIDs(blocked))
	}
}

func TestServiceEvaluateCompany_BlocksNewestExcessMachinesOnly(t *testing.T) {
	t.Parallel()

	limit := 2
	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	repo := &fakeRepository{
		limit: &limit,
		candidates: []MachineCandidate{
			candidate("machine-3", "company-1", base.Add(3*time.Minute)),
			candidate("machine-1", "company-1", base.Add(1*time.Minute)),
			candidate("machine-4", "company-1", base.Add(4*time.Minute)),
			candidate("machine-2", "company-1", base.Add(2*time.Minute)),
		},
	}

	svc := NewService(repo)
	evaluation, err := svc.EvaluateCompany(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("EvaluateCompany returned error: %v", err)
	}
	if repo.limitCalls != 1 || repo.candidateListCalls != 1 {
		t.Fatalf("repo calls = limit:%d candidates:%d, want 1 each", repo.limitCalls, repo.candidateListCalls)
	}
	if repo.lastLimitCompanyID != "company-1" || repo.lastListCompanyID != "company-1" {
		t.Fatalf("unexpected company ids: limit=%q list=%q", repo.lastLimitCompanyID, repo.lastListCompanyID)
	}
	if evaluation.MachineLimit == nil || *evaluation.MachineLimit != limit {
		t.Fatalf("MachineLimit = %v, want %d", evaluation.MachineLimit, limit)
	}
	if got := machineIDs(evaluation.AllowedMachines()); !equalStrings(got, []string{"machine-1", "machine-2"}) {
		t.Fatalf("allowed machines = %v, want [machine-1 machine-2]", got)
	}
	if got := machineIDs(evaluation.BlockedMachines()); !equalStrings(got, []string{"machine-3", "machine-4"}) {
		t.Fatalf("blocked machines = %v, want [machine-3 machine-4]", got)
	}
	for _, machine := range evaluation.BlockedMachines() {
		if machine.State != StateBlocked || machine.BlockReason != BlockReasonMachineLimitExceeded {
			t.Fatalf("blocked machine state = %+v, want blocked with machine-limit reason", machine)
		}
	}
}

func TestEvaluateCompany_BreaksTimestampTiesByID(t *testing.T) {
	t.Parallel()

	limit := 2
	createdAt := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	evaluation, err := evaluateCompany("company-1", &limit, []MachineCandidate{
		candidate("machine-c", "company-1", createdAt),
		candidate("machine-a", "company-1", createdAt),
		candidate("machine-b", "company-1", createdAt),
	})
	if err != nil {
		t.Fatalf("evaluateCompany returned error: %v", err)
	}

	if got := machineIDs(evaluation.Machines); !equalStrings(got, []string{"machine-a", "machine-b", "machine-c"}) {
		t.Fatalf("machine order = %v, want [machine-a machine-b machine-c]", got)
	}
	if got := machineIDs(evaluation.BlockedMachines()); !equalStrings(got, []string{"machine-c"}) {
		t.Fatalf("blocked machines = %v, want [machine-c]", got)
	}
}

func TestServiceGetMachineState_ReportsBlockedReason(t *testing.T) {
	t.Parallel()

	limit := 1
	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	svc := NewService(&fakeRepository{
		limit: &limit,
		candidates: []MachineCandidate{
			candidate("machine-1", "company-1", base),
			candidate("machine-2", "company-1", base.Add(time.Minute)),
		},
	})

	state, err := svc.GetMachineState(context.Background(), "company-1", "machine-2")
	if err != nil {
		t.Fatalf("GetMachineState returned error: %v", err)
	}
	if !state.IsBlocked() {
		t.Fatalf("state = %+v, want blocked", state)
	}
	if state.BlockReason != BlockReasonMachineLimitExceeded {
		t.Fatalf("BlockReason = %q, want %q", state.BlockReason, BlockReasonMachineLimitExceeded)
	}
}

func TestServiceEvaluateCompany_LimitIncreaseReleasesNextEligibleMachine(t *testing.T) {
	t.Parallel()

	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	limit := 2
	repo := &fakeRepository{
		limit: &limit,
		candidates: []MachineCandidate{
			candidate("machine-1", "company-1", base.Add(1*time.Minute)),
			candidate("machine-2", "company-1", base.Add(2*time.Minute)),
			candidate("machine-3", "company-1", base.Add(3*time.Minute)),
			candidate("machine-4", "company-1", base.Add(4*time.Minute)),
		},
	}
	svc := NewService(repo)

	initial, err := svc.EvaluateCompany(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("initial EvaluateCompany returned error: %v", err)
	}
	if got := machineIDs(initial.BlockedMachines()); !equalStrings(got, []string{"machine-3", "machine-4"}) {
		t.Fatalf("initial blocked machines = %v, want [machine-3 machine-4]", got)
	}

	limit = 3
	updated, err := svc.EvaluateCompany(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("updated EvaluateCompany returned error: %v", err)
	}
	if got := machineIDs(updated.BlockedMachines()); !equalStrings(got, []string{"machine-4"}) {
		t.Fatalf("updated blocked machines = %v, want [machine-4]", got)
	}
	state, ok := updated.StateForMachine("machine-3")
	if !ok || state.State != StateAllowed || state.BlockReason != BlockReasonNone {
		t.Fatalf("machine-3 state after limit increase = %+v, want allowed without reason", state)
	}
}

func TestServiceEvaluateCompany_MachineRemovalReleasesAccessDeterministically(t *testing.T) {
	t.Parallel()

	base := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)
	limit := 2
	repo := &fakeRepository{
		limit: &limit,
		candidates: []MachineCandidate{
			candidate("machine-a", "company-1", base.Add(1*time.Minute)),
			candidate("machine-b", "company-1", base.Add(2*time.Minute)),
			candidate("machine-c", "company-1", base.Add(3*time.Minute)),
			candidate("machine-d", "company-1", base.Add(4*time.Minute)),
		},
	}
	svc := NewService(repo)

	initial, err := svc.EvaluateCompany(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("initial EvaluateCompany returned error: %v", err)
	}
	if got := machineIDs(initial.BlockedMachines()); !equalStrings(got, []string{"machine-c", "machine-d"}) {
		t.Fatalf("initial blocked machines = %v, want [machine-c machine-d]", got)
	}

	repo.candidates = []MachineCandidate{
		candidate("machine-a", "company-1", base.Add(1*time.Minute)),
		candidate("machine-c", "company-1", base.Add(3*time.Minute)),
		candidate("machine-d", "company-1", base.Add(4*time.Minute)),
	}
	updated, err := svc.EvaluateCompany(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("updated EvaluateCompany returned error: %v", err)
	}
	if got := machineIDs(updated.BlockedMachines()); !equalStrings(got, []string{"machine-d"}) {
		t.Fatalf("updated blocked machines = %v, want [machine-d]", got)
	}
	state, ok := updated.StateForMachine("machine-c")
	if !ok || state.State != StateAllowed || state.BlockReason != BlockReasonNone {
		t.Fatalf("machine-c state after removal = %+v, want allowed without reason", state)
	}
}

func TestServiceEvaluateCompany_DBReadErrorsFailClosed(t *testing.T) {
	t.Parallel()

	dbErr := errors.New("db unavailable")
	tests := []struct {
		name string
		repo *fakeRepository
	}{
		{
			name: "company_limit_lookup",
			repo: &fakeRepository{limitErr: dbErr},
		},
		{
			name: "candidate_list_lookup",
			repo: &fakeRepository{limit: intPtr(2), candidatesErr: dbErr},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			svc := NewService(tc.repo)
			_, err := svc.EvaluateCompany(context.Background(), "company-1")
			if !errors.Is(err, dbErr) {
				t.Fatalf("expected db error, got %v", err)
			}
		})
	}
}

func TestServiceEvaluateCompany_MapsMissingCompanyToNotFound(t *testing.T) {
	t.Parallel()

	svc := NewService(&fakeRepository{limitErr: pgx.ErrNoRows})

	_, err := svc.EvaluateCompany(context.Background(), "missing-company")
	if !errors.Is(err, ErrCompanyNotFound) {
		t.Fatalf("expected ErrCompanyNotFound, got %v", err)
	}
}

func TestEvaluateCompany_RejectsMalformedCandidateRows(t *testing.T) {
	t.Parallel()

	limit := 1
	createdAt := time.Date(2026, 4, 8, 12, 0, 0, 0, time.UTC)

	tests := []struct {
		name       string
		candidates []MachineCandidate
	}{
		{
			name: "missing_created_at",
			candidates: []MachineCandidate{
				{ID: "machine-1", CompanyID: "company-1"},
			},
		},
		{
			name: "wrong_company_id",
			candidates: []MachineCandidate{
				candidate("machine-1", "company-2", createdAt),
			},
		},
		{
			name: "duplicate_machine_id",
			candidates: []MachineCandidate{
				candidate("machine-1", "company-1", createdAt),
				candidate("machine-1", "company-1", createdAt.Add(time.Minute)),
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := evaluateCompany("company-1", &limit, tc.candidates)
			if !errors.Is(err, ErrMalformedCandidate) {
				t.Fatalf("expected ErrMalformedCandidate, got %v", err)
			}
		})
	}
}

func TestEvaluateCompany_RejectsNegativeLimit(t *testing.T) {
	t.Parallel()

	limit := -1
	_, err := evaluateCompany("company-1", &limit, []MachineCandidate{})
	if !errors.Is(err, ErrMalformedLimit) {
		t.Fatalf("expected ErrMalformedLimit, got %v", err)
	}
}

func candidate(id, companyID string, createdAt time.Time) MachineCandidate {
	return MachineCandidate{ID: id, CompanyID: companyID, CreatedAt: createdAt}
}

func intPtr(value int) *int {
	return &value
}

func machineIDs(machines []MachineState) []string {
	ids := make([]string, 0, len(machines))
	for _, machine := range machines {
		ids = append(ids, machine.MachineID)
	}
	if ids == nil {
		return []string{}
	}
	return ids
}

func equalStrings(got, want []string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}
