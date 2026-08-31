package machines

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

var (
	ErrNotFound                 = errors.New("machine not found")
	ErrAlreadyExists            = errors.New("machine key already exists")
	ErrInvalidName              = errors.New("name is required")
	ErrGroupNotFound            = errors.New("group not found")
	ErrSubgroupNotFound         = errors.New("subgroup not found")
	ErrGroupRequired            = errors.New("group is required when subgroup is set")
	ErrSubgroupGroupMismatch    = errors.New("subgroup does not belong to the selected group")
	ErrInvalidDocument          = errors.New("invalid document")
	ErrMachineStateAnnotation   = errors.New("machine state annotation failed")
	digitsOnlyDocumentSanitizer = regexp.MustCompile(`\D`)
)

type repository interface {
	List(ctx context.Context, companyID string) ([]Machine, error)
	GetByID(ctx context.Context, id, companyID string) (*Machine, error)
	Create(ctx context.Context, userID, companyID, machineKey string, input MachineUpsertInput) (*Machine, error)
	Update(ctx context.Context, id, companyID string, input MachineUpsertInput) (*Machine, error)
	Delete(ctx context.Context, id, companyID string) error
	UpdateHeartbeat(ctx context.Context, machineKey, companyID, agentVersion string) (bool, error)
	SetForceUpdate(ctx context.Context, id, companyID string) error
	MarkOffline(ctx context.Context, timeout time.Duration) (int64, error)
	GroupExists(ctx context.Context, id, companyID string) (bool, error)
	GetSubgroup(ctx context.Context, id, companyID string) (*MachineSubgroupRef, error)
}

type Service struct {
	repo            repository
	machineLimitSvc *machinelimit.Service
}

func NewService(repo repository, machineLimitSvc *machinelimit.Service) *Service {
	return &Service{repo: repo, machineLimitSvc: machineLimitSvc}
}

func (s *Service) List(ctx context.Context, companyID string) ([]Machine, error) {
	machines, err := s.repo.List(ctx, companyID)
	if err != nil {
		return nil, err
	}
	return s.annotateMachines(ctx, companyID, machines, true)
}

func (s *Service) GetByID(ctx context.Context, id, companyID string) (*Machine, error) {
	m, err := s.repo.GetByID(ctx, id, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return m, err
}

func (s *Service) Create(ctx context.Context, userID, companyID string, input MachineUpsertInput) (*Machine, error) {
	normalizedInput, err := s.normalizeInput(ctx, companyID, input)
	if err != nil {
		return nil, err
	}

	machineKey, err := generateMachineKey()
	if err != nil {
		return nil, err
	}

	m, err := s.repo.Create(ctx, userID, companyID, machineKey, normalizedInput)
	if err != nil {
		return nil, err
	}

	annotated, err := s.annotateMachines(ctx, companyID, []Machine{*m}, false)
	if err != nil {
		return nil, err
	}
	return &annotated[0], nil
}

func (s *Service) Update(ctx context.Context, id, companyID string, input MachineUpsertInput) (*Machine, error) {
	normalizedInput, err := s.normalizeInput(ctx, companyID, input)
	if err != nil {
		return nil, err
	}

	m, err := s.repo.Update(ctx, id, companyID, normalizedInput)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return m, err
}

func (s *Service) Delete(ctx context.Context, id, companyID string) error {
	err := s.repo.Delete(ctx, id, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	return err
}

func (s *Service) Heartbeat(ctx context.Context, machineKey, companyID, agentVersion string) (bool, error) {
	forceUpdate, err := s.repo.UpdateHeartbeat(ctx, machineKey, companyID, agentVersion)
	if errors.Is(err, pgx.ErrNoRows) {
		return false, ErrNotFound
	}
	return forceUpdate, err
}

func (s *Service) SetForceUpdate(ctx context.Context, id, companyID string) error {
	err := s.repo.SetForceUpdate(ctx, id, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	return err
}

func (s *Service) MarkStaleOffline(ctx context.Context, timeout time.Duration) (int64, error) {
	return s.repo.MarkOffline(ctx, timeout)
}

func (s *Service) annotateMachines(ctx context.Context, companyID string, machines []Machine, requireCompleteMatch bool) ([]Machine, error) {
	if s.machineLimitSvc == nil {
		return nil, fmt.Errorf("%w: machine limit service unavailable", ErrMachineStateAnnotation)
	}

	evaluation, err := s.machineLimitSvc.EvaluateCompany(ctx, companyID)
	if err != nil {
		return nil, fmt.Errorf("%w: evaluate company %q: %w", ErrMachineStateAnnotation, companyID, err)
	}
	if evaluation == nil {
		return nil, fmt.Errorf("%w: company evaluation %q returned nil", ErrMachineStateAnnotation, companyID)
	}

	statesByID := make(map[string]machinelimit.MachineState, len(evaluation.Machines))
	for _, state := range evaluation.Machines {
		statesByID[state.MachineID] = state
	}

	annotated := make([]Machine, 0, len(machines))
	for _, machine := range machines {
		state, ok := statesByID[machine.ID]
		if !ok {
			return nil, fmt.Errorf("%w: machine %q missing from company evaluation %q", ErrMachineStateAnnotation, machine.ID, companyID)
		}
		machine.State = state.State
		machine.BlockReason = state.BlockReason
		annotated = append(annotated, machine)
		delete(statesByID, machine.ID)
	}

	if requireCompleteMatch && len(statesByID) > 0 {
		unexpectedIDs := make([]string, 0, len(statesByID))
		for machineID := range statesByID {
			unexpectedIDs = append(unexpectedIDs, machineID)
		}
		sort.Strings(unexpectedIDs)
		return nil, fmt.Errorf("%w: company evaluation %q returned unexpected machine ids %s", ErrMachineStateAnnotation, companyID, strings.Join(unexpectedIDs, ", "))
	}

	return annotated, nil
}

func (s *Service) normalizeInput(ctx context.Context, companyID string, input MachineUpsertInput) (MachineUpsertInput, error) {
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" {
		return MachineUpsertInput{}, ErrInvalidName
	}

	input.OSInfo = normalizeOptionalString(input.OSInfo)
	input.MacAddress = normalizeOptionalString(input.MacAddress)
	input.GroupID = normalizeOptionalIdentifier(input.GroupID)
	input.SubgroupID = normalizeOptionalIdentifier(input.SubgroupID)
	input.LegalName = normalizeOptionalString(input.LegalName)
	input.TradeName = normalizeOptionalString(input.TradeName)

	documentType, documentNumber, err := normalizeDocument(input.DocumentType, input.DocumentNumber)
	if err != nil {
		return MachineUpsertInput{}, err
	}
	input.DocumentType = documentType
	input.DocumentNumber = documentNumber

	if err := s.validateGroupSelection(ctx, companyID, input.GroupID, input.SubgroupID); err != nil {
		return MachineUpsertInput{}, err
	}

	return input, nil
}

func (s *Service) validateGroupSelection(ctx context.Context, companyID string, groupID, subgroupID *string) error {
	if subgroupID != nil && groupID == nil {
		return ErrGroupRequired
	}

	if groupID != nil {
		exists, err := s.repo.GroupExists(ctx, *groupID, companyID)
		if err != nil {
			return err
		}
		if !exists {
			return ErrGroupNotFound
		}
	}

	if subgroupID == nil {
		return nil
	}

	subgroup, err := s.repo.GetSubgroup(ctx, *subgroupID, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrSubgroupNotFound
	}
	if err != nil {
		return err
	}
	if subgroup.GroupID != *groupID {
		return ErrSubgroupGroupMismatch
	}

	return nil
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

func normalizeOptionalIdentifier(value *string) *string {
	if value == nil {
		return nil
	}
	trimmed := strings.TrimSpace(*value)
	if trimmed == "" {
		return nil
	}
	return &trimmed
}

func normalizeDocument(documentType, documentNumber *string) (*string, *string, error) {
	normalizedType := ""
	if documentType != nil {
		normalizedType = strings.ToLower(strings.TrimSpace(*documentType))
	}

	normalizedNumber := ""
	if documentNumber != nil {
		normalizedNumber = digitsOnlyDocumentSanitizer.ReplaceAllString(*documentNumber, "")
	}

	if normalizedType == "" && normalizedNumber == "" {
		return nil, nil, nil
	}
	if normalizedType == "" || normalizedNumber == "" {
		return nil, nil, fmt.Errorf("%w: document_type and document_number must be provided together", ErrInvalidDocument)
	}

	switch normalizedType {
	case "cpf":
		if len(normalizedNumber) != 11 {
			return nil, nil, fmt.Errorf("%w: cpf must have 11 digits", ErrInvalidDocument)
		}
	case "cnpj":
		if len(normalizedNumber) != 14 {
			return nil, nil, fmt.Errorf("%w: cnpj must have 14 digits", ErrInvalidDocument)
		}
	default:
		return nil, nil, fmt.Errorf("%w: document_type must be cpf or cnpj", ErrInvalidDocument)
	}

	return &normalizedType, &normalizedNumber, nil
}

func generateMachineKey() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}
