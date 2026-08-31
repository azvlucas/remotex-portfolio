package auth

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ErrUserNotFound = errors.New("user not found")
var ErrMachineNotFound = errors.New("machine not found")

type UserRecord struct {
	ID               string
	Email            string
	PasswordHash     string
	Name             string
	CompanyID        *string
	CompanyName      *string
	Role             Role
	IsActive         bool
	CompanyActive    bool
	AdminTOTPEnabled bool
	AdminTOTPSecret  *string
}

type CreateUserInput struct {
	CompanyID    *string
	Email        string
	PasswordHash string
	Name         string
	Role         Role
	IsActive     bool
}

type MachineRecord struct {
	ID        string
	CompanyID string
	Name      string
}

type UserStore interface {
	EmailExists(ctx context.Context, email string) (bool, error)
	CreateUser(ctx context.Context, input CreateUserInput) (*UserRecord, error)
	GetUserByEmail(ctx context.Context, email string) (*UserRecord, error)
	GetUserByID(ctx context.Context, userID string) (*UserRecord, error)
	GetMachineByID(ctx context.Context, machineID string) (*MachineRecord, error)
	TouchUser(ctx context.Context, userID string, updatedAt time.Time) error
	EnableAdminTOTP(ctx context.Context, userID, secret string, updatedAt time.Time) error
}

type PostgresUserStore struct {
	db *pgxpool.Pool
}

func NewPostgresUserStore(db *pgxpool.Pool) UserStore {
	return &PostgresUserStore{db: db}
}

func (s *PostgresUserStore) EmailExists(ctx context.Context, email string) (bool, error) {
	var exists bool
	err := s.db.QueryRow(ctx, "SELECT EXISTS(SELECT 1 FROM users WHERE email = $1)", email).Scan(&exists)
	return exists, err
}

func (s *PostgresUserStore) CreateUser(ctx context.Context, input CreateUserInput) (*UserRecord, error) {
	if input.Role == "" {
		input.Role = RoleCompanyUser
	}
	if !input.IsActive {
		input.IsActive = true
	}

	row := s.db.QueryRow(ctx,
		`INSERT INTO users (company_id, email, password_hash, name, role, is_active)
		 VALUES ($1, $2, $3, $4, $5, $6)
		 RETURNING id, email, password_hash, name, company_id, role, is_active`,
		input.CompanyID, input.Email, input.PasswordHash, input.Name, input.Role, input.IsActive,
	)

	var record UserRecord
	if err := row.Scan(
		&record.ID,
		&record.Email,
		&record.PasswordHash,
		&record.Name,
		&record.CompanyID,
		&record.Role,
		&record.IsActive,
	); err != nil {
		return nil, err
	}
	record.CompanyActive = true
	record.AdminTOTPEnabled = false
	return &record, nil
}

func (s *PostgresUserStore) GetUserByEmail(ctx context.Context, email string) (*UserRecord, error) {
	return s.getUser(ctx,
		`SELECT u.id,
		        u.email,
		        u.password_hash,
		        u.name,
		        u.company_id,
		        c.name,
		        COALESCE(u.role, 'company_user'),
		        COALESCE(u.is_active, TRUE),
		        COALESCE(c.is_active, TRUE),
		        COALESCE(u.admin_totp_enabled, FALSE),
		        NULLIF(BTRIM(u.admin_totp_secret), '')
		   FROM users u
		   LEFT JOIN companies c ON c.id = u.company_id
		  WHERE u.email = $1`,
		email,
	)
}

func (s *PostgresUserStore) GetUserByID(ctx context.Context, userID string) (*UserRecord, error) {
	return s.getUser(ctx,
		`SELECT u.id,
		        u.email,
		        u.password_hash,
		        u.name,
		        u.company_id,
		        c.name,
		        COALESCE(u.role, 'company_user'),
		        COALESCE(u.is_active, TRUE),
		        COALESCE(c.is_active, TRUE),
		        COALESCE(u.admin_totp_enabled, FALSE),
		        NULLIF(BTRIM(u.admin_totp_secret), '')
		   FROM users u
		   LEFT JOIN companies c ON c.id = u.company_id
		  WHERE u.id = $1`,
		userID,
	)
}

func (s *PostgresUserStore) GetMachineByID(ctx context.Context, machineID string) (*MachineRecord, error) {
	record := &MachineRecord{}
	err := s.db.QueryRow(ctx,
		`SELECT id, COALESCE(company_id::text, ''), name
		   FROM machines
		  WHERE id = $1`,
		machineID,
	).Scan(&record.ID, &record.CompanyID, &record.Name)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrMachineNotFound
		}
		return nil, err
	}
	return record, nil
}

func (s *PostgresUserStore) TouchUser(ctx context.Context, userID string, updatedAt time.Time) error {
	_, err := s.db.Exec(ctx, "UPDATE users SET updated_at = $1 WHERE id = $2", updatedAt, userID)
	return err
}

func (s *PostgresUserStore) EnableAdminTOTP(ctx context.Context, userID, secret string, updatedAt time.Time) error {
	_, err := s.db.Exec(ctx,
		`UPDATE users
		    SET admin_totp_secret = $1,
		        admin_totp_enabled = TRUE,
		        updated_at = $2
		  WHERE id = $3`,
		secret, updatedAt, userID,
	)
	return err
}

func (s *PostgresUserStore) getUser(ctx context.Context, query string, arg string) (*UserRecord, error) {
	record := &UserRecord{}
	err := s.db.QueryRow(ctx, query, arg).Scan(
		&record.ID,
		&record.Email,
		&record.PasswordHash,
		&record.Name,
		&record.CompanyID,
		&record.CompanyName,
		&record.Role,
		&record.IsActive,
		&record.CompanyActive,
		&record.AdminTOTPEnabled,
		&record.AdminTOTPSecret,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrUserNotFound
		}
		return nil, err
	}
	return record, nil
}
