package machines

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

type fakeRepository struct {
	groupExists          bool
	groupExistsErr       error
	subgroup             *MachineSubgroupRef
	subgroupErr          error
	listResult           []Machine
	listErr              error
	createResult         *Machine
	createErr            error
	lastCreateInput      MachineUpsertInput
	lastUpdateInput      MachineUpsertInput
	machineLimit         *int
	machineLimitErr      error
	machineCandidates    []machinelimit.MachineCandidate
	machineCandidatesErr error
}

func (f *fakeRepository) List(context.Context, string) ([]Machine, error) {
	if f.listErr != nil {
		return nil, f.listErr
	}
	return f.listResult, nil
}

func (f *fakeRepository) GetByID(context.Context, string, string) (*Machine, error) {
	return nil, pgx.ErrNoRows
}

func (f *fakeRepository) Create(_ context.Context, _ string, companyID string, machineKey string, input MachineUpsertInput) (*Machine, error) {
	f.lastCreateInput = input
	if f.createErr != nil {
		return nil, f.createErr
	}
	if f.createResult != nil {
		machine := *f.createResult
		if machine.CompanyID == "" {
			machine.CompanyID = companyID
		}
		if machine.Name == "" {
			machine.Name = input.Name
		}
		if machine.MachineKey == "" {
			machine.MachineKey = machineKey
		}
		return &machine, nil
	}
	return &Machine{ID: "machine-1", CompanyID: companyID, Name: input.Name, MachineKey: machineKey}, nil
}

func (f *fakeRepository) Update(_ context.Context, _ string, _ string, input MachineUpsertInput) (*Machine, error) {
	f.lastUpdateInput = input
	return &Machine{Name: input.Name}, nil
}

func (f *fakeRepository) Delete(context.Context, string, string) error {
	return nil
}

func (f *fakeRepository) UpdateHeartbeat(context.Context, string, string, string) (bool, error) {
	return false, nil
}

func (f *fakeRepository) SetForceUpdate(context.Context, string, string) error {
	return nil
}

func (f *fakeRepository) MarkOffline(context.Context, time.Duration) (int64, error) {
	return 0, nil
}

func (f *fakeRepository) GroupExists(context.Context, string, string) (bool, error) {
	return f.groupExists, f.groupExistsErr
}

func (f *fakeRepository) GetSubgroup(context.Context, string, string) (*MachineSubgroupRef, error) {
	if f.subgroupErr != nil {
		return nil, f.subgroupErr
	}
	return f.subgroup, nil
}

func (f *fakeRepository) GetCompanyMachineLimit(context.Context, string) (*int, error) {
	if f.machineLimitErr != nil {
		return nil, f.machineLimitErr
	}
	return f.machineLimit, nil
}

func (f *fakeRepository) ListCompanyMachineCandidates(context.Context, string) ([]machinelimit.MachineCandidate, error) {
	if f.machineCandidatesErr != nil {
		return nil, f.machineCandidatesErr
	}
	return f.machineCandidates, nil
}

func newTestService(repo *fakeRepository) *Service {
	return NewService(repo, machinelimit.NewService(repo))
}

func TestGenerateMachineKey(t *testing.T) {
	key1, err := generateMachineKey()
	if err != nil {
		t.Fatalf("generateMachineKey failed: %v", err)
	}
	if len(key1) != 64 {
		t.Errorf("key length = %d, want 64", len(key1))
	}

	key2, err := generateMachineKey()
	if err != nil {
		t.Fatalf("generateMachineKey failed: %v", err)
	}
	if key1 == key2 {
		t.Error("two generated keys should be different")
	}
}

func TestNormalizeDocument_StripsFormattingAndLowercasesType(t *testing.T) {
	documentType := "CPF"
	documentNumber := "123.456.789-01"

	normalizedType, normalizedNumber, err := normalizeDocument(&documentType, &documentNumber)
	if err != nil {
		t.Fatalf("normalizeDocument failed: %v", err)
	}
	if normalizedType == nil || *normalizedType != "cpf" {
		t.Fatalf("normalizedType = %v, want cpf", normalizedType)
	}
	if normalizedNumber == nil || *normalizedNumber != "12345678901" {
		t.Fatalf("normalizedNumber = %v, want 12345678901", normalizedNumber)
	}
}

func TestNormalizeDocument_RejectsWrongLength(t *testing.T) {
	documentType := "cnpj"
	documentNumber := "123"

	_, _, err := normalizeDocument(&documentType, &documentNumber)
	if !errors.Is(err, ErrInvalidDocument) {
		t.Fatalf("expected ErrInvalidDocument, got %v", err)
	}
}

func TestServiceCreate_RejectsSubgroupWithoutGroup(t *testing.T) {
	repo := &fakeRepository{}
	svc := newTestService(repo)

	input := MachineUpsertInput{
		Name:       "Maquina 1",
		SubgroupID: strPtr("sub-1"),
	}

	_, err := svc.Create(context.Background(), "user-1", "company-1", input)
	if !errors.Is(err, ErrGroupRequired) {
		t.Fatalf("expected ErrGroupRequired, got %v", err)
	}
}

func TestServiceUpdate_RejectsUnknownGroup(t *testing.T) {
	repo := &fakeRepository{groupExists: false}
	svc := newTestService(repo)

	input := MachineUpsertInput{
		Name:    "Maquina 1",
		GroupID: strPtr("group-1"),
	}

	_, err := svc.Update(context.Background(), "machine-1", "company-1", input)
	if !errors.Is(err, ErrGroupNotFound) {
		t.Fatalf("expected ErrGroupNotFound, got %v", err)
	}
}

func TestServiceUpdate_RejectsSubgroupFromAnotherGroup(t *testing.T) {
	repo := &fakeRepository{
		groupExists: true,
		subgroup: &MachineSubgroupRef{
			ID:      "sub-1",
			GroupID: "group-2",
		},
	}
	svc := newTestService(repo)

	input := MachineUpsertInput{
		Name:       "Maquina 1",
		GroupID:    strPtr("group-1"),
		SubgroupID: strPtr("sub-1"),
	}

	_, err := svc.Update(context.Background(), "machine-1", "company-1", input)
	if !errors.Is(err, ErrSubgroupGroupMismatch) {
		t.Fatalf("expected ErrSubgroupGroupMismatch, got %v", err)
	}
}

func TestServiceCreate_NormalizesDocumentBeforePersisting(t *testing.T) {
	repo := &fakeRepository{groupExists: true}
	repo.createResult = &Machine{ID: "machine-1", CompanyID: "company-1", CreatedAt: time.Now().UTC()}
	repo.machineCandidates = []machinelimit.MachineCandidate{{
		ID:        "machine-1",
		CompanyID: "company-1",
		CreatedAt: time.Now().UTC(),
	}}
	svc := newTestService(repo)

	input := MachineUpsertInput{
		Name:           " Maquina 1 ",
		DocumentType:   strPtr("CNPJ"),
		DocumentNumber: strPtr("12.345.678/0001-90"),
	}

	_, err := svc.Create(context.Background(), "user-1", "company-1", input)
	if err != nil {
		t.Fatalf("Create failed: %v", err)
	}
	if repo.lastCreateInput.Name != "Maquina 1" {
		t.Fatalf("name = %q, want Maquina 1", repo.lastCreateInput.Name)
	}
	if repo.lastCreateInput.DocumentType == nil || *repo.lastCreateInput.DocumentType != "cnpj" {
		t.Fatalf("document type = %v, want cnpj", repo.lastCreateInput.DocumentType)
	}
	if repo.lastCreateInput.DocumentNumber == nil || *repo.lastCreateInput.DocumentNumber != "12345678000190" {
		t.Fatalf("document number = %v, want 12345678000190", repo.lastCreateInput.DocumentNumber)
	}
}

func TestServiceUpdate_PreservesManagersOnlyAccessFlag(t *testing.T) {
	repo := &fakeRepository{groupExists: true}
	svc := newTestService(repo)

	input := MachineUpsertInput{
		Name:               "Maquina 1",
		ManagersOnlyAccess: true,
	}

	if _, err := svc.Update(context.Background(), "machine-1", "company-1", input); err != nil {
		t.Fatalf("Update failed: %v", err)
	}
	if !repo.lastUpdateInput.ManagersOnlyAccess {
		t.Fatalf("ManagersOnlyAccess = false, want true")
	}
}

func TestServiceList_AnnotatesMachineStatesAndReasons(t *testing.T) {
	createdAt := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	later := createdAt.Add(time.Minute)
	limit := 1
	repo := &fakeRepository{
		listResult: []Machine{
			{ID: "machine-1", CompanyID: "company-1", Name: "Allowed", IsOnline: true},
			{ID: "machine-2", CompanyID: "company-1", Name: "Blocked", IsOnline: false},
		},
		machineLimit: &limit,
		machineCandidates: []machinelimit.MachineCandidate{
			{ID: "machine-1", CompanyID: "company-1", CreatedAt: createdAt},
			{ID: "machine-2", CompanyID: "company-1", CreatedAt: later},
		},
	}
	svc := newTestService(repo)

	machines, err := svc.List(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(machines) != 2 {
		t.Fatalf("len(machines) = %d, want 2", len(machines))
	}
	if machines[0].State != machinelimit.StateAllowed || machines[0].BlockReason != machinelimit.BlockReasonNone {
		t.Fatalf("machine-1 state = %q/%q, want allowed/empty", machines[0].State, machines[0].BlockReason)
	}
	if machines[1].State != machinelimit.StateBlocked || machines[1].BlockReason != machinelimit.BlockReasonMachineLimitExceeded {
		t.Fatalf("machine-2 state = %q/%q, want blocked/machine_limit_exceeded", machines[1].State, machines[1].BlockReason)
	}
	if !machines[0].IsOnline {
		t.Fatalf("machine-1 IsOnline = false, want true")
	}
	if machines[1].IsOnline {
		t.Fatalf("machine-2 IsOnline = true, want false")
	}
}

func TestServiceList_NilRepositorySliceReturnsEmptyAnnotatedSlice(t *testing.T) {
	repo := &fakeRepository{}
	svc := newTestService(repo)

	machines, err := svc.List(context.Background(), "company-1")
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if machines == nil {
		t.Fatal("machines = nil, want empty slice")
	}
	if len(machines) != 0 {
		t.Fatalf("len(machines) = %d, want 0", len(machines))
	}
}

func TestServiceList_MissingMachineInEvaluationFailsClosed(t *testing.T) {
	repo := &fakeRepository{
		listResult: []Machine{{ID: "machine-1", CompanyID: "company-1", Name: "Host"}},
	}
	svc := newTestService(repo)

	_, err := svc.List(context.Background(), "company-1")
	if !errors.Is(err, ErrMachineStateAnnotation) {
		t.Fatalf("expected ErrMachineStateAnnotation, got %v", err)
	}
}

func TestServiceList_MissingLimitServiceFailsClosed(t *testing.T) {
	svc := NewService(&fakeRepository{}, nil)

	_, err := svc.List(context.Background(), "company-1")
	if !errors.Is(err, ErrMachineStateAnnotation) {
		t.Fatalf("expected ErrMachineStateAnnotation, got %v", err)
	}
}

func TestServiceCreate_AnnotatesCreatedMachineState(t *testing.T) {
	createdAt := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	later := createdAt.Add(time.Minute)
	limit := 1
	repo := &fakeRepository{
		createResult: &Machine{ID: "machine-2", CompanyID: "company-1", Name: "Blocked Host", MachineKey: "machine-key", CreatedAt: later},
		machineLimit: &limit,
		machineCandidates: []machinelimit.MachineCandidate{
			{ID: "machine-1", CompanyID: "company-1", CreatedAt: createdAt},
			{ID: "machine-2", CompanyID: "company-1", CreatedAt: later},
		},
	}
	svc := newTestService(repo)

	machine, err := svc.Create(context.Background(), "user-1", "company-1", MachineUpsertInput{Name: "Blocked Host"})
	if err != nil {
		t.Fatalf("Create failed: %v", err)
	}
	if machine.State != machinelimit.StateBlocked || machine.BlockReason != machinelimit.BlockReasonMachineLimitExceeded {
		t.Fatalf("machine state = %q/%q, want blocked/machine_limit_exceeded", machine.State, machine.BlockReason)
	}
	if machine.MachineKey != "machine-key" {
		t.Fatalf("machine key = %q, want machine-key", machine.MachineKey)
	}
}

func TestServiceCreate_EvaluatorErrorFailsClosed(t *testing.T) {
	repo := &fakeRepository{
		createResult:    &Machine{ID: "machine-1", CompanyID: "company-1", Name: "Host"},
		machineLimitErr: errors.New("boom"),
	}
	svc := newTestService(repo)

	_, err := svc.Create(context.Background(), "user-1", "company-1", MachineUpsertInput{Name: "Host"})
	if !errors.Is(err, ErrMachineStateAnnotation) {
		t.Fatalf("expected ErrMachineStateAnnotation, got %v", err)
	}
}

func TestServiceCreate_MissingCreatedMachineInEvaluationFailsClosed(t *testing.T) {
	repo := &fakeRepository{
		createResult: &Machine{ID: "machine-2", CompanyID: "company-1", Name: "Host"},
		machineCandidates: []machinelimit.MachineCandidate{{
			ID:        "machine-1",
			CompanyID: "company-1",
			CreatedAt: time.Now().UTC(),
		}},
	}
	svc := newTestService(repo)

	_, err := svc.Create(context.Background(), "user-1", "company-1", MachineUpsertInput{Name: "Host"})
	if !errors.Is(err, ErrMachineStateAnnotation) {
		t.Fatalf("expected ErrMachineStateAnnotation, got %v", err)
	}
}

func strPtr(value string) *string {
	return &value
}
