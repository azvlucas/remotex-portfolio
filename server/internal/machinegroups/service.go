package machinegroups

import (
	"context"
	"errors"
	"strings"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

var (
	ErrGroupNotFound    = errors.New("group not found")
	ErrSubgroupNotFound = errors.New("subgroup not found")
	ErrDuplicateName    = errors.New("name already exists")
	ErrGroupInUse       = errors.New("group is in use")
	ErrSubgroupInUse    = errors.New("subgroup is in use")
	ErrInvalidName      = errors.New("name is required")
)

type repository interface {
	ListGroups(ctx context.Context, companyID string) ([]Group, error)
	GetGroupByID(ctx context.Context, id, companyID string) (*Group, error)
	CreateGroup(ctx context.Context, companyID, name string) (*Group, error)
	UpdateGroup(ctx context.Context, id, companyID, name string) (*Group, error)
	DeleteGroup(ctx context.Context, id, companyID string) error
	ListSubgroups(ctx context.Context, companyID, groupID string) ([]Subgroup, error)
	GetSubgroupByID(ctx context.Context, id, companyID string) (*Subgroup, error)
	CreateSubgroup(ctx context.Context, companyID, groupID, name string) (*Subgroup, error)
	UpdateSubgroup(ctx context.Context, id, companyID, name string) (*Subgroup, error)
	DeleteSubgroup(ctx context.Context, id, companyID string) error
}

type Service struct {
	repo repository
}

func NewService(repo repository) *Service {
	return &Service{repo: repo}
}

func (s *Service) ListGroups(ctx context.Context, companyID string) ([]Group, error) {
	groups, err := s.repo.ListGroups(ctx, companyID)
	if err != nil {
		return nil, err
	}
	if groups == nil {
		groups = []Group{}
	}
	return groups, nil
}

func (s *Service) CreateGroup(ctx context.Context, companyID, name string) (*Group, error) {
	normalizedName, err := normalizeName(name)
	if err != nil {
		return nil, err
	}
	group, err := s.repo.CreateGroup(ctx, companyID, normalizedName)
	return group, mapRepoError(err, false)
}

func (s *Service) UpdateGroup(ctx context.Context, id, companyID, name string) (*Group, error) {
	normalizedName, err := normalizeName(name)
	if err != nil {
		return nil, err
	}
	group, err := s.repo.UpdateGroup(ctx, id, companyID, normalizedName)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrGroupNotFound
	}
	return group, mapRepoError(err, false)
}

func (s *Service) DeleteGroup(ctx context.Context, id, companyID string) error {
	err := s.repo.DeleteGroup(ctx, id, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrGroupNotFound
	}
	return mapRepoError(err, true)
}

func (s *Service) ListSubgroups(ctx context.Context, companyID, groupID string) ([]Subgroup, error) {
	if _, err := s.repo.GetGroupByID(ctx, groupID, companyID); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrGroupNotFound
		}
		return nil, err
	}

	subgroups, err := s.repo.ListSubgroups(ctx, companyID, groupID)
	if err != nil {
		return nil, err
	}
	if subgroups == nil {
		subgroups = []Subgroup{}
	}
	return subgroups, nil
}

func (s *Service) CreateSubgroup(ctx context.Context, companyID, groupID, name string) (*Subgroup, error) {
	if _, err := s.repo.GetGroupByID(ctx, groupID, companyID); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrGroupNotFound
		}
		return nil, err
	}

	normalizedName, err := normalizeName(name)
	if err != nil {
		return nil, err
	}

	subgroup, err := s.repo.CreateSubgroup(ctx, companyID, groupID, normalizedName)
	return subgroup, mapRepoError(err, false)
}

func (s *Service) UpdateSubgroup(ctx context.Context, id, companyID, name string) (*Subgroup, error) {
	normalizedName, err := normalizeName(name)
	if err != nil {
		return nil, err
	}

	subgroup, err := s.repo.UpdateSubgroup(ctx, id, companyID, normalizedName)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrSubgroupNotFound
	}
	return subgroup, mapRepoError(err, false)
}

func (s *Service) DeleteSubgroup(ctx context.Context, id, companyID string) error {
	err := s.repo.DeleteSubgroup(ctx, id, companyID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrSubgroupNotFound
	}
	return mapRepoError(err, false)
}

func normalizeName(name string) (string, error) {
	normalized := strings.TrimSpace(name)
	if normalized == "" {
		return "", ErrInvalidName
	}
	return normalized, nil
}

func mapRepoError(err error, deletingGroup bool) error {
	if err == nil {
		return nil
	}

	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		switch pgErr.Code {
		case "23505":
			return ErrDuplicateName
		case "23503":
			if deletingGroup {
				return ErrGroupInUse
			}
			return ErrSubgroupInUse
		}
	}
	return err
}
