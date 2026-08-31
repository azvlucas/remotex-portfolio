package machines

import (
	"context"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

type Machine struct {
	ID                   string                   `json:"id"`
	UserID               string                   `json:"user_id"`
	CompanyID            string                   `json:"company_id"`
	Name                 string                   `json:"name"`
	MachineKey           string                   `json:"-"`
	OSInfo               *string                  `json:"os_info,omitempty"`
	MacAddress           *string                  `json:"mac_address,omitempty"`
	AgentVersion         *string                  `json:"agent_version,omitempty"`
	LastHeartbeat        *time.Time               `json:"last_heartbeat,omitempty"`
	IsOnline             bool                     `json:"is_online"`
	State                machinelimit.State       `json:"state,omitempty"`
	BlockReason          machinelimit.BlockReason `json:"block_reason,omitempty"`
	GroupID              *string                  `json:"group_id,omitempty"`
	GroupName            *string                  `json:"group_name,omitempty"`
	SubgroupID           *string                  `json:"subgroup_id,omitempty"`
	SubgroupName         *string                  `json:"subgroup_name,omitempty"`
	LegalName            *string                  `json:"legal_name,omitempty"`
	TradeName            *string                  `json:"trade_name,omitempty"`
	DocumentType         *string                  `json:"document_type,omitempty"`
	DocumentNumber       *string                  `json:"document_number,omitempty"`
	ManagersOnlyAccess   bool                     `json:"managers_only_access"`
	RequireClientConsent bool                     `json:"require_client_consent"`
	ForceUpdate          bool                     `json:"force_update"`
	CreatedAt            time.Time                `json:"created_at"`
	UpdatedAt            time.Time                `json:"updated_at"`
}

type MachineUpsertInput struct {
	Name                 string
	OSInfo               *string
	MacAddress           *string
	GroupID              *string
	SubgroupID           *string
	LegalName            *string
	TradeName            *string
	DocumentType         *string
	DocumentNumber       *string
	ManagersOnlyAccess   bool
	RequireClientConsent bool
}

type MachineSubgroupRef struct {
	ID      string
	GroupID string
}

type Repository struct {
	db *pgxpool.Pool
}

func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

func (r *Repository) List(ctx context.Context, companyID string) ([]Machine, error) {
	rows, err := r.db.Query(ctx, machineSelectQuery+`
		  WHERE m.company_id = $1
		  ORDER BY m.name`,
		companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var machines []Machine
	for rows.Next() {
		var m Machine
		if err := scanMachine(rows, &m); err != nil {
			return nil, err
		}
		machines = append(machines, m)
	}
	return machines, rows.Err()
}

func (r *Repository) GetByID(ctx context.Context, id, companyID string) (*Machine, error) {
	var m Machine
	err := scanMachine(r.db.QueryRow(ctx, machineSelectQuery+`
		  WHERE m.id = $1
		    AND m.company_id = $2`,
		id, companyID,
	), &m)
	if err != nil {
		return nil, err
	}
	return &m, nil
}

func (r *Repository) Create(ctx context.Context, userID, companyID, machineKey string, input MachineUpsertInput) (*Machine, error) {
	var m Machine
	err := scanMachine(r.db.QueryRow(ctx, `
		INSERT INTO machines (
			user_id, company_id, name, machine_key, os_info, mac_address,
			group_id, subgroup_id, legal_name, trade_name, document_type, document_number,
			managers_only_access, require_client_consent
		)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)
		RETURNING
			id,
			user_id,
			company_id,
			name,
			machine_key,
			os_info,
			mac_address,
			agent_version,
			last_heartbeat,
			is_online,
			group_id,
			(SELECT name FROM machine_groups WHERE id = group_id),
			subgroup_id,
			(SELECT name FROM machine_subgroups WHERE id = subgroup_id),
			legal_name,
			trade_name,
			document_type,
			document_number,
			managers_only_access,
			require_client_consent,
			force_update,
			created_at,
			updated_at`,
		userID,
		companyID,
		input.Name,
		machineKey,
		input.OSInfo,
		input.MacAddress,
		input.GroupID,
		input.SubgroupID,
		input.LegalName,
		input.TradeName,
		input.DocumentType,
		input.DocumentNumber,
		input.ManagersOnlyAccess,
		input.RequireClientConsent,
	), &m)
	if err != nil {
		return nil, err
	}
	return &m, nil
}

func (r *Repository) Update(ctx context.Context, id, companyID string, input MachineUpsertInput) (*Machine, error) {
	var m Machine
	err := scanMachine(r.db.QueryRow(ctx, `
		UPDATE machines
		   SET name = $3,
		       os_info = $4,
		       mac_address = $5,
		       group_id = $6,
		       subgroup_id = $7,
		       legal_name = $8,
		       trade_name = $9,
		       document_type = $10,
		       document_number = $11,
		       managers_only_access = $12,
		       require_client_consent = $13,
		       updated_at = NOW()
		 WHERE id = $1
		   AND company_id = $2
		 RETURNING
			id,
			user_id,
			company_id,
			name,
			machine_key,
			os_info,
			mac_address,
			agent_version,
			last_heartbeat,
			is_online,
			group_id,
			(SELECT name FROM machine_groups WHERE id = group_id),
			subgroup_id,
			(SELECT name FROM machine_subgroups WHERE id = subgroup_id),
			legal_name,
			trade_name,
			document_type,
			document_number,
			managers_only_access,
			require_client_consent,
			force_update,
			created_at,
			updated_at`,
		id,
		companyID,
		input.Name,
		input.OSInfo,
		input.MacAddress,
		input.GroupID,
		input.SubgroupID,
		input.LegalName,
		input.TradeName,
		input.DocumentType,
		input.DocumentNumber,
		input.ManagersOnlyAccess,
		input.RequireClientConsent,
	), &m)
	if err != nil {
		return nil, err
	}
	return &m, nil
}

func (r *Repository) Delete(ctx context.Context, id, companyID string) error {
	tag, err := r.db.Exec(ctx,
		"DELETE FROM machines WHERE id = $1 AND company_id = $2",
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

func (r *Repository) UpdateHeartbeat(ctx context.Context, machineKey, companyID, agentVersion string) (bool, error) {
	var agentVersionPtr *string
	if agentVersion != "" {
		agentVersionPtr = &agentVersion
	}
	var forceUpdate bool
	err := r.db.QueryRow(ctx,
		`WITH old AS (
		    SELECT force_update
		      FROM machines
		     WHERE machine_key = $1 AND company_id = $2
		)
		UPDATE machines
		   SET last_heartbeat = NOW(),
		       is_online = TRUE,
		       updated_at = NOW(),
		       agent_version = COALESCE($3, agent_version),
		       force_update = FALSE
		 WHERE machine_key = $1
		   AND company_id = $2
		 RETURNING (SELECT force_update FROM old)`,
		machineKey, companyID, agentVersionPtr,
	).Scan(&forceUpdate)
	if err != nil {
		return false, err
	}
	return forceUpdate, nil
}

func (r *Repository) SetForceUpdate(ctx context.Context, id, companyID string) error {
	tag, err := r.db.Exec(ctx,
		`UPDATE machines
		    SET force_update = TRUE,
		        updated_at = NOW()
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

func (r *Repository) MarkOffline(ctx context.Context, timeout time.Duration) (int64, error) {
	tag, err := r.db.Exec(ctx,
		`UPDATE machines
		    SET is_online = FALSE,
		        updated_at = NOW()
		  WHERE is_online = TRUE
		    AND (last_heartbeat IS NULL OR last_heartbeat < NOW() - $1::interval)`,
		timeout.String(),
	)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

func (r *Repository) GroupExists(ctx context.Context, id, companyID string) (bool, error) {
	var exists bool
	err := r.db.QueryRow(ctx,
		`SELECT EXISTS(
			SELECT 1
			  FROM machine_groups
			 WHERE id = $1
			   AND company_id = $2
		)`,
		id, companyID,
	).Scan(&exists)
	return exists, err
}

func (r *Repository) GetSubgroup(ctx context.Context, id, companyID string) (*MachineSubgroupRef, error) {
	var subgroup MachineSubgroupRef
	err := r.db.QueryRow(ctx,
		`SELECT id, group_id
		   FROM machine_subgroups
		  WHERE id = $1
		    AND company_id = $2`,
		id, companyID,
	).Scan(&subgroup.ID, &subgroup.GroupID)
	if err != nil {
		return nil, err
	}
	return &subgroup, nil
}

type machineScanner interface {
	Scan(dest ...interface{}) error
}

func scanMachine(scanner machineScanner, m *Machine) error {
	return scanner.Scan(
		&m.ID,
		&m.UserID,
		&m.CompanyID,
		&m.Name,
		&m.MachineKey,
		&m.OSInfo,
		&m.MacAddress,
		&m.AgentVersion,
		&m.LastHeartbeat,
		&m.IsOnline,
		&m.GroupID,
		&m.GroupName,
		&m.SubgroupID,
		&m.SubgroupName,
		&m.LegalName,
		&m.TradeName,
		&m.DocumentType,
		&m.DocumentNumber,
		&m.ManagersOnlyAccess,
		&m.RequireClientConsent,
		&m.ForceUpdate,
		&m.CreatedAt,
		&m.UpdatedAt,
	)
}

const machineSelectQuery = `
SELECT
	m.id,
	m.user_id,
	m.company_id,
	m.name,
	m.machine_key,
	m.os_info,
	m.mac_address,
	m.agent_version,
	m.last_heartbeat,
	m.is_online,
	m.group_id,
	g.name AS group_name,
	m.subgroup_id,
	sg.name AS subgroup_name,
	m.legal_name,
	m.trade_name,
	m.document_type,
	m.document_number,
	m.managers_only_access,
	m.require_client_consent,
	m.force_update,
	m.created_at,
	m.updated_at
  FROM machines m
  LEFT JOIN machine_groups g
    ON g.id = m.group_id
  LEFT JOIN machine_subgroups sg
    ON sg.id = m.subgroup_id
`
