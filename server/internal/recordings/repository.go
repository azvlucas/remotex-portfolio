package recordings

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ErrNotFound = errors.New("recording not found")

// Recording representa uma gravação de sessão remota.
type Recording struct {
	ID          string    `json:"id"`
	SessionID   *string   `json:"session_id,omitempty"`
	CompanyID   string    `json:"company_id"`
	MachineID   string    `json:"machine_id"`
	StorageKey  string    `json:"storage_key"`
	FileSize    int64     `json:"file_size"`
	DurationSec int       `json:"duration_sec"`
	Status      string    `json:"status"`
	CreatedAt   time.Time `json:"created_at"`
}

type Repository struct {
	db *pgxpool.Pool
}

func NewRepository(db *pgxpool.Pool) *Repository {
	return &Repository{db: db}
}

func (r *Repository) Create(ctx context.Context, sessionID *string, companyID, machineID, storageKey string, fileSize int64, durationSec int) (*Recording, error) {
	if ok, err := r.machineBelongsToCompany(ctx, machineID, companyID); err != nil {
		return nil, err
	} else if !ok {
		return nil, ErrNotFound
	}

	var rec Recording
	err := r.db.QueryRow(ctx,
		`INSERT INTO session_recordings (session_id, company_id, machine_id, storage_key, file_size, duration_sec, status)
		 VALUES ($1, $2, $3, $4, $5, $6, 'pending')
		 RETURNING id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at`,
		sessionID, companyID, machineID, storageKey, fileSize, durationSec,
	).Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID, &rec.StorageKey,
		&rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt)
	if err != nil {
		return nil, err
	}
	return &rec, nil
}

func (r *Repository) UpdateStatus(ctx context.Context, id, companyID, status string) error {
	tag, err := r.db.Exec(ctx,
		`UPDATE session_recordings SET status = $3 WHERE id = $1 AND company_id = $2`,
		id, companyID, status,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (r *Repository) GetByID(ctx context.Context, id, companyID string) (*Recording, error) {
	var rec Recording
	err := r.db.QueryRow(ctx,
		`SELECT id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at
		   FROM session_recordings
		  WHERE id = $1 AND company_id = $2`,
		id, companyID,
	).Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID, &rec.StorageKey,
		&rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return &rec, err
}

func (r *Repository) GetBySessionID(ctx context.Context, sessionID, companyID string) (*Recording, error) {
	var rec Recording
	err := r.db.QueryRow(ctx,
		`SELECT id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at
		   FROM session_recordings
		  WHERE session_id = $1 AND company_id = $2`,
		sessionID, companyID,
	).Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID, &rec.StorageKey,
		&rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	return &rec, err
}

func (r *Repository) ListByCompany(ctx context.Context, companyID string, limit, offset int) ([]Recording, error) {
	if limit <= 0 {
		limit = 50
	}
	rows, err := r.db.Query(ctx,
		`SELECT id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at
		   FROM session_recordings
		  WHERE company_id = $1
		  ORDER BY created_at DESC
		  LIMIT $2 OFFSET $3`,
		companyID, limit, offset,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []Recording
	for rows.Next() {
		var rec Recording
		if err := rows.Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID,
			&rec.StorageKey, &rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt); err != nil {
			return nil, err
		}
		list = append(list, rec)
	}
	if list == nil {
		list = []Recording{}
	}
	return list, rows.Err()
}

func (r *Repository) ListByMachine(ctx context.Context, machineID, companyID string, limit, offset int) ([]Recording, error) {
	if ok, err := r.machineBelongsToCompany(ctx, machineID, companyID); err != nil {
		return nil, err
	} else if !ok {
		return nil, ErrNotFound
	}

	if limit <= 0 {
		limit = 50
	}
	rows, err := r.db.Query(ctx,
		`SELECT id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at
		   FROM session_recordings
		  WHERE machine_id = $1 AND company_id = $2
		  ORDER BY created_at DESC
		  LIMIT $3 OFFSET $4`,
		machineID, companyID, limit, offset,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []Recording
	for rows.Next() {
		var rec Recording
		if err := rows.Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID,
			&rec.StorageKey, &rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt); err != nil {
			return nil, err
		}
		list = append(list, rec)
	}
	if list == nil {
		list = []Recording{}
	}
	return list, rows.Err()
}

func (r *Repository) Delete(ctx context.Context, id, companyID string) (*Recording, error) {
	var rec Recording
	err := r.db.QueryRow(ctx,
		`DELETE FROM session_recordings
		  WHERE id = $1 AND company_id = $2
		  RETURNING id, session_id, company_id, machine_id, storage_key, file_size, duration_sec, status, created_at`,
		id, companyID,
	).Scan(&rec.ID, &rec.SessionID, &rec.CompanyID, &rec.MachineID, &rec.StorageKey,
		&rec.FileSize, &rec.DurationSec, &rec.Status, &rec.CreatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &rec, nil
}

func (r *Repository) machineBelongsToCompany(ctx context.Context, machineID, companyID string) (bool, error) {
	var exists bool
	err := r.db.QueryRow(ctx,
		`SELECT EXISTS(SELECT 1 FROM machines WHERE id = $1 AND company_id = $2)`,
		machineID, companyID,
	).Scan(&exists)
	return exists, err
}
