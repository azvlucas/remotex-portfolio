package signaling

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

var ErrAccessDenied = errors.New("signaling access denied")

type AccessAuthorizer interface {
	AuthorizeSessionJoin(ctx context.Context, claims *auth.Claims, sessionID, role string) error
	AuthorizeChat(ctx context.Context, claims *auth.Claims, machineID string) error
}

type authorizationRepository interface {
	MachineBelongsToCompany(ctx context.Context, machineID, companyID string) (bool, error)
	ViewerHasSessionAccess(ctx context.Context, userID, companyID, machineID string) (bool, error)
}

type Authorizer struct {
	repo         authorizationRepository
	queryTimeout time.Duration
}

func NewAuthorizer(db *pgxpool.Pool) *Authorizer {
	return &Authorizer{
		repo:         postgresAuthorizationRepository{db: db},
		queryTimeout: 5 * time.Second,
	}
}

func newAuthorizerWithRepo(repo authorizationRepository) *Authorizer {
	return &Authorizer{
		repo:         repo,
		queryTimeout: 5 * time.Second,
	}
}

func (a *Authorizer) AuthorizeSessionJoin(ctx context.Context, claims *auth.Claims, sessionID, role string) error {
	if claims == nil || claims.CompanyID == "" || sessionID == "" {
		return ErrAccessDenied
	}

	authCtx, cancel := a.withTimeout(ctx)
	defer cancel()

	switch role {
	case RoleHost:
		return a.authorizeHostMachine(authCtx, claims, sessionID)
	case RoleViewer:
		if claims.IsMachine() || claims.UserID == "" {
			return ErrAccessDenied
		}

		allowed, err := a.repo.ViewerHasSessionAccess(authCtx, claims.UserID, claims.CompanyID, sessionID)
		if err != nil {
			return err
		}
		if !allowed {
			return ErrAccessDenied
		}
		return nil
	default:
		return ErrAccessDenied
	}
}

func (a *Authorizer) AuthorizeChat(ctx context.Context, claims *auth.Claims, machineID string) error {
	if claims == nil || claims.CompanyID == "" || machineID == "" {
		return ErrAccessDenied
	}

	authCtx, cancel := a.withTimeout(ctx)
	defer cancel()

	if claims.IsMachine() {
		return a.authorizeHostMachine(authCtx, claims, machineID)
	}
	if claims.UserID == "" {
		return ErrAccessDenied
	}

	allowed, err := a.repo.MachineBelongsToCompany(authCtx, machineID, claims.CompanyID)
	if err != nil {
		return err
	}
	if !allowed {
		return ErrAccessDenied
	}
	return nil
}

func (a *Authorizer) authorizeHostMachine(ctx context.Context, claims *auth.Claims, machineID string) error {
	if claims == nil || !claims.IsMachine() || claims.MachineID == "" || claims.MachineID != machineID {
		return ErrAccessDenied
	}

	allowed, err := a.repo.MachineBelongsToCompany(ctx, machineID, claims.CompanyID)
	if err != nil {
		return err
	}
	if !allowed {
		return ErrAccessDenied
	}
	return nil
}

func (a *Authorizer) withTimeout(ctx context.Context) (context.Context, context.CancelFunc) {
	if ctx == nil {
		return context.WithTimeout(context.Background(), a.queryTimeout)
	}
	return context.WithTimeout(ctx, a.queryTimeout)
}

type postgresAuthorizationRepository struct {
	db *pgxpool.Pool
}

func (r postgresAuthorizationRepository) MachineBelongsToCompany(ctx context.Context, machineID, companyID string) (bool, error) {
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

func (r postgresAuthorizationRepository) ViewerHasSessionAccess(ctx context.Context, userID, companyID, machineID string) (bool, error) {
	var exists bool
	err := r.db.QueryRow(ctx,
		`SELECT EXISTS(
			SELECT 1
			  FROM sessions s
			  JOIN machines m ON m.id = s.machine_id
			 WHERE s.user_id = $1
			   AND s.company_id = $2
			   AND s.machine_id = $3
			   AND m.company_id = $2
			   AND s.status IN ('active', 'pending_consent')
		)`,
		userID, companyID, machineID,
	).Scan(&exists)
	return exists, err
}
