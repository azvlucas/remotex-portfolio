package signaling

import (
	"context"
	"testing"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

type fakeAuthorizationRepository struct {
	machineBelongsFn      func(ctx context.Context, machineID, companyID string) (bool, error)
	viewerSessionAccessFn func(ctx context.Context, userID, companyID, machineID string) (bool, error)
}

func (f fakeAuthorizationRepository) MachineBelongsToCompany(ctx context.Context, machineID, companyID string) (bool, error) {
	if f.machineBelongsFn != nil {
		return f.machineBelongsFn(ctx, machineID, companyID)
	}
	return false, nil
}

func (f fakeAuthorizationRepository) ViewerHasSessionAccess(ctx context.Context, userID, companyID, machineID string) (bool, error) {
	if f.viewerSessionAccessFn != nil {
		return f.viewerSessionAccessFn(ctx, userID, companyID, machineID)
	}
	return false, nil
}

func TestAuthorizerAllowsViewerOnlyForOwnAuthorizedSession(t *testing.T) {
	authorizer := newAuthorizerWithRepo(fakeAuthorizationRepository{
		viewerSessionAccessFn: func(_ context.Context, userID, companyID, machineID string) (bool, error) {
			return userID == "viewer-1" && companyID == "company-1" && machineID == "machine-1", nil
		},
	})

	err := authorizer.AuthorizeSessionJoin(context.Background(), &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-1",
	}, "machine-1", RoleViewer)
	if err != nil {
		t.Fatalf("AuthorizeSessionJoin() error = %v, want nil", err)
	}

	err = authorizer.AuthorizeSessionJoin(context.Background(), &auth.Claims{
		UserID:    "viewer-2",
		CompanyID: "company-1",
	}, "machine-1", RoleViewer)
	if err != ErrAccessDenied {
		t.Fatalf("AuthorizeSessionJoin() error = %v, want %v", err, ErrAccessDenied)
	}
}

func TestAuthorizerAllowsHostOnlyForOwnMachine(t *testing.T) {
	authorizer := newAuthorizerWithRepo(fakeAuthorizationRepository{
		machineBelongsFn: func(_ context.Context, machineID, companyID string) (bool, error) {
			return machineID == "machine-1" && companyID == "company-1", nil
		},
	})

	err := authorizer.AuthorizeSessionJoin(context.Background(), &auth.Claims{
		UserID:      "machine-1",
		MachineID:   "machine-1",
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
	}, "machine-1", RoleHost)
	if err != nil {
		t.Fatalf("AuthorizeSessionJoin() error = %v, want nil", err)
	}

	err = authorizer.AuthorizeSessionJoin(context.Background(), &auth.Claims{
		UserID:      "machine-1",
		MachineID:   "machine-1",
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
	}, "machine-2", RoleHost)
	if err != ErrAccessDenied {
		t.Fatalf("AuthorizeSessionJoin() error = %v, want %v", err, ErrAccessDenied)
	}
}

func TestAuthorizerChatRequiresMachineWithinCompany(t *testing.T) {
	authorizer := newAuthorizerWithRepo(fakeAuthorizationRepository{
		machineBelongsFn: func(_ context.Context, machineID, companyID string) (bool, error) {
			return machineID == "machine-1" && companyID == "company-1", nil
		},
	})

	err := authorizer.AuthorizeChat(context.Background(), &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-1",
	}, "machine-1")
	if err != nil {
		t.Fatalf("AuthorizeChat() error = %v, want nil", err)
	}

	err = authorizer.AuthorizeChat(context.Background(), &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-2",
	}, "machine-1")
	if err != ErrAccessDenied {
		t.Fatalf("AuthorizeChat() error = %v, want %v", err, ErrAccessDenied)
	}
}
