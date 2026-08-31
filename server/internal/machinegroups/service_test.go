package machinegroups

import (
	"context"
	"errors"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

type fakeRepo struct {
	group          *Group
	groupErr       error
	subgroups      []Subgroup
	subgroupsErr   error
	deleteGroupErr error
}

func (f *fakeRepo) ListGroups(context.Context, string) ([]Group, error) {
	return nil, nil
}

func (f *fakeRepo) GetGroupByID(context.Context, string, string) (*Group, error) {
	if f.groupErr != nil {
		return nil, f.groupErr
	}
	return f.group, nil
}

func (f *fakeRepo) CreateGroup(context.Context, string, string) (*Group, error) {
	return &Group{ID: "group-1", Name: "Grupo"}, nil
}

func (f *fakeRepo) UpdateGroup(context.Context, string, string, string) (*Group, error) {
	return &Group{ID: "group-1", Name: "Grupo"}, nil
}

func (f *fakeRepo) DeleteGroup(context.Context, string, string) error {
	return f.deleteGroupErr
}

func (f *fakeRepo) ListSubgroups(context.Context, string, string) ([]Subgroup, error) {
	return f.subgroups, f.subgroupsErr
}

func (f *fakeRepo) GetSubgroupByID(context.Context, string, string) (*Subgroup, error) {
	return nil, pgx.ErrNoRows
}

func (f *fakeRepo) CreateSubgroup(context.Context, string, string, string) (*Subgroup, error) {
	return &Subgroup{ID: "sub-1", GroupID: "group-1", Name: "Subgrupo"}, nil
}

func (f *fakeRepo) UpdateSubgroup(context.Context, string, string, string) (*Subgroup, error) {
	return &Subgroup{ID: "sub-1", GroupID: "group-1", Name: "Subgrupo"}, nil
}

func (f *fakeRepo) DeleteSubgroup(context.Context, string, string) error {
	return nil
}

func TestServiceListSubgroups_GroupNotFound(t *testing.T) {
	repo := &fakeRepo{groupErr: pgx.ErrNoRows}
	svc := NewService(repo)

	_, err := svc.ListSubgroups(context.Background(), "company-1", "group-1")
	if !errors.Is(err, ErrGroupNotFound) {
		t.Fatalf("expected ErrGroupNotFound, got %v", err)
	}
}

func TestServiceDeleteGroup_MapsForeignKeyViolation(t *testing.T) {
	repo := &fakeRepo{
		deleteGroupErr: &pgconn.PgError{Code: "23503"},
	}
	svc := NewService(repo)

	err := svc.DeleteGroup(context.Background(), "group-1", "company-1")
	if !errors.Is(err, ErrGroupInUse) {
		t.Fatalf("expected ErrGroupInUse, got %v", err)
	}
}

func TestNormalizeName_TrimsValue(t *testing.T) {
	name, err := normalizeName("  Financeiro  ")
	if err != nil {
		t.Fatalf("normalizeName failed: %v", err)
	}
	if name != "Financeiro" {
		t.Fatalf("name = %q, want Financeiro", name)
	}
}
