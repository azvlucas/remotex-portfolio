package users

import (
	"context"
	"errors"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

var (
	ErrUserNotFound   = errors.New("user not found")
	ErrDuplicateEmail = errors.New("duplicate email")
)

type UserInfo struct {
	ID        string    `json:"id"`
	Name      string    `json:"name"`
	Email     string    `json:"email"`
	Role      auth.Role `json:"role"`
	IsActive  bool      `json:"is_active"`
	CreatedAt time.Time `json:"created_at"`
}

type Service struct {
	db      *pgxpool.Pool
	authSvc *auth.Service
}

func NewService(db *pgxpool.Pool, authSvc *auth.Service) *Service {
	return &Service{db: db, authSvc: authSvc}
}

func (s *Service) ListCompanyUsers(ctx context.Context, companyID string) ([]UserInfo, error) {
	rows, err := s.db.Query(ctx,
		`SELECT id, name, email, role, is_active, created_at
		   FROM users
		  WHERE company_id = $1
		    AND role <> 'super_admin'
		  ORDER BY name`,
		companyID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var users []UserInfo
	for rows.Next() {
		var u UserInfo
		if err := rows.Scan(&u.ID, &u.Name, &u.Email, &u.Role, &u.IsActive, &u.CreatedAt); err != nil {
			return nil, err
		}
		users = append(users, u)
	}
	if users == nil {
		users = []UserInfo{}
	}
	return users, rows.Err()
}

func (s *Service) CreateCompanyUser(ctx context.Context, companyID, name, email, password string) (*UserInfo, error) {
	hash, err := s.authSvc.HashPassword(password)
	if err != nil {
		return nil, err
	}

	var user UserInfo
	err = s.db.QueryRow(ctx,
		`INSERT INTO users (company_id, email, password_hash, name, role, is_active)
		 SELECT c.id, $2, $3, $4, $5, TRUE
		   FROM companies c
		  WHERE c.id = $1
		 RETURNING id, name, email, role, is_active, created_at`,
		companyID,
		strings.TrimSpace(strings.ToLower(email)),
		hash,
		strings.TrimSpace(name),
		auth.RoleCompanyUser,
	).Scan(&user.ID, &user.Name, &user.Email, &user.Role, &user.IsActive, &user.CreatedAt)
	if err != nil {
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "23505" {
			return nil, ErrDuplicateEmail
		}
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, errors.New("company not found")
		}
		return nil, err
	}
	return &user, nil
}

func (s *Service) SetUserActive(ctx context.Context, userID, companyID string, active bool) error {
	tag, err := s.db.Exec(ctx,
		`UPDATE users
		    SET is_active = $3,
		        updated_at = NOW()
		  WHERE id = $1
		    AND company_id = $2
		    AND role <> 'super_admin'`,
		userID, companyID, active,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrUserNotFound
	}
	return nil
}

func (s *Service) ResetUserPassword(ctx context.Context, userID, companyID, newPassword string) error {
	hash, err := s.authSvc.HashPassword(newPassword)
	if err != nil {
		return err
	}

	tag, err := s.db.Exec(ctx,
		`UPDATE users
		    SET password_hash = $3,
		        updated_at = NOW()
		  WHERE id = $1
		    AND company_id = $2
		    AND role <> 'super_admin'`,
		userID, companyID, hash,
	)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrUserNotFound
	}
	return nil
}
