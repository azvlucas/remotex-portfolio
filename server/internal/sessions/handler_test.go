package sessions

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/relay"
)

type fakeSessionService struct {
	startErr               error
	getErr                 error
	resolveErr             error
	heartbeatErr           error
	lastStartUser          string
	lastStartCompany       string
	lastStartManager       bool
	lastStartReq           StartRequest
	getSession             *Session
	lastConsentSessionID   string
	lastConsentCompanyID   string
	lastConsentMachineID   string
	lastConsentDecision    string
	lastHeartbeatSessionID string
	lastHeartbeatCompanyID string
	lastHeartbeatUserID    string
	lastHeartbeatMachineID string
	lastHeartbeatIsMachine bool
}

func (f *fakeSessionService) List(context.Context, string, int, int) ([]Session, error) {
	return []Session{}, nil
}

func (f *fakeSessionService) Get(_ context.Context, sessionID, companyID string) (*Session, error) {
	if f.getErr != nil {
		return nil, f.getErr
	}
	if f.getSession != nil {
		return f.getSession, nil
	}
	return &Session{ID: sessionID, CompanyID: companyID, MachineID: "machine-1", Status: SessionStatusPendingConsent}, nil
}

func (f *fakeSessionService) Start(_ context.Context, userID, companyID string, isManager bool, req StartRequest) (*Session, error) {
	f.lastStartUser = userID
	f.lastStartCompany = companyID
	f.lastStartManager = isManager
	f.lastStartReq = req
	if f.startErr != nil {
		return nil, f.startErr
	}
	return &Session{
		ID:         "session-1",
		MachineID:  req.MachineID,
		CompanyID:  companyID,
		UserID:     userID,
		AccessMode: req.AccessMode,
		Status:     SessionStatusActive,
	}, nil
}

func (f *fakeSessionService) End(context.Context, string, string) (*Session, error) {
	return &Session{ID: "session-1", Status: SessionStatusEnded}, nil
}

func (f *fakeSessionService) ResolveConsent(_ context.Context, sessionID, companyID, machineID, decision string) (*Session, error) {
	f.lastConsentSessionID = sessionID
	f.lastConsentCompanyID = companyID
	f.lastConsentMachineID = machineID
	f.lastConsentDecision = decision
	if f.resolveErr != nil {
		return nil, f.resolveErr
	}
	return &Session{
		ID:        sessionID,
		CompanyID: companyID,
		MachineID: machineID,
		Status:    consentStatusFromDecision(decision),
	}, nil
}

func (f *fakeSessionService) Heartbeat(_ context.Context, sessionID, companyID, userID, machineID string, isMachine bool) error {
	f.lastHeartbeatSessionID = sessionID
	f.lastHeartbeatCompanyID = companyID
	f.lastHeartbeatUserID = userID
	f.lastHeartbeatMachineID = machineID
	f.lastHeartbeatIsMachine = isMachine
	return f.heartbeatErr
}

type sessionRouteTestUserStore struct {
	record *auth.UserRecord
}

func (s *sessionRouteTestUserStore) EmailExists(context.Context, string) (bool, error) {
	return false, nil
}
func (s *sessionRouteTestUserStore) CreateUser(context.Context, auth.CreateUserInput) (*auth.UserRecord, error) {
	return s.record, nil
}
func (s *sessionRouteTestUserStore) GetUserByEmail(context.Context, string) (*auth.UserRecord, error) {
	return s.record, nil
}
func (s *sessionRouteTestUserStore) GetUserByID(context.Context, string) (*auth.UserRecord, error) {
	return s.record, nil
}
func (s *sessionRouteTestUserStore) GetMachineByID(_ context.Context, machineID string) (*auth.MachineRecord, error) {
	return &auth.MachineRecord{ID: machineID, CompanyID: "company-1", Name: "Host Machine"}, nil
}
func (s *sessionRouteTestUserStore) TouchUser(context.Context, string, time.Time) error { return nil }
func (s *sessionRouteTestUserStore) EnableAdminTOTP(context.Context, string, string, time.Time) error {
	return nil
}

func newSessionTestRouter(authSvc *auth.Service, userStore auth.UserStore, handler *Handler) http.Handler {
	router := chi.NewRouter()
	router.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Use(auth.RequireCompanyMiddleware())
		r.Mount("/sessions", handler.Routes())
	})
	return router
}

func mustGenerateSessionToken(t *testing.T, authSvc *auth.Service, subject auth.TokenSubject) string {
	t.Helper()
	pair, err := authSvc.GenerateTokenPair(subject)
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}
	return pair.AccessToken
}

func TestHandlerStartReturnsForbiddenWhenManagerAccessRequired(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{startErr: ErrManagerAccessRequired}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions", strings.NewReader(`{"machine_id":"machine-1","access_mode":"watch_only"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}

func TestHandlerStartReturnsBlockedMachineLimitErrorBody(t *testing.T) {
	tests := []struct {
		name          string
		accessMode    string
		serviceErr    error
		expectedMode  string
	}{
		{
			name:         "interactive",
			accessMode:   "interactive",
			serviceErr:   ErrMachineBlockedByLimit,
			expectedMode: AccessModeInteractive,
		},
		{
			name:         "watch only wrapped",
			accessMode:   "watch_only",
			serviceErr:   fmt.Errorf("%w: machine limit lookup failed", ErrMachineBlockedByLimit),
			expectedMode: AccessModeWatchOnly,
		},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
			companyID := "company-1"
			service := &fakeSessionService{startErr: tc.serviceErr}
			handler := NewHandler(service)
			userStore := &sessionRouteTestUserStore{
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
			router := newSessionTestRouter(authSvc, userStore, handler)

			token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
				UserID:    "user-1",
				Email:     "user@example.com",
				Name:      "Operator",
				Role:      auth.RoleCompanyUser,
				CompanyID: companyID,
			})

			req := httptest.NewRequest(http.MethodPost, "/sessions", strings.NewReader(fmt.Sprintf(`{"machine_id":"machine-1","access_mode":%q}`, tc.accessMode)))
			req.Header.Set("Authorization", "Bearer "+token)
			req.Header.Set("Content-Type", "application/json")

			w := httptest.NewRecorder()
			router.ServeHTTP(w, req)

			if w.Code != http.StatusForbidden {
				t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
			}
			if service.lastStartReq.AccessMode != tc.expectedMode {
				t.Fatalf("service access mode = %q, want %q", service.lastStartReq.AccessMode, tc.expectedMode)
			}

			var resp errorResp
			if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
				t.Fatalf("decode response: %v", err)
			}
			if resp.Error != machineBlockedByLimitMessage {
				t.Fatalf("error body = %q, want %q", resp.Error, machineBlockedByLimitMessage)
			}
		})
	}
}

func TestHandlerStartMarksManagerFlagForManagerUser(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "manager@example.com",
		Name:      "Manager",
		Role:      auth.RoleCompanyManager,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions", strings.NewReader(`{"machine_id":"machine-1","access_mode":"interactive"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusCreated {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusCreated, w.Body.String())
	}
	if !service.lastStartManager {
		t.Fatalf("lastStartManager = false, want true")
	}
	if service.lastStartReq.MachineID != "machine-1" {
		t.Fatalf("machine id = %q, want %q", service.lastStartReq.MachineID, "machine-1")
	}
}

func TestHandlerStartNormalizesWatchOnlyAccessMode(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "manager@example.com",
		Name:      "Manager",
		Role:      auth.RoleCompanyManager,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions", strings.NewReader(`{"machine_id":"machine-1","access_mode":"  WATCH_ONLY  "}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusCreated {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusCreated, w.Body.String())
	}
	if service.lastStartReq.AccessMode != AccessModeWatchOnly {
		t.Fatalf("access mode = %q, want %q", service.lastStartReq.AccessMode, AccessModeWatchOnly)
	}
}

func TestHandlerStartRejectsMachineToken(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions", strings.NewReader(`{"machine_id":"machine-1","access_mode":"interactive"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}

func TestHandlerGetAllowsMachineForOwnSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{
		getSession: &Session{ID: "session-1", CompanyID: "company-1", MachineID: "machine-1", Status: SessionStatusPendingConsent},
	}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodGet, "/sessions/session-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
}

func TestHandlerGetIncludesRelayTransportForMachine(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{
		getSession: &Session{ID: "session-1", CompanyID: "company-1", MachineID: "machine-1", Status: SessionStatusPendingConsent},
	}
	handler := NewHandler(service, relay.NewTicketIssuer("relay-secret", "relay.example.com", 9100, 5*time.Minute))
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodGet, "/sessions/session-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	body := w.Body.String()
	if !strings.Contains(body, "\"relay_session_id\":\"session-1\"") {
		t.Fatalf("response missing relay session id: %s", body)
	}
	if !strings.Contains(body, "\"relay_ticket\":") {
		t.Fatalf("response missing relay ticket: %s", body)
	}
}

func TestHandlerResolveConsentRejectsHumanCaller(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions/session-1/consent", strings.NewReader(`{"decision":"approved"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if service.lastConsentSessionID != "" {
		t.Fatalf("service should not have been called, got session=%q", service.lastConsentSessionID)
	}
}

func TestHandlerResolveConsentRejectsHumanCallerEvenWithMachineKey(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions/session-1/consent", strings.NewReader(`{"decision":"approved","machine_key":"key-1"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
	if service.lastConsentSessionID != "" {
		t.Fatalf("service should not have been called, got session=%q", service.lastConsentSessionID)
	}
}

func TestHandlerResolveConsentRejectsInvalidDecision(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{resolveErr: ErrInvalidConsentDecision}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions/session-1/consent", strings.NewReader(`{"decision":"maybe"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusBadRequest, w.Body.String())
	}
}

func TestHandlerResolveConsentRejectsMachineForOtherSessionOwner(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{resolveErr: ErrMachineAccessDenied}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions/session-2/consent", strings.NewReader(`{"decision":"approved"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}

func TestHandlerResolveConsentPassesDecisionToService(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPost, "/sessions/session-1/consent", strings.NewReader(`{"decision":"expired"}`))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusOK, w.Body.String())
	}
	if service.lastConsentSessionID != "session-1" || service.lastConsentMachineID != "machine-1" || service.lastConsentDecision != "expired" {
		t.Fatalf("unexpected consent call: session=%q machine=%q decision=%q", service.lastConsentSessionID, service.lastConsentMachineID, service.lastConsentDecision)
	}
}

func TestHandlerHeartbeatAllowsHumanForOwnSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPut, "/sessions/session-1/heartbeat", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNoContent, w.Body.String())
	}
	if service.lastHeartbeatSessionID != "session-1" || service.lastHeartbeatUserID != "user-1" || service.lastHeartbeatIsMachine {
		t.Fatalf("unexpected heartbeat call: session=%q user=%q is_machine=%v",
			service.lastHeartbeatSessionID, service.lastHeartbeatUserID, service.lastHeartbeatIsMachine)
	}
}

func TestHandlerHeartbeatRejectsHumanForOtherUsersSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	companyID := "company-1"
	service := &fakeSessionService{heartbeatErr: ErrHeartbeatAccessDenied}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{
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
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: companyID,
	})

	req := httptest.NewRequest(http.MethodPut, "/sessions/session-2/heartbeat", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}

func TestHandlerHeartbeatAllowsMachineForOwnSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPut, "/sessions/session-1/heartbeat", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNoContent, w.Body.String())
	}
	if !service.lastHeartbeatIsMachine || service.lastHeartbeatMachineID != "machine-1" {
		t.Fatalf("unexpected machine heartbeat call: is_machine=%v machine_id=%q",
			service.lastHeartbeatIsMachine, service.lastHeartbeatMachineID)
	}
}

func TestHandlerHeartbeatRejectsMachineForOtherMachinesSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	service := &fakeSessionService{heartbeatErr: ErrHeartbeatAccessDenied}
	handler := NewHandler(service)
	userStore := &sessionRouteTestUserStore{}
	router := newSessionTestRouter(authSvc, userStore, handler)

	token := mustGenerateSessionToken(t, authSvc, auth.TokenSubject{
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   "machine-1",
		CompanyID:   "company-1",
	})

	req := httptest.NewRequest(http.MethodPut, "/sessions/session-2/heartbeat", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusForbidden, w.Body.String())
	}
}
