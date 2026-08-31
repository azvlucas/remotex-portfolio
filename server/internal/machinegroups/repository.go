package machinegroups

import (
	"context"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Group struct {
	ID        string    `json:"id"`
	CompanyID string    `json:"company_id"`
	Name      string    `json:"name"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

type Subgroup struct {
	ID        string    `json:"id"`
	CompanyID string    `json:"company_id"`
	GroupID   string    `json:"group_id"`
	Name      string    `json:"name"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

type Repository struct {
	db *pgxpool.Pool
}

func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

func (r *Repository) ListGroups(ctx context.Context, companyID string) ([]Group, error) {
	rows, err := r.db.Query(ctx,
		`SELECT id, company_id, name, created_at, updated_at
		   FROM machine_groups
		  WHERE company_id = $1
		  ORDER BY name`,
		companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var groups []Group
	for rows.Next() {
		var group Group
		if err := rows.Scan(
			&group.ID,
			&group.CompanyID,
			&group.Name,
			&group.CreatedAt,
			&group.UpdatedAt,
		); err != nil {
			return nil, err
		}
		groups = append(groups, group)
	}
	return groups, rows.Err()
}

func (r *Repository) GetGroupByID(ctx context.Context, id, companyID string) (*Group, error) {
	var group Group
	err := r.db.QueryRow(ctx,
		`SELECT id, company_id, name, created_at, updated_at
		   FROM machine_groups
		  WHERE id = $1
		    AND company_id = $2`,
		id, companyID,
	).Scan(
		&group.ID,
		&group.CompanyID,
		&group.Name,
		&group.CreatedAt,
		&group.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &group, nil
}

func (r *Repository) CreateGroup(ctx context.Context, companyID, name string) (*Group, error) {
	var group Group
	err := r.db.QueryRow(ctx,
		`INSERT INTO machine_groups (company_id, name)
		 VALUES ($1, $2)
		 RETURNING id, company_id, name, created_at, updated_at`,
		companyID, name,
	).Scan(
		&group.ID,
		&group.CompanyID,
		&group.Name,
		&group.CreatedAt,
		&group.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &group, nil
}

func (r *Repository) UpdateGroup(ctx context.Context, id, companyID, name string) (*Group, error) {
	var group Group
	err := r.db.QueryRow(ctx,
		`UPDATE machine_groups
		    SET name = $3,
		        updated_at = NOW()
		  WHERE id = $1
		    AND company_id = $2
		 RETURNING id, company_id, name, created_at, updated_at`,
		id, companyID, name,
	).Scan(
		&group.ID,
		&group.CompanyID,
		&group.Name,
		&group.CreatedAt,
		&group.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &group, nil
}

func (r *Repository) DeleteGroup(ctx context.Context, id, companyID string) error {
	tag, err := r.db.Exec(ctx,
		`DELETE FROM machine_groups
		  WHERE id = $1
		    AND company_id = $2`,
		id, companyID,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return pgx.ErrNoRows
	}
	return nil
}

func (r *Repository) ListSubgroups(ctx context.Context, companyID, groupID string) ([]Subgroup, error) {
	rows, err := r.db.Query(ctx,
		`SELECT id, company_id, group_id, name, created_at, updated_at
		   FROM machine_subgroups
		  WHERE company_id = $1
		    AND group_id = $2
		  ORDER BY name`,
		companyID, groupID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var subgroups []Subgroup
	for rows.Next() {
		var subgroup Subgroup
		if err := rows.Scan(
			&subgroup.ID,
			&subgroup.CompanyID,
			&subgroup.GroupID,
			&subgroup.Name,
			&subgroup.CreatedAt,
			&subgroup.UpdatedAt,
		); err != nil {
			return nil, err
		}
		subgroups = append(subgroups, subgroup)
	}
	return subgroups, rows.Err()
}

func (r *Repository) GetSubgroupByID(ctx context.Context, id, companyID string) (*Subgroup, error) {
	var subgroup Subgroup
	err := r.db.QueryRow(ctx,
		`SELECT id, company_id, group_id, name, created_at, updated_at
		   FROM machine_subgroups
		  WHERE id = $1
		    AND company_id = $2`,
		id, companyID,
	).Scan(
		&subgroup.ID,
		&subgroup.CompanyID,
		&subgroup.GroupID,
		&subgroup.Name,
		&subgroup.CreatedAt,
		&subgroup.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &subgroup, nil
}

func (r *Repository) CreateSubgroup(ctx context.Context, companyID, groupID, name string) (*Subgroup, error) {
	var subgroup Subgroup
	err := r.db.QueryRow(ctx,
		`INSERT INTO machine_subgroups (company_id, group_id, name)
		 VALUES ($1, $2, $3)
		 RETURNING id, company_id, group_id, name, created_at, updated_at`,
		companyID, groupID, name,
	).Scan(
		&subgroup.ID,
		&subgroup.CompanyID,
		&subgroup.GroupID,
		&subgroup.Name,
		&subgroup.CreatedAt,
		&subgroup.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &subgroup, nil
}

func (r *Repository) UpdateSubgroup(ctx context.Context, id, companyID, name string) (*Subgroup, error) {
	var subgroup Subgroup
	err := r.db.QueryRow(ctx,
		`UPDATE machine_subgroups
		    SET name = $3,
		        updated_at = NOW()
		  WHERE id = $1
		    AND company_id = $2
		 RETURNING id, company_id, group_id, name, created_at, updated_at`,
		id, companyID, name,
	).Scan(
		&subgroup.ID,
		&subgroup.CompanyID,
		&subgroup.GroupID,
		&subgroup.Name,
		&subgroup.CreatedAt,
		&subgroup.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &subgroup, nil
}

func (r *Repository) DeleteSubgroup(ctx context.Context, id, companyID string) error {
	tag, err := r.db.Exec(ctx,
		`DELETE FROM machine_subgroups
		  WHERE id = $1
		    AND company_id = $2`,
		id, companyID,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return pgx.ErrNoRows
	}
	return nil
}
