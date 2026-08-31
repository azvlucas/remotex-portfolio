package machinelimit

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	ErrCompanyNotFound    = errors.New("company not found")
	ErrMachineNotFound    = errors.New("machine not found")
	ErrMalformedLimit     = errors.New("malformed machine limit")
	ErrMalformedCandidate = errors.New("malformed machine candidate")
)

type State string

const (
	StateAllowed State = "allowed"
	StateBlocked State = "blocked"
)

type BlockReason string

const (
	BlockReasonNone                 BlockReason = ""
	BlockReasonMachineLimitExceeded BlockReason = "machine_limit_exceeded"
)

type MachineCandidate struct {
	ID        string
	CompanyID string
	CreatedAt time.Time
}

type MachineState struct {
	MachineID   string
	CompanyID   string
	CreatedAt   time.Time
	State       State
	BlockReason BlockReason
}

func (s MachineState) IsBlocked() bool {
	return s.State == StateBlocked
}

type CompanyEvaluation struct {
	CompanyID     string
	MachineLimit  *int
	TotalMachines int
	Machines      []MachineState
}

func (e CompanyEvaluation) AllowedMachines() []MachineState {
	allowed := make([]MachineState, 0, len(e.Machines))
	for _, machine := range e.Machines {
		if machine.State == StateAllowed {
			allowed = append(allowed, machine)
		}
	}
	if allowed == nil {
		return []MachineState{}
	}
	return allowed
}

func (e CompanyEvaluation) BlockedMachines() []MachineState {
	blocked := make([]MachineState, 0, len(e.Machines))
	for _, machine := range e.Machines {
		if machine.State == StateBlocked {
			blocked = append(blocked, machine)
		}
	}
	if blocked == nil {
		return []MachineState{}
	}
	return blocked
}

func (e CompanyEvaluation) StateForMachine(machineID string) (MachineState, bool) {
	for _, machine := range e.Machines {
		if machine.MachineID == machineID {
			return machine, true
		}
	}
	return MachineState{}, false
}

type repository interface {
	GetCompanyMachineLimit(ctx context.Context, companyID string) (*int, error)
	ListCompanyMachineCandidates(ctx context.Context, companyID string) ([]MachineCandidate, error)
}

type Repository struct {
	db *pgxpool.Pool
}

func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

func (r *Repository) GetCompanyMachineLimit(ctx context.Context, companyID string) (*int, error) {
	var limit *int
	err := r.db.QueryRow(ctx,
		`SELECT machine_limit
		   FROM companies
		  WHERE id = $1`,
		companyID,
	).Scan(&limit)
	if err != nil {
		return nil, err
	}
	return limit, nil
}

func (r *Repository) ListCompanyMachineCandidates(ctx context.Context, companyID string) ([]MachineCandidate, error) {
	rows, err := r.db.Query(ctx,
		`SELECT id, company_id, created_at
		   FROM machines
		  WHERE company_id = $1
		  ORDER BY created_at ASC, id ASC`,
		companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	candidates := make([]MachineCandidate, 0)
	for rows.Next() {
		var candidate MachineCandidate
		if err := rows.Scan(&candidate.ID, &candidate.CompanyID, &candidate.CreatedAt); err != nil {
			return nil, err
		}
		candidates = append(candidates, candidate)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if candidates == nil {
		return []MachineCandidate{}, nil
	}
	return candidates, nil
}

type Service struct {
	repo repository
}

func NewService(repo repository) *Service {
	return &Service{repo: repo}
}

func (s *Service) EvaluateCompany(ctx context.Context, companyID string) (*CompanyEvaluation, error) {
	limit, err := s.repo.GetCompanyMachineLimit(ctx, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrCompanyNotFound
	}
	if err != nil {
		return nil, err
	}

	candidates, err := s.repo.ListCompanyMachineCandidates(ctx, companyID)
	if err != nil {
		return nil, err
	}
	if candidates == nil {
		candidates = []MachineCandidate{}
	}

	return evaluateCompany(companyID, limit, candidates)
}

func (s *Service) GetMachineState(ctx context.Context, companyID, machineID string) (*MachineState, error) {
	evaluation, err := s.EvaluateCompany(ctx, companyID)
	if err != nil {
		return nil, err
	}

	state, ok := evaluation.StateForMachine(machineID)
	if !ok {
		return nil, ErrMachineNotFound
	}
	return &state, nil
}

func evaluateCompany(companyID string, machineLimit *int, candidates []MachineCandidate) (*CompanyEvaluation, error) {
	normalizedLimit, err := normalizeMachineLimit(machineLimit)
	if err != nil {
		return nil, err
	}

	ranked, err := normalizeCandidates(companyID, candidates)
	if err != nil {
		return nil, err
	}

	sort.Slice(ranked, func(i, j int) bool {
		if ranked[i].CreatedAt.Equal(ranked[j].CreatedAt) {
			return ranked[i].ID < ranked[j].ID
		}
		return ranked[i].CreatedAt.Before(ranked[j].CreatedAt)
	})

	allowedCount := len(ranked)
	if normalizedLimit != nil && *normalizedLimit < allowedCount {
		allowedCount = *normalizedLimit
	}

	machines := make([]MachineState, 0, len(ranked))
	for idx, candidate := range ranked {
		state := MachineState{
			MachineID:   candidate.ID,
			CompanyID:   candidate.CompanyID,
			CreatedAt:   candidate.CreatedAt,
			State:       StateAllowed,
			BlockReason: BlockReasonNone,
		}
		if idx >= allowedCount {
			state.State = StateBlocked
			state.BlockReason = BlockReasonMachineLimitExceeded
		}
		machines = append(machines, state)
	}

	if machines == nil {
		machines = []MachineState{}
	}

	return &CompanyEvaluation{
		CompanyID:     companyID,
		MachineLimit:  normalizedLimit,
		TotalMachines: len(machines),
		Machines:      machines,
	}, nil
}

func normalizeMachineLimit(machineLimit *int) (*int, error) {
	if machineLimit == nil {
		return nil, nil
	}
	if *machineLimit < 0 {
		return nil, fmt.Errorf("%w: %d", ErrMalformedLimit, *machineLimit)
	}
	if *machineLimit == 0 {
		return nil, nil
	}
	normalized := *machineLimit
	return &normalized, nil
}

func normalizeCandidates(companyID string, candidates []MachineCandidate) ([]MachineCandidate, error) {
	normalized := make([]MachineCandidate, 0, len(candidates))
	seenIDs := make(map[string]struct{}, len(candidates))

	for idx, candidate := range candidates {
		candidate.ID = strings.TrimSpace(candidate.ID)
		candidate.CompanyID = strings.TrimSpace(candidate.CompanyID)

		switch {
		case candidate.ID == "":
			return nil, fmt.Errorf("%w: candidate %d missing id", ErrMalformedCandidate, idx)
		case candidate.CompanyID == "":
			return nil, fmt.Errorf("%w: machine %q missing company id", ErrMalformedCandidate, candidate.ID)
		case companyID != "" && candidate.CompanyID != companyID:
			return nil, fmt.Errorf("%w: machine %q belongs to company %q, want %q", ErrMalformedCandidate, candidate.ID, candidate.CompanyID, companyID)
		case candidate.CreatedAt.IsZero():
			return nil, fmt.Errorf("%w: machine %q missing created_at", ErrMalformedCandidate, candidate.ID)
		}

		if _, exists := seenIDs[candidate.ID]; exists {
			return nil, fmt.Errorf("%w: duplicate machine id %q", ErrMalformedCandidate, candidate.ID)
		}
		seenIDs[candidate.ID] = struct{}{}
		normalized = append(normalized, candidate)
	}

	if normalized == nil {
		return []MachineCandidate{}, nil
	}
	return normalized, nil
}
