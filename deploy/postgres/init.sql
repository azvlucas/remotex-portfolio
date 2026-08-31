-- RemoteX - Schema inicial
-- Executado automaticamente pelo container PostgreSQL na primeira inicializacao.

BEGIN;

CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- Empresas
CREATE TABLE companies (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name       VARCHAR(255) NOT NULL,
    is_active  BOOLEAN NOT NULL DEFAULT TRUE,
    concurrent_session_limit INTEGER,
    machine_limit INTEGER,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Contas de usuario
CREATE TABLE users (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id         UUID REFERENCES companies(id) ON DELETE SET NULL,
    email              VARCHAR(255) UNIQUE NOT NULL,
    password_hash      VARCHAR(255) NOT NULL,
    name               VARCHAR(255) NOT NULL,
    role               VARCHAR(50) NOT NULL DEFAULT 'company_user',
    is_active          BOOLEAN NOT NULL DEFAULT TRUE,
    admin_totp_enabled BOOLEAN NOT NULL DEFAULT FALSE,
    admin_totp_secret  VARCHAR(128),
    created_at         TIMESTAMPTZ DEFAULT NOW(),
    updated_at         TIMESTAMPTZ DEFAULT NOW()
);

-- Agrupadores de maquinas
CREATE TABLE machine_groups (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    name       VARCHAR(255) NOT NULL,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE machine_subgroups (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    group_id   UUID NOT NULL REFERENCES machine_groups(id),
    name       VARCHAR(255) NOT NULL,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Maquinas registradas
CREATE TABLE machines (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID REFERENCES users(id) ON DELETE CASCADE,
    company_id      UUID REFERENCES companies(id) ON DELETE CASCADE,
    name            VARCHAR(255) NOT NULL,
    machine_key     VARCHAR(64) UNIQUE NOT NULL,
    os_info         VARCHAR(255),
    mac_address     VARCHAR(17),
    agent_version   VARCHAR(50),
    group_id        UUID REFERENCES machine_groups(id),
    subgroup_id     UUID REFERENCES machine_subgroups(id),
    legal_name      VARCHAR(255),
    trade_name      VARCHAR(255),
    document_type   VARCHAR(10),
    document_number VARCHAR(14),
    managers_only_access BOOLEAN NOT NULL DEFAULT FALSE,
    require_client_consent BOOLEAN NOT NULL DEFAULT FALSE,
    force_update    BOOLEAN NOT NULL DEFAULT FALSE,
    last_heartbeat  TIMESTAMPTZ,
    is_online       BOOLEAN DEFAULT FALSE,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Sessoes de acesso remoto (auditoria)
CREATE TABLE sessions (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      UUID REFERENCES users(id),
    company_id   UUID REFERENCES companies(id) ON DELETE SET NULL,
    machine_id   UUID REFERENCES machines(id) ON DELETE SET NULL,
    access_mode  VARCHAR(32) NOT NULL DEFAULT 'interactive',
    status       VARCHAR(32) NOT NULL DEFAULT 'active',
    started_at   TIMESTAMPTZ DEFAULT NOW(),
    ended_at     TIMESTAMPTZ,
    duration_sec INTEGER,
    ip_viewer    INET,
    ip_host      INET,
    last_activity TIMESTAMPTZ DEFAULT NOW()
);

-- Configuracao de backup por maquina
CREATE TABLE backup_configs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    machine_id      UUID UNIQUE REFERENCES machines(id) ON DELETE CASCADE,
    folders         JSONB NOT NULL DEFAULT '[]',
    interval_hours  INTEGER DEFAULT 2,
    retention_count INTEGER DEFAULT 7,
    enabled         BOOLEAN DEFAULT TRUE,
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Snapshots de backup
CREATE TABLE backup_snapshots (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    machine_id  UUID REFERENCES machines(id) ON DELETE CASCADE,
    storage_key VARCHAR(512) NOT NULL,
    size_bytes  BIGINT,
    file_count  INTEGER,
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- Indices uteis
CREATE UNIQUE INDEX idx_companies_name_unique ON companies ((LOWER(name)));
CREATE INDEX idx_users_company_id ON users(company_id);
CREATE INDEX idx_users_role ON users(role);
CREATE INDEX idx_machines_user_id ON machines(user_id);
CREATE INDEX idx_machines_company_id ON machines(company_id);
CREATE INDEX idx_machines_online ON machines(is_online) WHERE is_online = TRUE;
CREATE INDEX idx_machines_group_id ON machines(group_id);
CREATE INDEX idx_machines_subgroup_id ON machines(subgroup_id);
CREATE INDEX idx_sessions_user_id ON sessions(user_id);
CREATE INDEX idx_sessions_machine_id ON sessions(machine_id);
CREATE INDEX idx_sessions_company_id ON sessions(company_id);
CREATE INDEX IF NOT EXISTS idx_sessions_company_started_at_report ON sessions(company_id, started_at DESC) WHERE started_at IS NOT NULL AND status IN ('active', 'ended');
CREATE INDEX IF NOT EXISTS idx_sessions_started_at_dashboard ON sessions(started_at DESC) WHERE started_at IS NOT NULL;
CREATE INDEX idx_sessions_company_active ON sessions(company_id) WHERE status IN ('active', 'pending_consent');
CREATE INDEX idx_backup_snapshots_machine_id ON backup_snapshots(machine_id);
CREATE UNIQUE INDEX idx_machine_groups_company_name_unique ON machine_groups (company_id, LOWER(name));
CREATE UNIQUE INDEX idx_machine_subgroups_group_name_unique ON machine_subgroups (group_id, LOWER(name));
CREATE INDEX idx_machine_subgroups_company_id ON machine_subgroups(company_id);
CREATE INDEX idx_machine_subgroups_group_id ON machine_subgroups(group_id);

COMMIT;
