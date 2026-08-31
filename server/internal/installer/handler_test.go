package installer

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
	"github.com/azvlucas/remotex-portfolio/server/internal/machines"
)

type enrollTestRepository struct {
	machineLimit         *int
	machineLimitErr      error
	machineCandidates    []machinelimit.MachineCandidate
	machineCandidatesErr error
	createResult         *machines.Machine
	createErr            error
	lastCreateUserID     string
	lastCreateCompanyID  string
	lastCreateInput      machines.MachineUpsertInput
}

func (r *enrollTestRepository) List(context.Context, string) ([]machines.Machine, error) {
	return []machines.Machine{}, nil
}

func (r *enrollTestRepository) GetByID(context.Context, string, string) (*machines.Machine, error) {
	return nil, pgx.ErrNoRows
}

func (r *enrollTestRepository) Create(_ context.Context, userID, companyID, machineKey string, input machines.MachineUpsertInput) (*machines.Machine, error) {
	r.lastCreateUserID = userID
	r.lastCreateCompanyID = companyID
	r.lastCreateInput = input

	if r.createErr != nil {
		return nil, r.createErr
	}

	createdAt := time.Now().UTC()
	machine := machines.Machine{
		ID:         "machine-created",
		UserID:     userID,
		CompanyID:  companyID,
		Name:       input.Name,
		MachineKey: machineKey,
		OSInfo:     input.OSInfo,
		CreatedAt:  createdAt,
	}
	if r.createResult != nil {
		machine = *r.createResult
		if machine.ID == "" {
			machine.ID = "machine-created"
		}
		if machine.UserID == "" {
			machine.UserID = userID
		}
		if machine.CompanyID == "" {
			machine.CompanyID = companyID
		}
		if machine.Name == "" {
			machine.Name = input.Name
		}
		if machine.MachineKey == "" {
			machine.MachineKey = machineKey
		}
		if machine.OSInfo == nil {
			machine.OSInfo = input.OSInfo
		}
		if machine.CreatedAt.IsZero() {
			machine.CreatedAt = createdAt
		}
	}

	r.machineCandidates = append(r.machineCandidates, machinelimit.MachineCandidate{
		ID:        machine.ID,
		CompanyID: machine.CompanyID,
		CreatedAt: machine.CreatedAt,
	})

	return &machine, nil
}

func (r *enrollTestRepository) Update(context.Context, string, string, machines.MachineUpsertInput) (*machines.Machine, error) {
	return nil, nil
}

func (r *enrollTestRepository) Delete(context.Context, string, string) error {
	return nil
}

func (r *enrollTestRepository) UpdateHeartbeat(context.Context, string, string, string) (bool, error) {
	return false, nil
}

func (r *enrollTestRepository) SetForceUpdate(context.Context, string, string) error {
	return nil
}

func (r *enrollTestRepository) MarkOffline(context.Context, time.Duration) (int64, error) {
	return 0, nil
}

func (r *enrollTestRepository) GroupExists(context.Context, string, string) (bool, error) {
	return false, nil
}

func (r *enrollTestRepository) GetSubgroup(context.Context, string, string) (*machines.MachineSubgroupRef, error) {
	return nil, pgx.ErrNoRows
}

func (r *enrollTestRepository) GetCompanyMachineLimit(context.Context, string) (*int, error) {
	if r.machineLimitErr != nil {
		return nil, r.machineLimitErr
	}
	return r.machineLimit, nil
}

func (r *enrollTestRepository) ListCompanyMachineCandidates(context.Context, string) ([]machinelimit.MachineCandidate, error) {
	if r.machineCandidatesErr != nil {
		return nil, r.machineCandidatesErr
	}
	if r.machineCandidates == nil {
		return []machinelimit.MachineCandidate{}, nil
	}
	candidates := make([]machinelimit.MachineCandidate, len(r.machineCandidates))
	copy(candidates, r.machineCandidates)
	return candidates, nil
}

func newTestEnrollHandler(repo *enrollTestRepository) (*Handler, *auth.Service) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	machineSvc := machines.NewService(repo, machinelimit.NewService(repo))
	return NewHandler(nil, authSvc, machineSvc), authSvc
}

func mustGenerateEnrollmentToken(t *testing.T, authSvc *auth.Service, subject auth.TokenSubject, ttl time.Duration) string {
	t.Helper()

	token, err := authSvc.GenerateEnrollmentToken(subject, ttl)
	if err != nil {
		t.Fatalf("GenerateEnrollmentToken() error = %v", err)
	}
	return token
}

func performEnrollRequest(t *testing.T, handler *Handler, body string) *httptest.ResponseRecorder {
	t.Helper()

	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/enroll", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")

	handler.PublicRoutes().ServeHTTP(recorder, request)
	return recorder
}

func TestEnrollReturnsBlockedMachineMetadataAndCredentials(t *testing.T) {
	limit := 1
	repo := &enrollTestRepository{
		machineLimit: &limit,
		machineCandidates: []machinelimit.MachineCandidate{
			{ID: "machine-older", CompanyID: "company-1", CreatedAt: time.Now().Add(-time.Hour).UTC()},
		},
		createResult: &machines.Machine{ID: "machine-blocked"},
	}
	handler, authSvc := newTestEnrollHandler(repo)
	token := mustGenerateEnrollmentToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "ops@example.com",
		Name:      "Ops",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	}, time.Hour)

	body, err := json.Marshal(enrollRequest{
		EnrollmentToken: token,
		MachineName:     "Blocked Host",
		OSInfo:          "Windows 11",
	})
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}

	response := performEnrollRequest(t, handler, string(body))
	if response.Code != http.StatusCreated {
		t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
	}
	if got := response.Header().Get("Content-Type"); !strings.Contains(got, "application/json") {
		t.Fatalf("Content-Type = %q, want application/json", got)
	}

	var payload enrollResponse
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}

	if payload.MachineID != "machine-blocked" {
		t.Fatalf("machine_id = %q, want %q", payload.MachineID, "machine-blocked")
	}
	if payload.UserID != payload.MachineID {
		t.Fatalf("user_id = %q, want machine_id %q", payload.UserID, payload.MachineID)
	}
	if payload.CompanyID != "company-1" {
		t.Fatalf("company_id = %q, want company-1", payload.CompanyID)
	}
	if payload.Role != auth.RoleMachineAgent {
		t.Fatalf("role = %q, want %q", payload.Role, auth.RoleMachineAgent)
	}
	if payload.State != machinelimit.StateBlocked {
		t.Fatalf("state = %q, want %q", payload.State, machinelimit.StateBlocked)
	}
	if payload.BlockReason != machinelimit.BlockReasonMachineLimitExceeded {
		t.Fatalf("block_reason = %q, want %q", payload.BlockReason, machinelimit.BlockReasonMachineLimitExceeded)
	}
	if payload.MachineKey == "" {
		t.Fatal("machine_key should not be empty")
	}
	if payload.AccessToken == "" || payload.RefreshToken == "" {
		t.Fatal("access_token and refresh_token should not be empty")
	}
	if payload.ExpiresIn <= 0 {
		t.Fatalf("expires_in = %d, want > 0", payload.ExpiresIn)
	}

	claims, err := authSvc.ValidateToken(payload.AccessToken)
	if err != nil {
		t.Fatalf("ValidateToken(access_token) error = %v", err)
	}
	if claims.SubjectType != auth.SubjectTypeMachine {
		t.Fatalf("access token subject_type = %q, want %q", claims.SubjectType, auth.SubjectTypeMachine)
	}
	if claims.MachineID != payload.MachineID {
		t.Fatalf("access token machine_id = %q, want %q", claims.MachineID, payload.MachineID)
	}
	if claims.CompanyID != payload.CompanyID {
		t.Fatalf("access token company_id = %q, want %q", claims.CompanyID, payload.CompanyID)
	}
	if claims.Role != auth.RoleMachineAgent {
		t.Fatalf("access token role = %q, want %q", claims.Role, auth.RoleMachineAgent)
	}
}

func TestEnrollRejectsInvalidRequestBody(t *testing.T) {
	handler, _ := newTestEnrollHandler(&enrollTestRepository{})

	response := performEnrollRequest(t, handler, "{")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `{"error":"invalid request body"}`) {
		t.Fatalf("body = %q, want invalid request body error", response.Body.String())
	}
}

func TestEnrollRejectsBlankEnrollmentToken(t *testing.T) {
	handler, _ := newTestEnrollHandler(&enrollTestRepository{})

	body, err := json.Marshal(enrollRequest{EnrollmentToken: "   ", MachineName: "Host"})
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}

	response := performEnrollRequest(t, handler, string(body))
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `{"error":"enrollment_token is required"}`) {
		t.Fatalf("body = %q, want enrollment_token is required error", response.Body.String())
	}
}

func TestEnrollRejectsInvalidOrExpiredEnrollmentToken(t *testing.T) {
	handler, authSvc := newTestEnrollHandler(&enrollTestRepository{})
	expiredToken := mustGenerateEnrollmentToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "ops@example.com",
		Name:      "Ops",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	}, -time.Minute)

	testCases := []struct {
		name  string
		token string
	}{
		{name: "invalid token", token: "not-a-token"},
		{name: "expired token", token: expiredToken},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			body, err := json.Marshal(enrollRequest{EnrollmentToken: tc.token, MachineName: "Host"})
			if err != nil {
				t.Fatalf("json.Marshal() error = %v", err)
			}

			response := performEnrollRequest(t, handler, string(body))
			if response.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
			}
			if !strings.Contains(response.Body.String(), `{"error":"invalid or expired enrollment token"}`) {
				t.Fatalf("body = %q, want invalid or expired enrollment token error", response.Body.String())
			}
		})
	}
}

func TestEnrollRequiresCompanyScopeInEnrollmentToken(t *testing.T) {
	handler, authSvc := newTestEnrollHandler(&enrollTestRepository{})
	token := mustGenerateEnrollmentToken(t, authSvc, auth.TokenSubject{
		UserID: "user-1",
		Email:  "ops@example.com",
		Name:   "Ops",
		Role:   auth.RoleCompanyUser,
	}, time.Hour)

	body, err := json.Marshal(enrollRequest{EnrollmentToken: token, MachineName: "Host"})
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}

	response := performEnrollRequest(t, handler, string(body))
	if response.Code != http.StatusForbidden {
		t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `{"error":"company scope missing from enrollment token"}`) {
		t.Fatalf("body = %q, want company scope missing error", response.Body.String())
	}
}

func TestEnrollReturnsInternalErrorWhenMachineCreateFails(t *testing.T) {
	handler, authSvc := newTestEnrollHandler(&enrollTestRepository{createErr: errors.New("boom")})
	token := mustGenerateEnrollmentToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "ops@example.com",
		Name:      "Ops",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	}, time.Hour)

	body, err := json.Marshal(enrollRequest{EnrollmentToken: token, MachineName: "Host"})
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}

	response := performEnrollRequest(t, handler, string(body))
	if response.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `{"error":"failed to enroll host"}`) {
		t.Fatalf("body = %q, want failed to enroll host error", response.Body.String())
	}
}
