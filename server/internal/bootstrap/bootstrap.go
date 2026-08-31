package bootstrap

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"strings"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

const defaultCompanyName = "Default Company"

type SuperAdminConfig struct {
	Email    string
	Password string
	Name     string
}

func SuperAdminConfigFromEnv() SuperAdminConfig {
	return SuperAdminConfig{
		Email:    strings.TrimSpace(strings.ToLower(os.Getenv("SUPER_ADMIN_EMAIL"))),
		Password: os.Getenv("SUPER_ADMIN_PASSWORD"),
		Name:     strings.TrimSpace(os.Getenv("SUPER_ADMIN_NAME")),
	}
}

func Migrate(ctx context.Context, db *pgxpool.Pool) error {
	noArgStatements := []string{
		`CREATE EXTENSION IF NOT EXISTS pgcrypto`,
		`CREATE TABLE IF NOT EXISTS companies (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			name VARCHAR(255) NOT NULL,
			is_active BOOLEAN NOT NULL DEFAULT TRUE,
			machine_limit INTEGER DEFAULT NULL,
			created_at TIMESTAMPTZ DEFAULT NOW(),
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS users (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			company_id UUID REFERENCES companies(id) ON DELETE SET NULL,
			email VARCHAR(255) UNIQUE NOT NULL,
			password_hash VARCHAR(255) NOT NULL,
			name VARCHAR(255) NOT NULL,
			role VARCHAR(50) NOT NULL DEFAULT 'company_user',
			is_active BOOLEAN NOT NULL DEFAULT TRUE,
			admin_totp_enabled BOOLEAN NOT NULL DEFAULT FALSE,
			admin_totp_secret VARCHAR(128),
			created_at TIMESTAMPTZ DEFAULT NOW(),
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS machine_groups (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
			name VARCHAR(255) NOT NULL,
			created_at TIMESTAMPTZ DEFAULT NOW(),
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS machine_subgroups (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
			group_id UUID NOT NULL REFERENCES machine_groups(id),
			name VARCHAR(255) NOT NULL,
			created_at TIMESTAMPTZ DEFAULT NOW(),
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS machines (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			user_id UUID REFERENCES users(id) ON DELETE CASCADE,
			company_id UUID REFERENCES companies(id) ON DELETE CASCADE,
			name VARCHAR(255) NOT NULL,
			machine_key VARCHAR(64) UNIQUE NOT NULL,
			os_info VARCHAR(255),
			mac_address VARCHAR(17),
			agent_version VARCHAR(50),
			group_id UUID REFERENCES machine_groups(id),
			subgroup_id UUID REFERENCES machine_subgroups(id),
			legal_name VARCHAR(255),
			trade_name VARCHAR(255),
			document_type VARCHAR(10),
			document_number VARCHAR(14),
			managers_only_access BOOLEAN NOT NULL DEFAULT FALSE,
			require_client_consent BOOLEAN NOT NULL DEFAULT FALSE,
			force_update BOOLEAN NOT NULL DEFAULT FALSE,
			last_heartbeat TIMESTAMPTZ,
			is_online BOOLEAN DEFAULT FALSE,
			created_at TIMESTAMPTZ DEFAULT NOW(),
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS sessions (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			user_id UUID REFERENCES users(id),
			company_id UUID REFERENCES companies(id) ON DELETE SET NULL,
			machine_id UUID REFERENCES machines(id) ON DELETE SET NULL,
			access_mode VARCHAR(32) NOT NULL DEFAULT 'interactive',
			status VARCHAR(32) NOT NULL DEFAULT 'active',
			started_at TIMESTAMPTZ DEFAULT NOW(),
			ended_at TIMESTAMPTZ,
			duration_sec INTEGER,
			ip_viewer INET,
			ip_host INET,
			last_activity TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS backup_configs (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			machine_id UUID UNIQUE REFERENCES machines(id) ON DELETE CASCADE,
			folders JSONB NOT NULL DEFAULT '[]',
			interval_hours INTEGER DEFAULT 2,
			retention_count INTEGER DEFAULT 7,
			enabled BOOLEAN DEFAULT TRUE,
			updated_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS backup_snapshots (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			machine_id UUID REFERENCES machines(id) ON DELETE CASCADE,
			storage_key VARCHAR(512) NOT NULL,
			size_bytes BIGINT,
			file_count INTEGER,
			created_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`ALTER TABLE users ADD COLUMN IF NOT EXISTS company_id UUID REFERENCES companies(id) ON DELETE SET NULL`,
		`ALTER TABLE users ADD COLUMN IF NOT EXISTS role VARCHAR(50) NOT NULL DEFAULT 'company_user'`,
		`ALTER TABLE users ADD COLUMN IF NOT EXISTS is_active BOOLEAN NOT NULL DEFAULT TRUE`,
		`ALTER TABLE users ADD COLUMN IF NOT EXISTS admin_totp_enabled BOOLEAN NOT NULL DEFAULT FALSE`,
		`ALTER TABLE users ADD COLUMN IF NOT EXISTS admin_totp_secret VARCHAR(128)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS company_id UUID REFERENCES companies(id) ON DELETE CASCADE`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS agent_version VARCHAR(50)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS group_id UUID REFERENCES machine_groups(id)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS subgroup_id UUID REFERENCES machine_subgroups(id)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS legal_name VARCHAR(255)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS trade_name VARCHAR(255)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS document_type VARCHAR(10)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS document_number VARCHAR(14)`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS managers_only_access BOOLEAN NOT NULL DEFAULT FALSE`,
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS require_client_consent BOOLEAN NOT NULL DEFAULT FALSE`,
		`ALTER TABLE sessions ADD COLUMN IF NOT EXISTS company_id UUID REFERENCES companies(id) ON DELETE SET NULL`,
		`ALTER TABLE sessions ADD COLUMN IF NOT EXISTS access_mode VARCHAR(32) NOT NULL DEFAULT 'interactive'`,
		`ALTER TABLE sessions ADD COLUMN IF NOT EXISTS status VARCHAR(32) NOT NULL DEFAULT 'active'`,
		`CREATE TABLE IF NOT EXISTS audit_events (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
			machine_id UUID NOT NULL REFERENCES machines(id) ON DELETE CASCADE,
			session_id UUID REFERENCES sessions(id) ON DELETE SET NULL,
			user_id UUID REFERENCES users(id) ON DELETE SET NULL,
			event_type VARCHAR(64) NOT NULL,
			status VARCHAR(32),
			summary TEXT NOT NULL,
			transfer_direction VARCHAR(16),
			transfer_root TEXT,
			transfer_item TEXT,
			transfer_total_bytes BIGINT,
			transfer_total_entries INTEGER,
			duration_sec INTEGER,
			error_message TEXT,
			created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
		)`,
		`CREATE UNIQUE INDEX IF NOT EXISTS idx_companies_name_unique ON companies ((LOWER(name)))`,
		`CREATE INDEX IF NOT EXISTS idx_users_company_id ON users(company_id)`,
		`CREATE INDEX IF NOT EXISTS idx_users_role ON users(role)`,
		`CREATE INDEX IF NOT EXISTS idx_machines_company_id ON machines(company_id)`,
		`CREATE INDEX IF NOT EXISTS idx_machines_group_id ON machines(group_id)`,
		`CREATE INDEX IF NOT EXISTS idx_machines_subgroup_id ON machines(subgroup_id)`,
		`CREATE INDEX IF NOT EXISTS idx_sessions_company_id ON sessions(company_id)`,
		`CREATE INDEX IF NOT EXISTS idx_sessions_company_started_at_report ON sessions(company_id, started_at DESC) WHERE started_at IS NOT NULL AND status IN ('active', 'ended')`,
		`CREATE INDEX IF NOT EXISTS idx_sessions_started_at_dashboard ON sessions(started_at DESC) WHERE started_at IS NOT NULL`,
		`CREATE INDEX IF NOT EXISTS idx_audit_events_company_machine_created_at ON audit_events(company_id, machine_id, created_at DESC)`,
		`CREATE INDEX IF NOT EXISTS idx_audit_events_session_id ON audit_events(session_id)`,
		`CREATE UNIQUE INDEX IF NOT EXISTS idx_machine_groups_company_name_unique ON machine_groups (company_id, LOWER(name))`,
		`CREATE UNIQUE INDEX IF NOT EXISTS idx_machine_subgroups_group_name_unique ON machine_subgroups (group_id, LOWER(name))`,
		`CREATE INDEX IF NOT EXISTS idx_machine_subgroups_company_id ON machine_subgroups(company_id)`,
		`CREATE INDEX IF NOT EXISTS idx_machine_subgroups_group_id ON machine_subgroups(group_id)`,
		`UPDATE users
		    SET role = COALESCE(NULLIF(role, ''), 'company_user'),
		        is_active = COALESCE(is_active, TRUE),
		        admin_totp_enabled = COALESCE(admin_totp_enabled, FALSE),
		        admin_totp_secret = NULLIF(BTRIM(COALESCE(admin_totp_secret, '')), '')`,
		`UPDATE sessions
		    SET access_mode = COALESCE(NULLIF(access_mode, ''), 'interactive')`,
		`UPDATE sessions
		    SET status = CASE
		        WHEN COALESCE(NULLIF(status, ''), '') <> '' THEN status
		        WHEN ended_at IS NOT NULL THEN 'ended'
		        ELSE 'active'
		    END`,
		`ALTER TABLE companies ADD COLUMN IF NOT EXISTS block_after_days INTEGER DEFAULT NULL`,
		`ALTER TABLE companies ADD COLUMN IF NOT EXISTS machine_limit INTEGER DEFAULT NULL`,
		`ALTER TABLE sessions DROP CONSTRAINT IF EXISTS sessions_machine_id_fkey`,
		`ALTER TABLE sessions ADD CONSTRAINT sessions_machine_id_fkey FOREIGN KEY (machine_id) REFERENCES machines(id) ON DELETE SET NULL`,
		`CREATE TABLE IF NOT EXISTS session_recordings (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			session_id UUID REFERENCES sessions(id) ON DELETE SET NULL,
			company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
			machine_id UUID NOT NULL REFERENCES machines(id) ON DELETE CASCADE,
			storage_key VARCHAR(512) NOT NULL,
			file_size BIGINT,
			duration_sec INTEGER,
			status VARCHAR(32) NOT NULL DEFAULT 'pending',
			created_at TIMESTAMPTZ DEFAULT NOW()
		)`,
		`CREATE INDEX IF NOT EXISTS idx_session_recordings_company_id ON session_recordings(company_id)`,
		`CREATE INDEX IF NOT EXISTS idx_session_recordings_machine_id ON session_recordings(machine_id)`,
		`CREATE INDEX IF NOT EXISTS idx_session_recordings_session_id ON session_recordings(session_id)`,
		// Denormalized names for preservation after deletion
		`ALTER TABLE audit_events ADD COLUMN IF NOT EXISTS machine_name TEXT`,
		`ALTER TABLE audit_events ADD COLUMN IF NOT EXISTS user_name_snapshot TEXT`,
		// Allow machine_id to be NULL (for deleted machines)
		`ALTER TABLE audit_events ALTER COLUMN machine_id DROP NOT NULL`,
		// Change machine_id FK from CASCADE to SET NULL
		`ALTER TABLE audit_events DROP CONSTRAINT IF EXISTS audit_events_machine_id_fkey`,
		`ALTER TABLE audit_events ADD CONSTRAINT audit_events_machine_id_fkey FOREIGN KEY (machine_id) REFERENCES machines(id) ON DELETE SET NULL`,
		// Backfill existing events
		`UPDATE audit_events ae SET machine_name = m.name FROM machines m WHERE ae.machine_id = m.id AND ae.machine_name IS NULL`,
		`UPDATE audit_events ae SET user_name_snapshot = u.name FROM users u WHERE ae.user_id = u.id AND ae.user_name_snapshot IS NULL`,
		// Company-level index for logs page
		`CREATE INDEX IF NOT EXISTS idx_audit_events_company_created_at ON audit_events(company_id, created_at DESC)`,
		// Force-update flag for remote host update trigger
		`ALTER TABLE machines ADD COLUMN IF NOT EXISTS force_update BOOLEAN NOT NULL DEFAULT FALSE`,
		// Concurrent session limit per company (NULL = unlimited)
		`ALTER TABLE companies ADD COLUMN IF NOT EXISTS concurrent_session_limit INTEGER DEFAULT NULL`,
		`ALTER TABLE companies ADD COLUMN IF NOT EXISTS machine_limit INTEGER DEFAULT NULL`,
		`CREATE INDEX IF NOT EXISTS idx_sessions_company_active ON sessions(company_id) WHERE status IN ('active', 'pending_consent')`,
	}

	withDefaultCompanyStatements := []string{
		`INSERT INTO companies (name, is_active)
		 SELECT $1::varchar(255), TRUE
		  WHERE (
		        EXISTS (SELECT 1 FROM users WHERE company_id IS NULL)
		     OR EXISTS (SELECT 1 FROM machines WHERE company_id IS NULL)
		     OR EXISTS (SELECT 1 FROM sessions WHERE company_id IS NULL)
		  ) AND NOT EXISTS (
		        SELECT 1 FROM companies WHERE LOWER(name) = LOWER($1::varchar(255))
		  )`,
		`UPDATE users
		    SET company_id = c.id
		   FROM (SELECT id FROM companies WHERE LOWER(name) = LOWER($1::varchar(255)) LIMIT 1) c
		  WHERE users.company_id IS NULL
		    AND COALESCE(users.role, 'company_user') <> 'super_admin'`,
	}

	postBackfillStatements := []string{
		`UPDATE machines m
		    SET company_id = u.company_id
		   FROM users u
		  WHERE m.company_id IS NULL
		    AND m.user_id = u.id
		    AND u.company_id IS NOT NULL`,
		`UPDATE machines
		    SET company_id = c.id
		   FROM (SELECT id FROM companies WHERE LOWER(name) = LOWER($1::varchar(255)) LIMIT 1) c
		  WHERE machines.company_id IS NULL`,
		`UPDATE sessions s
		    SET company_id = m.company_id
		   FROM machines m
		  WHERE s.company_id IS NULL
		    AND s.machine_id = m.id`,
		`UPDATE sessions s
		    SET company_id = u.company_id
		   FROM users u
		  WHERE s.company_id IS NULL
		    AND s.user_id = u.id`,
		`ALTER TABLE machines ALTER COLUMN company_id DROP NOT NULL`,
	}

	for _, stmt := range noArgStatements {
		if _, err := db.Exec(ctx, stmt); err != nil {
			return fmt.Errorf("bootstrap migration failed: %w", err)
		}
	}

	for _, stmt := range withDefaultCompanyStatements {
		if _, err := db.Exec(ctx, stmt, defaultCompanyName); err != nil {
			return fmt.Errorf("bootstrap migration failed: %w", err)
		}
	}

	for _, stmt := range postBackfillStatements {
		var err error
		if strings.Contains(stmt, "$1") {
			_, err = db.Exec(ctx, stmt, defaultCompanyName)
		} else {
			_, err = db.Exec(ctx, stmt)
		}
		if err != nil {
			return fmt.Errorf("bootstrap migration failed: %w", err)
		}
	}

	return nil
}

func EnsureSuperAdmin(ctx context.Context, db *pgxpool.Pool, users auth.UserStore, authSvc *auth.Service, cfg SuperAdminConfig) error {
	if cfg.Email == "" || cfg.Password == "" {
		slog.Warn("super admin seed skipped because SUPER_ADMIN_EMAIL or SUPER_ADMIN_PASSWORD is empty")
		return nil
	}

	if cfg.Name == "" {
		cfg.Name = "RemoteX Admin"
	}

	record, err := users.GetUserByEmail(ctx, cfg.Email)
	if err != nil && err != auth.ErrUserNotFound {
		return err
	}

	if record != nil {
		_, err = db.Exec(ctx,
			`UPDATE users
			    SET role = $2,
			        company_id = NULL,
			        is_active = TRUE,
			        name = COALESCE(NULLIF($3, ''), name),
			        updated_at = NOW()
			  WHERE id = $1`,
			record.ID, auth.RoleSuperAdmin, cfg.Name,
		)
		if err != nil {
			return err
		}
		slog.Info("super admin ensured from existing user", "email", cfg.Email, "user_id", record.ID)
		return nil
	}

	hash, err := authSvc.HashPassword(cfg.Password)
	if err != nil {
		return err
	}

	created, err := users.CreateUser(ctx, auth.CreateUserInput{
		Email:        cfg.Email,
		PasswordHash: hash,
		Name:         cfg.Name,
		Role:         auth.RoleSuperAdmin,
		IsActive:     true,
	})
	if err != nil {
		return err
	}

	slog.Info("super admin created", "email", cfg.Email, "user_id", created.ID)
	return nil
}
