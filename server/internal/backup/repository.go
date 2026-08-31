package backup

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ErrNotFound = errors.New("not found")

// BackupConfig representa a configuração de backup de uma máquina.
type BackupConfig struct {
	ID             string    `json:"id"`
	MachineID      string    `json:"machine_id"`
	Folders        []string  `json:"folders"`
	IntervalHours  int       `json:"interval_hours"`
	RetentionCount int       `json:"retention_count"`
	Enabled        bool      `json:"enabled"`
	UpdatedAt      time.Time `json:"updated_at"`
}

// Snapshot representa um snapshot de backup armazenado.
type Snapshot struct {
	ID         string    `json:"id"`
	MachineID  string    `json:"machine_id"`
	StorageKey string    `json:"storage_key"`
	SizeBytes  int64     `json:"size_bytes"`
	FileCount  int       `json:"file_count"`
	CreatedAt  time.Time `json:"created_at"`
}

type Repository struct {
	db *pgxpool.Pool
}

func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

func (r *Repository) GetConfig(ctx context.Context, machineID, companyID string) (*BackupConfig, error) {
	var cfg BackupConfig
	var foldersJSON []byte
	err := r.db.QueryRow(ctx,
		`SELECT bc.id, bc.machine_id, bc.folders, bc.interval_hours, bc.retention_count, bc.enabled, bc.updated_at
		   FROM backup_configs bc
		   JOIN machines m ON m.id = bc.machine_id
		  WHERE bc.machine_id = $1
		    AND m.company_id = $2`,
		machineID, companyID,
	).Scan(&cfg.ID, &cfg.MachineID, &foldersJSON, &cfg.IntervalHours, &cfg.RetentionCount, &cfg.Enabled, &cfg.UpdatedAt)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			ok, checkErr := r.machineBelongsToCompany(ctx, machineID, companyID)
			if checkErr != nil {
				return nil, checkErr
			}
			if !ok {
				return nil, ErrNotFound
			}
		}
		return nil, err
	}
	if err := json.Unmarshal(foldersJSON, &cfg.Folders); err != nil {
		cfg.Folders = []string{}
	}
	return &cfg, nil
}

func (r *Repository) UpsertConfig(ctx context.Context, machineID, companyID string, folders []string, intervalHours, retentionCount int, enabled bool) (*BackupConfig, error) {
	if ok, err := r.machineBelongsToCompany(ctx, machineID, companyID); err != nil {
		return nil, err
	} else if !ok {
		return nil, ErrNotFound
	}

	foldersJSON, err := json.Marshal(folders)
	if err != nil {
		return nil, err
	}

	var cfg BackupConfig
	var foldersOut []byte
	err = r.db.QueryRow(ctx,
		`INSERT INTO backup_configs (machine_id, folders, interval_hours, retention_count, enabled, updated_at)
		 VALUES ($1, $2, $3, $4, $5, NOW())
		 ON CONFLICT (machine_id) DO UPDATE SET
		     folders = EXCLUDED.folders,
		     interval_hours = EXCLUDED.interval_hours,
		     retention_count = EXCLUDED.retention_count,
		     enabled = EXCLUDED.enabled,
		     updated_at = NOW()
		 RETURNING id, machine_id, folders, interval_hours, retention_count, enabled, updated_at`,
		machineID, foldersJSON, intervalHours, retentionCount, enabled,
	).Scan(&cfg.ID, &cfg.MachineID, &foldersOut, &cfg.IntervalHours, &cfg.RetentionCount, &cfg.Enabled, &cfg.UpdatedAt)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(foldersOut, &cfg.Folders); err != nil {
		cfg.Folders = []string{}
	}
	return &cfg, nil
}

func (r *Repository) ListSnapshots(ctx context.Context, machineID, companyID string) ([]Snapshot, error) {
	if ok, err := r.machineBelongsToCompany(ctx, machineID, companyID); err != nil {
		return nil, err
	} else if !ok {
		return nil, ErrNotFound
	}

	rows, err := r.db.Query(ctx,
		`SELECT bs.id, bs.machine_id, bs.storage_key, bs.size_bytes, bs.file_count, bs.created_at
		   FROM backup_snapshots bs
		   JOIN machines m ON m.id = bs.machine_id
		  WHERE bs.machine_id = $1
		    AND m.company_id = $2
		  ORDER BY bs.created_at DESC`,
		machineID, companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var snapshots []Snapshot
	for rows.Next() {
		var s Snapshot
		if err := rows.Scan(&s.ID, &s.MachineID, &s.StorageKey, &s.SizeBytes, &s.FileCount, &s.CreatedAt); err != nil {
			return nil, err
		}
		snapshots = append(snapshots, s)
	}
	if snapshots == nil {
		snapshots = []Snapshot{}
	}
	return snapshots, rows.Err()
}

func (r *Repository) CreateSnapshot(ctx context.Context, machineID, companyID, storageKey string, sizeBytes int64, fileCount int) (*Snapshot, error) {
	if ok, err := r.machineBelongsToCompany(ctx, machineID, companyID); err != nil {
		return nil, err
	} else if !ok {
		return nil, ErrNotFound
	}

	var s Snapshot
	err := r.db.QueryRow(ctx,
		`INSERT INTO backup_snapshots (machine_id, storage_key, size_bytes, file_count)
		 VALUES ($1, $2, $3, $4)
		 RETURNING id, machine_id, storage_key, size_bytes, file_count, created_at`,
		machineID, storageKey, sizeBytes, fileCount,
	).Scan(&s.ID, &s.MachineID, &s.StorageKey, &s.SizeBytes, &s.FileCount, &s.CreatedAt)
	if err != nil {
		return nil, err
	}
	return &s, nil
}

func (r *Repository) GetSnapshot(ctx context.Context, snapshotID, companyID string) (*Snapshot, error) {
	var s Snapshot
	err := r.db.QueryRow(ctx,
		`SELECT bs.id, bs.machine_id, bs.storage_key, bs.size_bytes, bs.file_count, bs.created_at
		   FROM backup_snapshots bs
		   JOIN machines m ON m.id = bs.machine_id
		  WHERE bs.id = $1
		    AND m.company_id = $2`,
		snapshotID, companyID,
	).Scan(&s.ID, &s.MachineID, &s.StorageKey, &s.SizeBytes, &s.FileCount, &s.CreatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return &s, err
}

func (r *Repository) DeleteSnapshot(ctx context.Context, snapshotID, companyID string) error {
	tag, err := r.db.Exec(ctx,
		`DELETE FROM backup_snapshots bs
		  USING machines m
		  WHERE bs.id = $1
		    AND m.id = bs.machine_id
		    AND m.company_id = $2`,
		snapshotID, companyID,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (r *Repository) GetOldestSnapshot(ctx context.Context, machineID, companyID string) (*Snapshot, error) {
	var s Snapshot
	err := r.db.QueryRow(ctx,
		`SELECT bs.id, bs.machine_id, bs.storage_key, bs.size_bytes, bs.file_count, bs.created_at
		   FROM backup_snapshots bs
		   JOIN machines m ON m.id = bs.machine_id
		  WHERE bs.machine_id = $1
		    AND m.company_id = $2
		  ORDER BY bs.created_at ASC
		  LIMIT 1`,
		machineID, companyID,
	).Scan(&s.ID, &s.MachineID, &s.StorageKey, &s.SizeBytes, &s.FileCount, &s.CreatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return &s, err
}

func (r *Repository) CountSnapshots(ctx context.Context, machineID, companyID string) (int, error) {
	var count int
	err := r.db.QueryRow(ctx,
		`SELECT COUNT(*)
		   FROM backup_snapshots bs
		   JOIN machines m ON m.id = bs.machine_id
		  WHERE bs.machine_id = $1
		    AND m.company_id = $2`,
		machineID, companyID,
	).Scan(&count)
	return count, err
}

func (r *Repository) machineBelongsToCompany(ctx context.Context, machineID, companyID string) (bool, error) {
	var exists bool
	err := r.db.QueryRow(ctx,
		`SELECT EXISTS(
			SELECT 1
			  FROM machines
			 WHERE id = $1
			   AND company_id = $2
		)`,
		machineID, companyID,
	).Scan(&exists)
	return exists, err
}
