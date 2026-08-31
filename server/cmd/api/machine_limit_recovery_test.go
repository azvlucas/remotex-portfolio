package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"
	"unsafe"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/admin"
	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/installer"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
	"github.com/azvlucas/remotex-portfolio/server/internal/machines"
	"github.com/azvlucas/remotex-portfolio/server/internal/sessions"
)

const machineBlockedByLimitMessage = "Esta maquina esta bloqueada: Limite de maquinas excedido."

func TestMachineLimitRecoveryContract(t *testing.T) {
	t.Run("limit_increase", func(t *testing.T) {
		h := newMachineLimitRecoveryHarness(t)
		proof := h.createCompanyAndManager("limit-increase")
		h.updateMachineLimit(proof.company.ID, 1)

		machineA := h.enrollMachine(proof, "limit-A")
		machineB := h.enrollMachine(proof, "limit-B")
		machineC := h.enrollMachine(proof, "limit-C")
		h.heartbeatMachine(machineA)
		h.heartbeatMachine(machineB)
		h.heartbeatMachine(machineC)

		initialHuman := h.listMachines(proof.accessToken)
		assertCreatedOrder(t, initialHuman, machineA.ID, machineB.ID, machineC.ID)
		assertHumanStates(t, initialHuman, map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		assertAdminStates(t, h.searchAdminMachines(proof.company.Name), map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		h.assertSessionBlocked(proof.accessToken, machineB.ID, machineB.Name)

		h.updateMachineLimit(proof.company.ID, 2)

		releasedHuman := h.listMachines(proof.accessToken)
		assertHumanStates(t, releasedHuman, map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		assertAdminStates(t, h.searchAdminMachines(proof.company.Name), map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		h.assertSessionCreated(proof.accessToken, machineB.ID, machineB.Name)
		h.assertSessionBlocked(proof.accessToken, machineC.ID, machineC.Name)
	})

	t.Run("machine_removal", func(t *testing.T) {
		h := newMachineLimitRecoveryHarness(t)
		proof := h.createCompanyAndManager("machine-removal")
		h.updateMachineLimit(proof.company.ID, 1)

		machineA := h.enrollMachine(proof, "remove-A")
		machineB := h.enrollMachine(proof, "remove-B")
		machineC := h.enrollMachine(proof, "remove-C")
		h.heartbeatMachine(machineA)
		h.heartbeatMachine(machineB)
		h.heartbeatMachine(machineC)

		initialHuman := h.listMachines(proof.accessToken)
		assertCreatedOrder(t, initialHuman, machineA.ID, machineB.ID, machineC.ID)
		assertHumanStates(t, initialHuman, map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		assertAdminStates(t, h.searchAdminMachines(proof.company.Name), map[string]expectedMachineState{
			machineA.ID: {name: machineA.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineB.ID: {name: machineB.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		h.assertSessionBlocked(proof.accessToken, machineB.ID, machineB.Name)

		h.detachMachine(machineA.ID)

		releasedHuman := h.listMachines(proof.accessToken)
		assertHumanStates(t, releasedHuman, map[string]expectedMachineState{
			machineB.ID: {name: machineB.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		assertAdminStates(t, h.searchAdminMachines(proof.company.Name), map[string]expectedMachineState{
			machineB.ID: {name: machineB.Name, state: machinelimit.StateAllowed, reason: machinelimit.BlockReasonNone, isOnline: true},
			machineC.ID: {name: machineC.Name, state: machinelimit.StateBlocked, reason: machinelimit.BlockReasonMachineLimitExceeded, isOnline: true},
		})
		h.assertSessionCreated(proof.accessToken, machineB.ID, machineB.Name)
		h.assertSessionBlocked(proof.accessToken, machineC.ID, machineC.Name)
	})

	t.Run("heartbeat_requires_machine_key", func(t *testing.T) {
		h := newMachineLimitRecoveryHarness(t)
		proof := h.createCompanyAndManager("heartbeat-key")
		machine := h.enrollMachine(proof, "heartbeat-key-host")

		resp := h.startHeartbeat(machine.ID, machine.AccessToken, "")
		if resp.Code != http.StatusBadRequest {
			t.Fatalf("/machines/%s/heartbeat status = %d, want %d, body = %s", machine.ID, resp.Code, http.StatusBadRequest, resp.Body.String())
		}
	})

	t.Run("heartbeat_rejects_wrong_machine_identity", func(t *testing.T) {
		h := newMachineLimitRecoveryHarness(t)
		proof := h.createCompanyAndManager("heartbeat-identity")
		machineA := h.enrollMachine(proof, "heartbeat-identity-A")
		machineB := h.enrollMachine(proof, "heartbeat-identity-B")

		resp := h.startHeartbeat(machineB.ID, machineA.AccessToken, machineB.MachineKey)
		if resp.Code != http.StatusForbidden {
			t.Fatalf("/machines/%s/heartbeat status = %d, want %d, body = %s", machineB.ID, resp.Code, http.StatusForbidden, resp.Body.String())
		}
	})
}

type machineLimitRecoveryHarness struct {
	t           *testing.T
	authSvc     *auth.Service
	store       *recoveryStore
	router      http.Handler
	adminCookie *http.Cookie
}

type companyProof struct {
	company      admin.CompanySummary
	manager      admin.CompanyUser
	managerEmail string
	managerPass  string
	accessToken  string
}

type enrolledMachine struct {
	ID          string
	Name        string
	MachineKey  string
	AccessToken string
}

type authLoginResponse struct {
	UserID      string    `json:"user_id"`
	Email       string    `json:"email"`
	Name        string    `json:"name"`
	Role        auth.Role `json:"role"`
	CompanyID   string    `json:"company_id"`
	AccessToken string    `json:"access_token"`
}

type enrollAPIResponse struct {
	MachineID   string                   `json:"machine_id"`
	MachineKey  string                   `json:"machine_key"`
	CompanyID   string                   `json:"company_id"`
	State       machinelimit.State       `json:"state"`
	BlockReason machinelimit.BlockReason `json:"block_reason"`
	AccessToken string                   `json:"access_token"`
}

type machineAPIResponse struct {
	ID          string                   `json:"id"`
	Name        string                   `json:"name"`
	IsOnline    bool                     `json:"is_online"`
	State       machinelimit.State       `json:"state"`
	BlockReason machinelimit.BlockReason `json:"block_reason"`
	CreatedAt   string                   `json:"created_at"`
}

type heartbeatAPIResponse struct {
	ForceUpdate bool `json:"force_update"`
}

type sessionAPIResponse struct {
	ID        string `json:"id"`
	MachineID string `json:"machine_id"`
	Status    string `json:"status"`
}

type errorResponse struct {
	Error string `json:"error"`
}

type expectedMachineState struct {
	name     string
	state    machinelimit.State
	reason   machinelimit.BlockReason
	isOnline bool
}

func newMachineLimitRecoveryHarness(t *testing.T) *machineLimitRecoveryHarness {
	t.Helper()

	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	store := newRecoveryStore(t)
	store.seedSuperAdmin(authSvc)

	machineLimitSvc := machinelimit.NewService(store)
	machineSvc := machines.NewService(store, machineLimitSvc)
	sessionSvc := sessions.NewService(store, nil, machineLimitSvc)

	authHandler := newRecoveryAuthHandler(authSvc, store)
	adminHandler := newRecoveryAdminHandler(&recoveryAdminService{
		t:               t,
		store:           store,
		authSvc:         authSvc,
		machineLimitSvc: machineLimitSvc,
	}, authSvc, store)
	installerHandler := installer.NewHandler(nil, authSvc, machineSvc)
	machineHandler := machines.NewHandler(machineSvc, nil)
	sessionHandler := sessions.NewHandler(sessionSvc)

	router := chi.NewRouter()
	router.Mount("/auth", authHandler.Routes())
	router.Mount("/admin", adminHandler.Routes())
	router.Route("/host", func(r chi.Router) {
		r.Post("/enroll", installerHandler.Enroll)
	})
	router.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, store))
		r.Route("/machines", func(r chi.Router) {
			r.Group(func(r chi.Router) {
				r.Use(auth.RequireHumanCompanyMiddleware())
				machineHandler.RegisterHumanRoutes(r)
			})
			r.Group(func(r chi.Router) {
				r.Use(auth.RequireCompanyMiddleware())
				machineHandler.RegisterCompanyRoutes(r)
			})
		})
		r.Group(func(r chi.Router) {
			r.Use(auth.RequireCompanyMiddleware())
			r.Mount("/sessions", sessionHandler.Routes())
		})
	})

	adminToken, err := authSvc.GenerateAdminSessionToken(auth.TokenSubject{
		UserID: "admin-1",
		Email:  "admin@example.com",
		Name:   "Admin",
		Role:   auth.RoleSuperAdmin,
	})
	if err != nil {
		t.Fatalf("GenerateAdminSessionToken() error = %v", err)
	}

	return &machineLimitRecoveryHarness{
		t:           t,
		authSvc:     authSvc,
		store:       store,
		router:      router,
		adminCookie: &http.Cookie{Name: "remotex_admin_token", Value: adminToken, Path: "/admin"},
	}
}

func (h *machineLimitRecoveryHarness) createCompanyAndManager(label string) companyProof {
	h.t.Helper()

	companyName := "Recovery " + label
	companyReq := h.newAdminFormRequest(http.MethodPost, "/admin/companies", url.Values{
		"name":       []string{companyName},
		"csrf_token": []string{"csrf-admin"},
	})
	companyResp := httptest.NewRecorder()
	h.router.ServeHTTP(companyResp, companyReq)
	if companyResp.Code != http.StatusCreated {
		h.t.Fatalf("/admin/companies status = %d, body = %s", companyResp.Code, companyResp.Body.String())
	}

	var company admin.CompanySummary
	decodeJSON(h.t, companyResp, &company)
	if company.ID == "" {
		h.t.Fatalf("/admin/companies returned empty id: %+v", company)
	}

	managerEmail := strings.ToLower(strings.ReplaceAll(label, "_", "-")) + "@example.com"
	managerPass := "secret123"
	userReq := h.newAdminFormRequest(http.MethodPost, "/admin/companies/"+company.ID+"/users", url.Values{
		"name":       []string{"Manager " + label},
		"email":      []string{managerEmail},
		"password":   []string{managerPass},
		"role":       []string{string(auth.RoleCompanyManager)},
		"csrf_token": []string{"csrf-admin"},
	})
	userResp := httptest.NewRecorder()
	h.router.ServeHTTP(userResp, userReq)
	if userResp.Code != http.StatusCreated {
		h.t.Fatalf("/admin/companies/%s/users status = %d, body = %s", company.ID, userResp.Code, userResp.Body.String())
	}

	var manager admin.CompanyUser
	decodeJSON(h.t, userResp, &manager)
	if manager.ID == "" {
		h.t.Fatalf("/admin/companies/%s/users returned empty id: %+v", company.ID, manager)
	}

	loginBody := strings.NewReader(fmt.Sprintf(`{"email":%q,"password":%q}`, managerEmail, managerPass))
	loginReq := httptest.NewRequest(http.MethodPost, "/auth/login", loginBody)
	loginReq.Header.Set("Content-Type", "application/json")
	loginReq.Header.Set("Accept", "application/json")
	loginResp := httptest.NewRecorder()
	h.router.ServeHTTP(loginResp, loginReq)
	if loginResp.Code != http.StatusOK {
		h.t.Fatalf("/auth/login status = %d, body = %s", loginResp.Code, loginResp.Body.String())
	}

	var loginPayload authLoginResponse
	decodeJSON(h.t, loginResp, &loginPayload)
	if loginPayload.AccessToken == "" {
		h.t.Fatalf("/auth/login missing access token: %+v", loginPayload)
	}
	if loginPayload.CompanyID != company.ID {
		h.t.Fatalf("/auth/login company_id = %q, want %q", loginPayload.CompanyID, company.ID)
	}

	return companyProof{
		company:      company,
		manager:      manager,
		managerEmail: managerEmail,
		managerPass:  managerPass,
		accessToken:  loginPayload.AccessToken,
	}
}

func (h *machineLimitRecoveryHarness) updateMachineLimit(companyID string, limit int) {
	h.t.Helper()

	req := h.newAdminFormRequest(http.MethodPost, "/admin/companies/"+companyID+"/machine-limit", url.Values{
		"machine_limit": {fmt.Sprintf("%d", limit)},
		"csrf_token":    {"csrf-admin"},
	})
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	if resp.Code != http.StatusOK {
		h.t.Fatalf("/admin/companies/%s/machine-limit status = %d, body = %s", companyID, resp.Code, resp.Body.String())
	}
}

func (h *machineLimitRecoveryHarness) enrollMachine(proof companyProof, machineName string) enrolledMachine {
	h.t.Helper()

	token, err := h.authSvc.GenerateEnrollmentToken(auth.TokenSubject{
		UserID:      proof.manager.ID,
		Email:       proof.managerEmail,
		Name:        proof.manager.Name,
		Role:        auth.RoleCompanyManager,
		CompanyID:   proof.company.ID,
		SubjectType: auth.SubjectTypeUser,
	}, time.Hour)
	if err != nil {
		h.t.Fatalf("GenerateEnrollmentToken() error = %v", err)
	}

	body := strings.NewReader(fmt.Sprintf(`{"enrollment_token":%q,"machine_name":%q}`, token, machineName))
	req := httptest.NewRequest(http.MethodPost, "/host/enroll", body)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	if resp.Code != http.StatusCreated {
		h.t.Fatalf("/host/enroll %s status = %d, body = %s", machineName, resp.Code, resp.Body.String())
	}

	var payload enrollAPIResponse
	decodeJSON(h.t, resp, &payload)
	if payload.MachineID == "" {
		h.t.Fatalf("/host/enroll %s missing machine id: %+v", machineName, payload)
	}
	if payload.MachineKey == "" {
		h.t.Fatalf("/host/enroll %s missing machine key: %+v", machineName, payload)
	}
	if payload.AccessToken == "" {
		h.t.Fatalf("/host/enroll %s missing access token: %+v", machineName, payload)
	}
	if payload.CompanyID != proof.company.ID {
		h.t.Fatalf("/host/enroll %s company_id = %q, want %q", machineName, payload.CompanyID, proof.company.ID)
	}

	claims, err := h.authSvc.ValidateToken(payload.AccessToken)
	if err != nil {
		h.t.Fatalf("ValidateToken(/host/enroll access_token) error = %v", err)
	}
	if claims.SubjectType != auth.SubjectTypeMachine {
		h.t.Fatalf("/host/enroll %s access token subject_type = %q, want %q", machineName, claims.SubjectType, auth.SubjectTypeMachine)
	}
	if claims.Role != auth.RoleMachineAgent {
		h.t.Fatalf("/host/enroll %s access token role = %q, want %q", machineName, claims.Role, auth.RoleMachineAgent)
	}
	if claims.MachineID != payload.MachineID {
		h.t.Fatalf("/host/enroll %s access token machine_id = %q, want %q", machineName, claims.MachineID, payload.MachineID)
	}
	if claims.CompanyID != proof.company.ID {
		h.t.Fatalf("/host/enroll %s access token company_id = %q, want %q", machineName, claims.CompanyID, proof.company.ID)
	}

	return enrolledMachine{ID: payload.MachineID, Name: machineName, MachineKey: payload.MachineKey, AccessToken: payload.AccessToken}
}

func (h *machineLimitRecoveryHarness) listMachines(accessToken string) []machineAPIResponse {
	h.t.Helper()

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Accept", "application/json")
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	if resp.Code != http.StatusOK {
		h.t.Fatalf("/machines status = %d, body = %s", resp.Code, resp.Body.String())
	}

	var payload []machineAPIResponse
	decodeJSON(h.t, resp, &payload)
	return payload
}

func (h *machineLimitRecoveryHarness) searchAdminMachines(query string) []admin.MachineSummary {
	h.t.Helper()

	req := httptest.NewRequest(http.MethodGet, "/admin/machines?q="+url.QueryEscape(query), nil)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Forwarded-Proto", "https")
	req.AddCookie(h.adminCookie)
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	if resp.Code != http.StatusOK {
		h.t.Fatalf("/admin/machines status = %d, body = %s", resp.Code, resp.Body.String())
	}

	var payload []admin.MachineSummary
	decodeJSON(h.t, resp, &payload)
	return payload
}

func (h *machineLimitRecoveryHarness) heartbeatMachine(machine enrolledMachine) {
	h.t.Helper()

	resp := h.startHeartbeat(machine.ID, machine.AccessToken, machine.MachineKey)
	if resp.Code != http.StatusOK {
		h.t.Fatalf("/machines/%s/heartbeat status = %d, body = %s", machine.ID, resp.Code, resp.Body.String())
	}

	var payload heartbeatAPIResponse
	decodeJSON(h.t, resp, &payload)
	if payload.ForceUpdate {
		h.t.Fatalf("/machines/%s/heartbeat force_update = true, want false", machine.ID)
	}
}

func (h *machineLimitRecoveryHarness) detachMachine(machineID string) {
	h.t.Helper()

	req := h.newAdminFormRequest(http.MethodPost, "/admin/machines/"+machineID+"/detach", url.Values{
		"csrf_token": {"csrf-admin"},
	})
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	if resp.Code != http.StatusOK {
		h.t.Fatalf("/admin/machines/%s/detach status = %d, body = %s", machineID, resp.Code, resp.Body.String())
	}
}

func (h *machineLimitRecoveryHarness) assertSessionBlocked(accessToken, machineID, machineName string) {
	h.t.Helper()

	resp := h.startSession(accessToken, machineID)
	if resp.Code != http.StatusForbidden {
		h.t.Fatalf("/sessions start %s (%s) status = %d, body = %s", machineName, machineID, resp.Code, resp.Body.String())
	}
	var payload errorResponse
	decodeJSON(h.t, resp, &payload)
	if payload.Error != machineBlockedByLimitMessage {
		h.t.Fatalf("/sessions start %s (%s) error = %q, want %q", machineName, machineID, payload.Error, machineBlockedByLimitMessage)
	}
}

func (h *machineLimitRecoveryHarness) assertSessionCreated(accessToken, machineID, machineName string) {
	h.t.Helper()

	resp := h.startSession(accessToken, machineID)
	if resp.Code != http.StatusCreated {
		h.t.Fatalf("/sessions start %s (%s) status = %d, body = %s", machineName, machineID, resp.Code, resp.Body.String())
	}
	var payload sessionAPIResponse
	decodeJSON(h.t, resp, &payload)
	if payload.MachineID != machineID {
		h.t.Fatalf("/sessions start %s returned machine_id = %q, want %q", machineName, payload.MachineID, machineID)
	}
	if payload.Status != sessions.SessionStatusActive {
		h.t.Fatalf("/sessions start %s returned status = %q, want %q", machineName, payload.Status, sessions.SessionStatusActive)
	}
}

func (h *machineLimitRecoveryHarness) startHeartbeat(machineID, accessToken, machineKey string) *httptest.ResponseRecorder {
	h.t.Helper()

	body := strings.NewReader(fmt.Sprintf(`{"machine_key":%q,"agent_version":%q}`, machineKey, "0.1.11"))
	req := httptest.NewRequest(http.MethodPost, "/machines/"+machineID+"/heartbeat", body)
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	return resp
}

func (h *machineLimitRecoveryHarness) startSession(accessToken, machineID string) *httptest.ResponseRecorder {
	h.t.Helper()

	body := strings.NewReader(fmt.Sprintf(`{"machine_id":%q,"access_mode":%q}`, machineID, sessions.AccessModeWatchOnly))
	req := httptest.NewRequest(http.MethodPost, "/sessions", body)
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	resp := httptest.NewRecorder()
	h.router.ServeHTTP(resp, req)
	return resp
}

func (h *machineLimitRecoveryHarness) newAdminFormRequest(method, path string, form url.Values) *http.Request {
	body := strings.NewReader(form.Encode())
	req := httptest.NewRequest(method, path, body)
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Forwarded-Proto", "https")
	req.AddCookie(h.adminCookie)
	req.AddCookie(&http.Cookie{Name: "remotex_csrf_token", Value: "csrf-admin", Path: "/admin"})
	return req
}

func decodeJSON(t *testing.T, resp *httptest.ResponseRecorder, target any) {
	t.Helper()
	if err := json.NewDecoder(resp.Body).Decode(target); err != nil {
		t.Fatalf("decode json: %v (body=%s)", err, resp.Body.String())
	}
}

func assertCreatedOrder(t *testing.T, machines []machineAPIResponse, machineIDs ...string) {
	t.Helper()
	index := make(map[string]time.Time, len(machines))
	for _, machine := range machines {
		createdAt, err := time.Parse(time.RFC3339, machine.CreatedAt)
		if err != nil {
			t.Fatalf("parse created_at for %s: %v", machine.ID, err)
		}
		index[machine.ID] = createdAt
	}
	for _, machineID := range machineIDs {
		if _, ok := index[machineID]; !ok {
			t.Fatalf("machine %s missing from /machines payload", machineID)
		}
	}
	for i := 1; i < len(machineIDs); i++ {
		if !index[machineIDs[i-1]].Before(index[machineIDs[i]]) {
			t.Fatalf("created_at order mismatch: %s=%s, %s=%s", machineIDs[i-1], index[machineIDs[i-1]], machineIDs[i], index[machineIDs[i]])
		}
	}
}

func assertHumanStates(t *testing.T, machines []machineAPIResponse, expected map[string]expectedMachineState) {
	t.Helper()
	if len(machines) != len(expected) {
		t.Fatalf("/machines returned %d items, want %d (%+v)", len(machines), len(expected), machines)
	}
	actual := make(map[string]machineAPIResponse, len(machines))
	for _, machine := range machines {
		actual[machine.ID] = machine
	}
	for machineID, want := range expected {
		machine, ok := actual[machineID]
		if !ok {
			t.Fatalf("/machines missing machine %s", machineID)
		}
		if machine.Name != want.name {
			t.Fatalf("/machines %s name = %q, want %q", machineID, machine.Name, want.name)
		}
		if machine.State != want.state || machine.BlockReason != want.reason {
			t.Fatalf("/machines %s state = %q/%q, want %q/%q", machineID, machine.State, machine.BlockReason, want.state, want.reason)
		}
		if machine.IsOnline != want.isOnline {
			t.Fatalf("/machines %s is_online = %t, want %t", machineID, machine.IsOnline, want.isOnline)
		}
	}
}

func assertAdminStates(t *testing.T, machines []admin.MachineSummary, expected map[string]expectedMachineState) {
	t.Helper()
	if len(machines) != len(expected) {
		t.Fatalf("/admin/machines returned %d items, want %d (%+v)", len(machines), len(expected), machines)
	}
	actual := make(map[string]admin.MachineSummary, len(machines))
	for _, machine := range machines {
		actual[machine.ID] = machine
	}
	for machineID, want := range expected {
		machine, ok := actual[machineID]
		if !ok {
			t.Fatalf("/admin/machines missing machine %s", machineID)
		}
		if machine.Name != want.name {
			t.Fatalf("/admin/machines %s name = %q, want %q", machineID, machine.Name, want.name)
		}
		if machine.State != want.state || machine.BlockReason != want.reason {
			t.Fatalf("/admin/machines %s state = %q/%q, want %q/%q", machineID, machine.State, machine.BlockReason, want.state, want.reason)
		}
		if machine.IsOnline != want.isOnline {
			t.Fatalf("/admin/machines %s is_online = %t, want %t", machineID, machine.IsOnline, want.isOnline)
		}
	}
}

type recoveryAdminService struct {
	t               *testing.T
	store           *recoveryStore
	authSvc         *auth.Service
	machineLimitSvc *machinelimit.Service
}

func (s *recoveryAdminService) ListCompanies(context.Context) ([]admin.CompanySummary, error) {
	s.t.Helper()
	s.t.Fatal("unexpected admin ListCompanies call")
	return nil, nil
}

func (s *recoveryAdminService) GetDashboardStats(context.Context) (admin.DashboardStats, error) {
	s.t.Helper()
	s.t.Fatal("unexpected admin GetDashboardStats call")
	return admin.DashboardStats{}, nil
}

func (s *recoveryAdminService) GetCompanyDetail(context.Context, string) (*admin.CompanyDetail, error) {
	s.t.Helper()
	s.t.Fatal("unexpected admin GetCompanyDetail call")
	return nil, nil
}

func (s *recoveryAdminService) SearchMachines(ctx context.Context, query string, limit int) ([]admin.MachineSummary, error) {
	query = strings.ToLower(strings.TrimSpace(query))
	if limit <= 0 {
		limit = 100
	}

	companies := s.store.snapshotCompanies()
	machinesByCompany := s.store.snapshotMachinesByCompany()
	var summaries []admin.MachineSummary
	for companyID, machines := range machinesByCompany {
		company, ok := companies[companyID]
		if !ok {
			continue
		}
		evaluation, err := s.machineLimitSvc.EvaluateCompany(ctx, companyID)
		if err != nil {
			return nil, err
		}
		states := make(map[string]machinelimit.MachineState, len(evaluation.Machines))
		for _, state := range evaluation.Machines {
			states[state.MachineID] = state
		}
		for _, machine := range machines {
			if query != "" && !strings.Contains(strings.ToLower(machine.Name), query) && !strings.Contains(strings.ToLower(company.Name), query) {
				continue
			}
			state := states[machine.ID]
			summaries = append(summaries, admin.MachineSummary{
				ID:            machine.ID,
				Name:          machine.Name,
				MachineKey:    machine.MachineKey,
				CompanyID:     companyID,
				CompanyName:   company.Name,
				IsOnline:      machine.IsOnline,
				LastHeartbeat: cloneTimePtr(machine.LastHeartbeat),
				UpdatedAt:     machine.UpdatedAt,
				State:         state.State,
				BlockReason:   state.BlockReason,
			})
		}
	}
	sort.Slice(summaries, func(i, j int) bool {
		if summaries[i].CompanyName == summaries[j].CompanyName {
			if summaries[i].Name == summaries[j].Name {
				return summaries[i].ID < summaries[j].ID
			}
			return summaries[i].Name < summaries[j].Name
		}
		return summaries[i].CompanyName < summaries[j].CompanyName
	})
	if len(summaries) > limit {
		summaries = summaries[:limit]
	}
	return summaries, nil
}

func (s *recoveryAdminService) DetachMachine(_ context.Context, machineID string) error {
	return s.store.detachMachine(machineID)
}

func (s *recoveryAdminService) UpdateCompanyBlockDays(_ context.Context, companyID string, days *int) error {
	return s.store.updateBlockDays(companyID, days)
}

func (s *recoveryAdminService) UpdateConcurrentSessionLimit(_ context.Context, companyID string, limit *int) error {
	return s.store.updateConcurrentLimit(companyID, limit)
}

func (s *recoveryAdminService) UpdateMachineLimit(_ context.Context, companyID string, limit *int) error {
	return s.store.updateMachineLimit(companyID, limit)
}

func (s *recoveryAdminService) CreateCompany(_ context.Context, name string) (*admin.CompanySummary, error) {
	company, err := s.store.createCompany(name)
	if err != nil {
		return nil, err
	}
	return company, nil
}

func (s *recoveryAdminService) CreateCompanyUserWithRole(_ context.Context, companyID, name, email, password string, role auth.Role) (*admin.CompanyUser, error) {
	hash, err := s.authSvc.HashPassword(password)
	if err != nil {
		return nil, err
	}
	return s.store.createCompanyUser(companyID, name, email, hash, role)
}

func (s *recoveryAdminService) SetUserActive(_ context.Context, userID string, active bool) error {
	return s.store.setUserActive(userID, active)
}

func (s *recoveryAdminService) UpdateUserRole(_ context.Context, userID string, role auth.Role) error {
	return s.store.updateUserRole(userID, role)
}

func (s *recoveryAdminService) SetCompanyActive(_ context.Context, companyID string, active bool) error {
	return s.store.setCompanyActive(companyID, active)
}

type recoveryStore struct {
	t            *testing.T
	mu           sync.Mutex
	now          time.Time
	seq          int
	companies    map[string]*recoveryCompany
	users        map[string]*recoveryUser
	usersByEmail map[string]string
	machines     map[string]*recoveryMachine
	sessions     map[string]*recoverySession
}

type recoveryCompany struct {
	ID                     string
	Name                   string
	IsActive               bool
	BlockAfterDays         *int
	ConcurrentSessionLimit *int
	MachineLimit           *int
	CreatedAt              time.Time
	UpdatedAt              time.Time
}

type recoveryUser struct {
	ID           string
	CompanyID    *string
	Email        string
	PasswordHash string
	Name         string
	Role         auth.Role
	IsActive     bool
	CreatedAt    time.Time
	UpdatedAt    time.Time
}

type recoveryMachine struct {
	ID                   string
	UserID               string
	CompanyID            string
	Name                 string
	MachineKey           string
	OSInfo               *string
	LastHeartbeat        *time.Time
	IsOnline             bool
	ManagersOnlyAccess   bool
	RequireClientConsent bool
	ForceUpdate          bool
	CreatedAt            time.Time
	UpdatedAt            time.Time
}

type recoverySession struct {
	ID          string
	UserID      string
	CompanyID   string
	MachineID   string
	AccessMode  string
	Status      string
	StartedAt   time.Time
	EndedAt     *time.Time
	DurationSec *int
	IPViewer    *string
	IPHost      *string
}

func newRecoveryStore(t *testing.T) *recoveryStore {
	return &recoveryStore{
		t:            t,
		now:          time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC),
		companies:    make(map[string]*recoveryCompany),
		users:        make(map[string]*recoveryUser),
		usersByEmail: make(map[string]string),
		machines:     make(map[string]*recoveryMachine),
		sessions:     make(map[string]*recoverySession),
	}
}

func (s *recoveryStore) seedSuperAdmin(authSvc *auth.Service) {
	s.mu.Lock()
	defer s.mu.Unlock()

	hash, err := authSvc.HashPassword("unused-admin-password")
	if err != nil {
		s.t.Fatalf("HashPassword(admin) error = %v", err)
	}
	createdAt := s.nextTimeLocked()
	s.users["admin-1"] = &recoveryUser{
		ID:           "admin-1",
		Email:        "admin@example.com",
		PasswordHash: hash,
		Name:         "Admin",
		Role:         auth.RoleSuperAdmin,
		IsActive:     true,
		CreatedAt:    createdAt,
		UpdatedAt:    createdAt,
	}
	s.usersByEmail["admin@example.com"] = "admin-1"
}

func (s *recoveryStore) EmailExists(_ context.Context, email string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.usersByEmail[strings.ToLower(strings.TrimSpace(email))]
	return ok, nil
}

func (s *recoveryStore) CreateUser(_ context.Context, input auth.CreateUserInput) (*auth.UserRecord, error) {
	companyID := ""
	if input.CompanyID != nil {
		companyID = *input.CompanyID
	}
	user, err := s.createCompanyUser(companyID, input.Name, input.Email, input.PasswordHash, input.Role)
	if err != nil {
		return nil, err
	}
	return s.GetUserByID(context.Background(), user.ID)
}

func (s *recoveryStore) GetUserByEmail(_ context.Context, email string) (*auth.UserRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	userID, ok := s.usersByEmail[strings.ToLower(strings.TrimSpace(email))]
	if !ok {
		return nil, auth.ErrUserNotFound
	}
	return s.authUserRecordLocked(s.users[userID])
}

func (s *recoveryStore) GetUserByID(_ context.Context, userID string) (*auth.UserRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	user, ok := s.users[userID]
	if !ok {
		return nil, auth.ErrUserNotFound
	}
	return s.authUserRecordLocked(user)
}

func (s *recoveryStore) GetMachineByID(_ context.Context, machineID string) (*auth.MachineRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[machineID]
	if !ok || strings.TrimSpace(machine.CompanyID) == "" {
		return nil, auth.ErrMachineNotFound
	}
	return &auth.MachineRecord{ID: machine.ID, CompanyID: machine.CompanyID, Name: machine.Name}, nil
}

func (s *recoveryStore) TouchUser(_ context.Context, userID string, updatedAt time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if user, ok := s.users[userID]; ok {
		user.UpdatedAt = updatedAt.UTC()
	}
	return nil
}

func (s *recoveryStore) EnableAdminTOTP(_ context.Context, userID, _ string, updatedAt time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if user, ok := s.users[userID]; ok {
		user.UpdatedAt = updatedAt.UTC()
		return nil
	}
	return auth.ErrUserNotFound
}

func (s *recoveryStore) List(_ context.Context, companyID string) ([]machines.Machine, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	list := make([]machines.Machine, 0)
	for _, machine := range s.machines {
		if machine.CompanyID == companyID {
			list = append(list, machine.toMachine())
		}
	}
	sort.Slice(list, func(i, j int) bool {
		if list[i].Name == list[j].Name {
			return list[i].ID < list[j].ID
		}
		return list[i].Name < list[j].Name
	})
	return list, nil
}

func (s *recoveryStore) GetByID(_ context.Context, id, companyID string) (*machines.Machine, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[id]
	if !ok || machine.CompanyID != companyID {
		return nil, pgx.ErrNoRows
	}
	clone := machine.toMachine()
	return &clone, nil
}

func (s *recoveryStore) Create(_ context.Context, userID, companyID, machineKey string, input machines.MachineUpsertInput) (*machines.Machine, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	createdAt := s.nextTimeLocked()
	machine := &recoveryMachine{
		ID:                   s.nextIDLocked("machine"),
		UserID:               userID,
		CompanyID:            companyID,
		Name:                 input.Name,
		MachineKey:           machineKey,
		OSInfo:               cloneStringPtr(input.OSInfo),
		ManagersOnlyAccess:   input.ManagersOnlyAccess,
		RequireClientConsent: input.RequireClientConsent,
		CreatedAt:            createdAt,
		UpdatedAt:            createdAt,
	}
	s.machines[machine.ID] = machine
	clone := machine.toMachine()
	return &clone, nil
}

func (s *recoveryStore) Update(_ context.Context, id, companyID string, input machines.MachineUpsertInput) (*machines.Machine, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[id]
	if !ok || machine.CompanyID != companyID {
		return nil, pgx.ErrNoRows
	}
	machine.Name = input.Name
	machine.OSInfo = cloneStringPtr(input.OSInfo)
	machine.ManagersOnlyAccess = input.ManagersOnlyAccess
	machine.RequireClientConsent = input.RequireClientConsent
	machine.UpdatedAt = s.nextTimeLocked()
	clone := machine.toMachine()
	return &clone, nil
}

func (s *recoveryStore) Delete(_ context.Context, id, companyID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[id]
	if !ok || machine.CompanyID != companyID {
		return pgx.ErrNoRows
	}
	delete(s.machines, id)
	return nil
}

func (s *recoveryStore) UpdateHeartbeat(_ context.Context, machineKey, companyID, agentVersion string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, machine := range s.machines {
		if machine.MachineKey == machineKey && machine.CompanyID == companyID {
			now := s.nextTimeLocked()
			machine.LastHeartbeat = &now
			machine.IsOnline = true
			machine.UpdatedAt = now
			return machine.ForceUpdate, nil
		}
	}
	return false, pgx.ErrNoRows
}

func (s *recoveryStore) SetForceUpdate(_ context.Context, id, companyID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[id]
	if !ok || machine.CompanyID != companyID {
		return pgx.ErrNoRows
	}
	machine.ForceUpdate = true
	machine.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) MarkOffline(_ context.Context, timeout time.Duration) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	cutoff := s.now.Add(-timeout)
	var count int64
	for _, machine := range s.machines {
		if machine.LastHeartbeat != nil && machine.LastHeartbeat.Before(cutoff) && machine.IsOnline {
			machine.IsOnline = false
			machine.UpdatedAt = s.nextTimeLocked()
			count++
		}
	}
	return count, nil
}

func (s *recoveryStore) GroupExists(context.Context, string, string) (bool, error) {
	return false, nil
}

func (s *recoveryStore) GetSubgroup(context.Context, string, string) (*machines.MachineSubgroupRef, error) {
	return nil, pgx.ErrNoRows
}

func (s *recoveryStore) GetCompanyMachineLimit(_ context.Context, companyID string) (*int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	company, ok := s.companies[companyID]
	if !ok {
		return nil, pgx.ErrNoRows
	}
	return cloneIntPtr(company.MachineLimit), nil
}

func (s *recoveryStore) ListCompanyMachineCandidates(_ context.Context, companyID string) ([]machinelimit.MachineCandidate, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	candidates := make([]machinelimit.MachineCandidate, 0)
	for _, machine := range s.machines {
		if machine.CompanyID == companyID {
			candidates = append(candidates, machinelimit.MachineCandidate{ID: machine.ID, CompanyID: companyID, CreatedAt: machine.CreatedAt})
		}
	}
	return candidates, nil
}

func (s *recoveryStore) Query(_ context.Context, sql string, _ ...any) (pgx.Rows, error) {
	s.t.Helper()
	s.t.Fatalf("unexpected Query call: %s", normalizeSQL(sql))
	return nil, nil
}

func (s *recoveryStore) QueryRow(_ context.Context, sql string, args ...any) pgx.Row {
	s.mu.Lock()
	defer s.mu.Unlock()
	query := normalizeSQL(sql)
	switch {
	case strings.Contains(query, "COALESCE(managers_only_access, FALSE)"):
		machineID, _ := args[0].(string)
		companyID, _ := args[1].(string)
		machine, ok := s.machines[machineID]
		if !ok || machine.CompanyID != companyID {
			return fakeRow{err: pgx.ErrNoRows}
		}
		return fakeRow{values: []any{machine.ManagersOnlyAccess, machine.RequireClientConsent}}
	case strings.Contains(query, "SELECT concurrent_session_limit FROM companies"):
		companyID, _ := args[0].(string)
		company, ok := s.companies[companyID]
		if !ok {
			return fakeRow{err: pgx.ErrNoRows}
		}
		return fakeRow{values: []any{cloneIntPtr(company.ConcurrentSessionLimit)}}
	case strings.Contains(query, "SELECT COUNT(*) FROM sessions"):
		companyID, _ := args[0].(string)
		count := 0
		for _, session := range s.sessions {
			if session.CompanyID == companyID && (session.Status == sessions.SessionStatusActive || session.Status == sessions.SessionStatusPendingConsent) {
				count++
			}
		}
		return fakeRow{values: []any{count}}
	case strings.Contains(query, "INSERT INTO sessions"):
		userID, _ := args[0].(string)
		companyID, _ := args[1].(string)
		machineID, _ := args[2].(string)
		accessMode, _ := args[5].(string)
		status, _ := args[6].(string)
		machine, ok := s.machines[machineID]
		if !ok || machine.CompanyID != companyID {
			return fakeRow{err: pgx.ErrNoRows}
		}
		startedAt := s.nextTimeLocked()
		session := &recoverySession{
			ID:         s.nextIDLocked("session"),
			UserID:     userID,
			CompanyID:  companyID,
			MachineID:  machineID,
			AccessMode: accessMode,
			Status:     status,
			StartedAt:  startedAt,
			IPViewer:   stringArgPtr(args[3]),
			IPHost:     stringArgPtr(args[4]),
		}
		s.sessions[session.ID] = session
		requesterName := ""
		if user, ok := s.users[userID]; ok {
			requesterName = user.Name
		}
		return fakeRow{values: []any{session.ID, session.UserID, session.CompanyID, session.MachineID, session.AccessMode, session.Status, session.StartedAt, session.EndedAt, session.DurationSec, session.IPViewer, session.IPHost, requesterName}}
	default:
		s.t.Helper()
		s.t.Fatalf("unexpected QueryRow call: %s", query)
		return fakeRow{err: errors.New("unexpected query")}
	}
}

func (s *recoveryStore) Exec(_ context.Context, sql string, _ ...any) (pgconn.CommandTag, error) {
	s.t.Helper()
	s.t.Fatalf("unexpected Exec call: %s", normalizeSQL(sql))
	return pgconn.CommandTag{}, nil
}

func (s *recoveryStore) createCompany(name string) (*admin.CompanySummary, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	name = strings.TrimSpace(name)
	for _, company := range s.companies {
		if strings.EqualFold(company.Name, name) {
			return nil, admin.ErrDuplicateName
		}
	}
	createdAt := s.nextTimeLocked()
	company := &recoveryCompany{
		ID:        s.nextIDLocked("company"),
		Name:      name,
		IsActive:  true,
		CreatedAt: createdAt,
		UpdatedAt: createdAt,
	}
	s.companies[company.ID] = company
	return &admin.CompanySummary{ID: company.ID, Name: company.Name, IsActive: company.IsActive, CreatedAt: company.CreatedAt, UpdatedAt: company.UpdatedAt}, nil
}

func (s *recoveryStore) createCompanyUser(companyID, name, email, passwordHash string, role auth.Role) (*admin.CompanyUser, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	company, ok := s.companies[companyID]
	if !ok {
		return nil, admin.ErrCompanyNotFound
	}
	if role != auth.RoleCompanyManager && role != auth.RoleCompanyUser {
		role = auth.RoleCompanyUser
	}
	normalizedEmail := strings.ToLower(strings.TrimSpace(email))
	if _, exists := s.usersByEmail[normalizedEmail]; exists {
		return nil, admin.ErrDuplicateEmail
	}
	createdAt := s.nextTimeLocked()
	companyIDCopy := companyID
	user := &recoveryUser{
		ID:           s.nextIDLocked("user"),
		CompanyID:    &companyIDCopy,
		Email:        normalizedEmail,
		PasswordHash: passwordHash,
		Name:         strings.TrimSpace(name),
		Role:         role,
		IsActive:     true,
		CreatedAt:    createdAt,
		UpdatedAt:    createdAt,
	}
	s.users[user.ID] = user
	s.usersByEmail[user.Email] = user.ID
	company.UpdatedAt = s.nextTimeLocked()
	return &admin.CompanyUser{ID: user.ID, CompanyID: companyID, Name: user.Name, Email: user.Email, Role: user.Role, IsActive: user.IsActive, CreatedAt: user.CreatedAt}, nil
}

func (s *recoveryStore) updateMachineLimit(companyID string, limit *int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	company, ok := s.companies[companyID]
	if !ok {
		return admin.ErrCompanyNotFound
	}
	company.MachineLimit = cloneIntPtr(limit)
	company.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) updateConcurrentLimit(companyID string, limit *int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	company, ok := s.companies[companyID]
	if !ok {
		return admin.ErrCompanyNotFound
	}
	company.ConcurrentSessionLimit = cloneIntPtr(limit)
	company.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) updateBlockDays(companyID string, days *int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	company, ok := s.companies[companyID]
	if !ok {
		return admin.ErrCompanyNotFound
	}
	company.BlockAfterDays = cloneIntPtr(days)
	company.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) detachMachine(machineID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	machine, ok := s.machines[machineID]
	if !ok {
		return admin.ErrMachineNotFound
	}
	machine.CompanyID = ""
	machine.UserID = ""
	machine.LastHeartbeat = nil
	machine.IsOnline = false
	machine.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) setUserActive(userID string, active bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	user, ok := s.users[userID]
	if !ok || user.Role == auth.RoleSuperAdmin {
		return admin.ErrUserNotFound
	}
	user.IsActive = active
	user.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) updateUserRole(userID string, role auth.Role) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	user, ok := s.users[userID]
	if !ok || user.Role == auth.RoleSuperAdmin {
		return admin.ErrUserNotFound
	}
	user.Role = role
	user.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) setCompanyActive(companyID string, active bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	company, ok := s.companies[companyID]
	if !ok {
		return admin.ErrCompanyNotFound
	}
	company.IsActive = active
	company.UpdatedAt = s.nextTimeLocked()
	return nil
}

func (s *recoveryStore) snapshotCompanies() map[string]recoveryCompany {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string]recoveryCompany, len(s.companies))
	for id, company := range s.companies {
		out[id] = *company
	}
	return out
}

func (s *recoveryStore) snapshotMachinesByCompany() map[string][]recoveryMachine {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[string][]recoveryMachine)
	for _, machine := range s.machines {
		if strings.TrimSpace(machine.CompanyID) == "" {
			continue
		}
		out[machine.CompanyID] = append(out[machine.CompanyID], *machine)
	}
	for companyID := range out {
		sort.Slice(out[companyID], func(i, j int) bool {
			if out[companyID][i].Name == out[companyID][j].Name {
				return out[companyID][i].ID < out[companyID][j].ID
			}
			return out[companyID][i].Name < out[companyID][j].Name
		})
	}
	return out
}

func (s *recoveryStore) authUserRecordLocked(user *recoveryUser) (*auth.UserRecord, error) {
	companyActive := true
	var companyName *string
	if user.CompanyID != nil {
		company, ok := s.companies[*user.CompanyID]
		if !ok {
			return nil, auth.ErrUserNotFound
		}
		companyActive = company.IsActive
		companyNameValue := company.Name
		companyName = &companyNameValue
	}
	return &auth.UserRecord{
		ID:               user.ID,
		Email:            user.Email,
		PasswordHash:     user.PasswordHash,
		Name:             user.Name,
		CompanyID:        cloneStringPtr(user.CompanyID),
		CompanyName:      companyName,
		Role:             user.Role,
		IsActive:         user.IsActive,
		CompanyActive:    companyActive,
		AdminTOTPEnabled: false,
	}, nil
}

func (s *recoveryStore) nextIDLocked(prefix string) string {
	s.seq++
	return fmt.Sprintf("%s-%02d", prefix, s.seq)
}

func (s *recoveryStore) nextTimeLocked() time.Time {
	current := s.now
	s.now = s.now.Add(time.Second)
	return current
}

func (m *recoveryMachine) toMachine() machines.Machine {
	return machines.Machine{
		ID:                   m.ID,
		UserID:               m.UserID,
		CompanyID:            m.CompanyID,
		Name:                 m.Name,
		MachineKey:           m.MachineKey,
		OSInfo:               cloneStringPtr(m.OSInfo),
		LastHeartbeat:        cloneTimePtr(m.LastHeartbeat),
		IsOnline:             m.IsOnline,
		ManagersOnlyAccess:   m.ManagersOnlyAccess,
		RequireClientConsent: m.RequireClientConsent,
		ForceUpdate:          m.ForceUpdate,
		CreatedAt:            m.CreatedAt,
		UpdatedAt:            m.UpdatedAt,
	}
}

type fakeRow struct {
	values []any
	err    error
}

func (r fakeRow) Scan(dest ...any) error {
	if r.err != nil {
		return r.err
	}
	return assignScan(dest, r.values)
}

func assignScan(dest []any, values []any) error {
	if len(dest) != len(values) {
		return fmt.Errorf("scan arg mismatch: got %d destinations for %d values", len(dest), len(values))
	}
	for i := range dest {
		if err := assignValue(dest[i], values[i]); err != nil {
			return fmt.Errorf("assign column %d: %w", i, err)
		}
	}
	return nil
}

func assignValue(dest any, value any) error {
	rv := reflect.ValueOf(dest)
	if rv.Kind() != reflect.Pointer || rv.IsNil() {
		return errors.New("destination must be a non-nil pointer")
	}

	target := rv.Elem()
	if value == nil {
		target.Set(reflect.Zero(target.Type()))
		return nil
	}

	valueRef := reflect.ValueOf(value)
	if target.Kind() == reflect.Pointer {
		ptr := reflect.New(target.Type().Elem())
		if valueRef.Type().AssignableTo(target.Type().Elem()) {
			ptr.Elem().Set(valueRef)
			target.Set(ptr)
			return nil
		}
		if valueRef.Type().ConvertibleTo(target.Type().Elem()) {
			ptr.Elem().Set(valueRef.Convert(target.Type().Elem()))
			target.Set(ptr)
			return nil
		}
	}

	if valueRef.Type().AssignableTo(target.Type()) {
		target.Set(valueRef)
		return nil
	}
	if valueRef.Type().ConvertibleTo(target.Type()) {
		target.Set(valueRef.Convert(target.Type()))
		return nil
	}
	return fmt.Errorf("cannot assign %T to %s", value, target.Type())
}

func newRecoveryAuthHandler(authSvc *auth.Service, users auth.UserStore) *auth.Handler {
	handler := new(auth.Handler)
	setUnexportedField(handler, "svc", authSvc)
	setUnexportedField(handler, "users", users)
	setUnexportedField(handler, "limiter", auth.NewBucketRateLimiter(
		auth.NewInMemoryRateLimitStore(auth.InMemoryRateLimitStoreConfig{MaxEntries: 32}),
		auth.RateLimiterConfig{MaxAttempts: 5, Window: time.Minute},
		auth.RateLimitBucketLogin,
	))
	setUnexportedField(handler, "queryTimeout", 5*time.Second)
	return handler
}

func newRecoveryAdminHandler(svc any, authSvc *auth.Service, users auth.UserStore) *admin.Handler {
	handler := new(admin.Handler)
	setUnexportedField(handler, "svc", svc)
	setUnexportedField(handler, "authSvc", authSvc)
	setUnexportedField(handler, "users", users)
	setUnexportedField(handler, "adminHost", "")
	setUnexportedField(handler, "adminHostConfigured", false)
	return handler
}

func setUnexportedField(target any, name string, value any) {
	field := reflect.ValueOf(target).Elem().FieldByName(name)
	reflect.NewAt(field.Type(), unsafe.Pointer(field.UnsafeAddr())).Elem().Set(reflect.ValueOf(value))
}

func normalizeSQL(sql string) string {
	return strings.Join(strings.Fields(sql), " ")
}

func cloneIntPtr(value *int) *int {
	if value == nil {
		return nil
	}
	clone := *value
	return &clone
}

func cloneStringPtr(value *string) *string {
	if value == nil {
		return nil
	}
	clone := *value
	return &clone
}

func cloneTimePtr(value *time.Time) *time.Time {
	if value == nil {
		return nil
	}
	clone := value.UTC()
	return &clone
}

func stringArgPtr(value any) *string {
	if value == nil {
		return nil
	}
	switch typed := value.(type) {
	case string:
		if strings.TrimSpace(typed) == "" {
			return nil
		}
		clone := typed
		return &clone
	case *string:
		return cloneStringPtr(typed)
	default:
		return nil
	}
}
