package machines

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
)

type routeTestRepository struct {
	listCalls                 int
	deleteCalls               int
	heartbeatCalls            int
	lastHeartbeatMachineKey   string
	lastHeartbeatCompanyID    string
	lastHeartbeatAgentVersion string
	listResult                []Machine
	createResult              *Machine
	updateResult              *Machine
	machineLimit              *int
	machineLimitErr           error
	machineCandidates         []machinelimit.MachineCandidate
	machineCandidatesErr      error
}

func (r *routeTestRepository) List(context.Context, string) ([]Machine, error) {
	r.listCalls++
	if r.listResult != nil {
		return r.listResult, nil
	}
	return []Machine{}, nil
}

func (r *routeTestRepository) GetByID(_ context.Context, id, companyID string) (*Machine, error) {
	return &Machine{ID: id, CompanyID: companyID}, nil
}

func (r *routeTestRepository) Create(_ context.Context, userID, companyID, machineKey string, input MachineUpsertInput) (*Machine, error) {
	if r.createResult != nil {
		machine := *r.createResult
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
		return &machine, nil
	}
	return &Machine{
		ID:         "machine-1",
		UserID:     userID,
		CompanyID:  companyID,
		Name:       input.Name,
		MachineKey: machineKey,
	}, nil
}

func (r *routeTestRepository) Update(_ context.Context, id, companyID string, input MachineUpsertInput) (*Machine, error) {
	if r.updateResult != nil {
		machine := *r.updateResult
		if machine.ID == "" {
			machine.ID = id
		}
		if machine.CompanyID == "" {
			machine.CompanyID = companyID
		}
		if machine.Name == "" {
			machine.Name = input.Name
		}
		return &machine, nil
	}
	return &Machine{
		ID:        id,
		CompanyID: companyID,
		Name:      input.Name,
	}, nil
}

func (r *routeTestRepository) Delete(context.Context, string, string) error {
	r.deleteCalls++
	return nil
}

func (r *routeTestRepository) UpdateHeartbeat(_ context.Context, machineKey, companyID, agentVersion string) (bool, error) {
	r.heartbeatCalls++
	r.lastHeartbeatMachineKey = machineKey
	r.lastHeartbeatCompanyID = companyID
	r.lastHeartbeatAgentVersion = agentVersion
	return false, nil
}

func (r *routeTestRepository) SetForceUpdate(context.Context, string, string) error {
	return nil
}

func (r *routeTestRepository) MarkOffline(context.Context, time.Duration) (int64, error) {
	return 0, nil
}

func (r *routeTestRepository) GroupExists(context.Context, string, string) (bool, error) {
	return false, nil
}

func (r *routeTestRepository) GetSubgroup(context.Context, string, string) (*MachineSubgroupRef, error) {
	return nil, pgx.ErrNoRows
}

func (r *routeTestRepository) GetCompanyMachineLimit(context.Context, string) (*int, error) {
	if r.machineLimitErr != nil {
		return nil, r.machineLimitErr
	}
	return r.machineLimit, nil
}

func (r *routeTestRepository) ListCompanyMachineCandidates(context.Context, string) ([]machinelimit.MachineCandidate, error) {
	if r.machineCandidatesErr != nil {
		return nil, r.machineCandidatesErr
	}
	if r.machineCandidates != nil {
		return r.machineCandidates, nil
	}
	return []machinelimit.MachineCandidate{}, nil
}

func newTestMachineService(repo *routeTestRepository) *Service {
	return NewService(repo, machinelimit.NewService(repo))
}

type routeTestUserStore struct {
	record  *auth.UserRecord
	machine *auth.MachineRecord
}

func (s *routeTestUserStore) EmailExists(context.Context, string) (bool, error) {
	return false, nil
}

func (s *routeTestUserStore) CreateUser(context.Context, auth.CreateUserInput) (*auth.UserRecord, error) {
	return s.record, nil
}

func (s *routeTestUserStore) GetUserByEmail(context.Context, string) (*auth.UserRecord, error) {
	return s.record, nil
}

func (s *routeTestUserStore) GetUserByID(context.Context, string) (*auth.UserRecord, error) {
	if s.record == nil {
		return nil, auth.ErrUserNotFound
	}
	return s.record, nil
}

func (s *routeTestUserStore) GetMachineByID(context.Context, string) (*auth.MachineRecord, error) {
	if s.machine == nil {
		return nil, auth.ErrMachineNotFound
	}
	return s.machine, nil
}

func (s *routeTestUserStore) TouchUser(context.Context, string, time.Time) error {
	return nil
}

func (s *routeTestUserStore) EnableAdminTOTP(context.Context, string, string, time.Time) error {
	return nil
}

func newTestRouter(authSvc *auth.Service, userStore auth.UserStore, handler *Handler) http.Handler {
	router := chi.NewRouter()
	router.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Route("/machines", func(r chi.Router) {
			r.Group(func(r chi.Router) {
				r.Use(auth.RequireHumanCompanyMiddleware())
				handler.RegisterHumanRoutes(r)
			})
			r.Group(func(r chi.Router) {
				r.Use(auth.RequireCompanyMiddleware())
				handler.RegisterCompanyRoutes(r)
			})
		})
	})
	return router
}

func mustGenerateToken(t *testing.T, authSvc *auth.Service, subject auth.TokenSubject) string {
	t.Helper()

	pair, err := authSvc.GenerateTokenPair(subject)
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}

	return pair.AccessToken
}

func TestCompanyRoutesHeartbeatAllowsMachineTokenForOwnMachine(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	router := newTestRouter(authSvc, &routeTestUserStore{
		machine: &auth.MachineRecord{ID: "machine-1", CompanyID: "company-1", Name: "Host Machine"},
	}, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        auth.RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/heartbeat",
		strings.NewReader(`{"machine_key":"key-1","agent_version":"0.1.11"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	if repo.heartbeatCalls != 1 {
		t.Fatalf("heartbeatCalls = %d, want 1", repo.heartbeatCalls)
	}
	if repo.lastHeartbeatMachineKey != "key-1" {
		t.Fatalf("machine key = %q, want %q", repo.lastHeartbeatMachineKey, "key-1")
	}
	if repo.lastHeartbeatCompanyID != "company-1" {
		t.Fatalf("company id = %q, want %q", repo.lastHeartbeatCompanyID, "company-1")
	}
	if repo.lastHeartbeatAgentVersion != "0.1.11" {
		t.Fatalf("agent version = %q, want %q", repo.lastHeartbeatAgentVersion, "0.1.11")
	}
}

func TestCompanyRoutesHeartbeatRejectsDifferentMachineToken(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	router := newTestRouter(authSvc, &routeTestUserStore{
		machine: &auth.MachineRecord{ID: "machine-1", CompanyID: "company-1", Name: "Host Machine"},
	}, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        auth.RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-2/heartbeat",
		strings.NewReader(`{"machine_key":"key-1"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if repo.heartbeatCalls != 0 {
		t.Fatalf("heartbeatCalls = %d, want 0", repo.heartbeatCalls)
	}
}

func TestCompanyRoutesHeartbeatRequiresMachineKey(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	router := newTestRouter(authSvc, &routeTestUserStore{
		machine: &auth.MachineRecord{ID: "machine-1", CompanyID: "company-1", Name: "Host Machine"},
	}, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        auth.RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/heartbeat",
		strings.NewReader(`{"machine_key":""}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusBadRequest, w.Body.String())
	}
	if repo.heartbeatCalls != 0 {
		t.Fatalf("heartbeatCalls = %d, want 0", repo.heartbeatCalls)
	}
}

func TestHumanRoutesRejectMachineTokenForList(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	router := newTestRouter(authSvc, &routeTestUserStore{
		machine: &auth.MachineRecord{ID: "machine-1", CompanyID: "company-1", Name: "Host Machine"},
	}, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        auth.RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if repo.listCalls != 0 {
		t.Fatalf("listCalls = %d, want 0", repo.listCalls)
	}
}

func TestHumanRoutesListOmitsMachineKey(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	timestamp := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	repo := &routeTestRepository{
		listResult: []Machine{{
			ID:         "machine-1",
			UserID:     "user-1",
			CompanyID:  companyID,
			Name:       "Host Machine",
			MachineKey: "key-1",
			CreatedAt:  timestamp,
			UpdatedAt:  timestamp,
		}},
		machineCandidates: []machinelimit.MachineCandidate{{
			ID:        "machine-1",
			CompanyID: companyID,
			CreatedAt: timestamp,
		}},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	body := w.Body.String()
	if strings.Contains(body, "machine_key") {
		t.Fatalf("response leaked machine_key: %s", body)
	}
	if !strings.Contains(body, `"id":"machine-1"`) {
		t.Fatalf("response missing machine payload: %s", body)
	}
}

func TestHumanRoutesListIncludesBlockedStateFields(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	firstCreatedAt := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	secondCreatedAt := firstCreatedAt.Add(time.Minute)
	limit := 1
	repo := &routeTestRepository{
		listResult: []Machine{
			{ID: "machine-1", UserID: "user-1", CompanyID: companyID, Name: "Allowed Host", IsOnline: false, CreatedAt: firstCreatedAt, UpdatedAt: firstCreatedAt},
			{ID: "machine-2", UserID: "user-1", CompanyID: companyID, Name: "Blocked Host", IsOnline: true, CreatedAt: secondCreatedAt, UpdatedAt: secondCreatedAt},
		},
		machineLimit: &limit,
		machineCandidates: []machinelimit.MachineCandidate{
			{ID: "machine-1", CompanyID: companyID, CreatedAt: firstCreatedAt},
			{ID: "machine-2", CompanyID: companyID, CreatedAt: secondCreatedAt},
		},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	body := w.Body.Bytes()
	if strings.Contains(string(body), "machine_key") {
		t.Fatalf("response leaked machine_key: %s", string(body))
	}

	var resp []machineResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("unmarshal response: %v", err)
	}
	if len(resp) != 2 {
		t.Fatalf("len(resp) = %d, want 2", len(resp))
	}
	if resp[0].State != string(machinelimit.StateAllowed) || resp[0].BlockReason != "" {
		t.Fatalf("machine-1 state = %q/%q, want allowed/empty", resp[0].State, resp[0].BlockReason)
	}
	if resp[1].State != string(machinelimit.StateBlocked) || resp[1].BlockReason != string(machinelimit.BlockReasonMachineLimitExceeded) {
		t.Fatalf("machine-2 state = %q/%q, want blocked/machine_limit_exceeded", resp[1].State, resp[1].BlockReason)
	}
	if !resp[1].IsOnline {
		t.Fatalf("machine-2 IsOnline = false, want true")
	}
}

func TestHumanRoutesListFailsClosedOnMachineStateMismatch(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	timestamp := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	repo := &routeTestRepository{
		listResult: []Machine{{ID: "machine-1", UserID: "user-1", CompanyID: companyID, Name: "Host Machine", CreatedAt: timestamp, UpdatedAt: timestamp}},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusInternalServerError, w.Body.String())
	}
	if body := w.Body.String(); !strings.Contains(body, `"error":"internal error"`) {
		t.Fatalf("unexpected body: %s", body)
	}
}

func TestHumanRoutesCreateReturnsMachineKey(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	timestamp := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	repo := &routeTestRepository{
		createResult: &Machine{
			ID:         "machine-1",
			UserID:     "user-1",
			CompanyID:  companyID,
			Name:       "Host Machine",
			MachineKey: "bootstrap-key",
			CreatedAt:  timestamp,
			UpdatedAt:  timestamp,
		},
		machineCandidates: []machinelimit.MachineCandidate{{
			ID:        "machine-1",
			CompanyID: companyID,
			CreatedAt: timestamp,
		}},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/machines", strings.NewReader(`{"name":"Host Machine"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusCreated {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusCreated, w.Body.String())
	}
	body := w.Body.String()
	if !strings.Contains(body, `"machine_key":"bootstrap-key"`) {
		t.Fatalf("response missing bootstrap machine_key: %s", body)
	}
}

func TestHumanRoutesCreateIncludesBlockedStateFields(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	firstCreatedAt := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	secondCreatedAt := firstCreatedAt.Add(time.Minute)
	limit := 1
	repo := &routeTestRepository{
		createResult: &Machine{
			ID:         "machine-2",
			UserID:     "user-1",
			CompanyID:  companyID,
			Name:       "Blocked Host",
			MachineKey: "bootstrap-key",
			CreatedAt:  secondCreatedAt,
			UpdatedAt:  secondCreatedAt,
		},
		machineLimit: &limit,
		machineCandidates: []machinelimit.MachineCandidate{
			{ID: "machine-1", CompanyID: companyID, CreatedAt: firstCreatedAt},
			{ID: "machine-2", CompanyID: companyID, CreatedAt: secondCreatedAt},
		},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/machines", strings.NewReader(`{"name":"Blocked Host"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusCreated {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusCreated, w.Body.String())
	}

	var resp machineCreateResponse
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("unmarshal response: %v", err)
	}
	if resp.MachineKey != "bootstrap-key" {
		t.Fatalf("machine key = %q, want bootstrap-key", resp.MachineKey)
	}
	if resp.State != string(machinelimit.StateBlocked) || resp.BlockReason != string(machinelimit.BlockReasonMachineLimitExceeded) {
		t.Fatalf("state = %q/%q, want blocked/machine_limit_exceeded", resp.State, resp.BlockReason)
	}
}

func TestHumanRoutesCreateFailsClosedOnMachineStateError(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	timestamp := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	repo := &routeTestRepository{
		createResult: &Machine{
			ID:         "machine-1",
			UserID:     "user-1",
			CompanyID:  companyID,
			Name:       "Host Machine",
			MachineKey: "bootstrap-key",
			CreatedAt:  timestamp,
			UpdatedAt:  timestamp,
		},
		machineLimitErr: errors.New("boom"),
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/machines", strings.NewReader(`{"name":"Host Machine"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusInternalServerError, w.Body.String())
	}
	if body := w.Body.String(); !strings.Contains(body, `"error":"internal error"`) {
		t.Fatalf("unexpected body: %s", body)
	}
}

func TestHumanRoutesUpdateOmitsMachineKey(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	timestamp := time.Date(2026, time.January, 2, 3, 4, 5, 0, time.UTC)
	repo := &routeTestRepository{
		updateResult: &Machine{
			ID:         "machine-1",
			UserID:     "user-1",
			CompanyID:  companyID,
			Name:       "Host Machine",
			MachineKey: "key-1",
			CreatedAt:  timestamp,
			UpdatedAt:  timestamp,
		},
	}
	handler := NewHandler(newTestMachineService(repo), nil)
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPut, "/machines/machine-1", strings.NewReader(`{"name":"Host Machine"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	body := w.Body.String()
	if strings.Contains(body, "machine_key") {
		t.Fatalf("response leaked machine_key: %s", body)
	}
}

func TestCompanyRoutesHeartbeatRejectsHumanToken(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	companyID := "company-1"
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/heartbeat",
		strings.NewReader(`{"machine_key":"key-1","agent_version":"0.1.11"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if repo.heartbeatCalls != 0 {
		t.Fatalf("heartbeatCalls = %d, want 0", repo.heartbeatCalls)
	}
}

func TestCompanyRoutesAuditIsReachableByMachineToken(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	router := newTestRouter(authSvc, &routeTestUserStore{
		machine: &auth.MachineRecord{ID: "machine-1", CompanyID: "company-1", Name: "Host Machine"},
	}, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        auth.RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/audit",
		strings.NewReader(`{"event_type":"session_started"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusServiceUnavailable, w.Body.String())
	}
}

func TestCompanyRoutesAuditRejectsHumanToken(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	companyID := "company-1"
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/audit",
		strings.NewReader(`{"event_type":"session_started"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}

func TestHumanRoutesDeleteRequiresManagerRole(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	companyID := "company-1"
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "user@example.com",
			Name:          "Operator",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyUser,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodDelete, "/machines/machine-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if repo.deleteCalls != 0 {
		t.Fatalf("deleteCalls = %d, want 0", repo.deleteCalls)
	}
}

func TestHumanRoutesDeleteAllowsManagerRole(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &routeTestRepository{}
	handler := NewHandler(newTestMachineService(repo), nil)
	companyID := "company-1"
	userStore := &routeTestUserStore{
		record: &auth.UserRecord{
			ID:            "user-1",
			Email:         "manager@example.com",
			Name:          "Manager",
			CompanyID:     &companyID,
			Role:          auth.RoleCompanyManager,
			IsActive:      true,
			CompanyActive: true,
		},
	}
	router := newTestRouter(authSvc, userStore, handler)

	token := mustGenerateToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "manager@example.com",
		Name:      "Manager",
		Role:      auth.RoleCompanyManager,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodDelete, "/machines/machine-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNoContent, w.Body.String())
	}
	if repo.deleteCalls != 1 {
		t.Fatalf("deleteCalls = %d, want 1", repo.deleteCalls)
	}
}
