package admin

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	dto "github.com/prometheus/client_model/go"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

type fakeAdminService struct {
	getCompanyDetailFn   func(ctx context.Context, companyID string) (*CompanyDetail, error)
	searchMachinesFn     func(ctx context.Context, query string, limit int) ([]MachineSummary, error)
	detachMachineFn      func(ctx context.Context, machineID string) error
	updateMachineLimitFn func(ctx context.Context, companyID string, limit *int) error
}

func (f *fakeAdminService) ListCompanies(context.Context) ([]CompanySummary, error) {
	return nil, nil
}

func (f *fakeAdminService) GetDashboardStats(context.Context) (DashboardStats, error) {
	return DashboardStats{}, nil
}

func (f *fakeAdminService) GetCompanyDetail(ctx context.Context, companyID string) (*CompanyDetail, error) {
	if f.getCompanyDetailFn != nil {
		return f.getCompanyDetailFn(ctx, companyID)
	}
	return nil, ErrCompanyNotFound
}

func (f *fakeAdminService) SearchMachines(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
	if f.searchMachinesFn != nil {
		return f.searchMachinesFn(ctx, query, limit)
	}
	return []MachineSummary{}, nil
}

func (f *fakeAdminService) DetachMachine(ctx context.Context, machineID string) error {
	if f.detachMachineFn != nil {
		return f.detachMachineFn(ctx, machineID)
	}
	return nil
}

func (f *fakeAdminService) UpdateCompanyBlockDays(context.Context, string, *int) error {
	return nil
}

func (f *fakeAdminService) CreateCompany(context.Context, string) (*CompanySummary, error) {
	return nil, nil
}

func (f *fakeAdminService) CreateCompanyUserWithRole(context.Context, string, string, string, string, auth.Role) (*CompanyUser, error) {
	return nil, nil
}

func (f *fakeAdminService) SetUserActive(context.Context, string, bool) error {
	return nil
}

func (f *fakeAdminService) UpdateUserRole(context.Context, string, auth.Role) error {
	return nil
}

func (f *fakeAdminService) SetCompanyActive(context.Context, string, bool) error {
	return nil
}

func (f *fakeAdminService) UpdateConcurrentSessionLimit(context.Context, string, *int) error {
	return nil
}

func (f *fakeAdminService) UpdateMachineLimit(ctx context.Context, companyID string, limit *int) error {
	if f.updateMachineLimitFn != nil {
		return f.updateMachineLimitFn(ctx, companyID, limit)
	}
	return nil
}

type fakeAdminUserStore struct {
	getUserByEmailFn  func(context.Context, string) (*auth.UserRecord, error)
	getUserByIDFn     func(context.Context, string) (*auth.UserRecord, error)
	enableAdminTOTPFn func(context.Context, string, string, time.Time) error
	getUserByIDCalls  int
}

func (f *fakeAdminUserStore) EmailExists(context.Context, string) (bool, error) {
	return false, nil
}

func (f *fakeAdminUserStore) CreateUser(context.Context, auth.CreateUserInput) (*auth.UserRecord, error) {
	return nil, nil
}

func (f *fakeAdminUserStore) GetUserByEmail(ctx context.Context, email string) (*auth.UserRecord, error) {
	if f.getUserByEmailFn != nil {
		return f.getUserByEmailFn(ctx, email)
	}
	return nil, auth.ErrUserNotFound
}

func (f *fakeAdminUserStore) GetUserByID(ctx context.Context, userID string) (*auth.UserRecord, error) {
	f.getUserByIDCalls++
	if f.getUserByIDFn != nil {
		return f.getUserByIDFn(ctx, userID)
	}
	return &auth.UserRecord{
		ID:            "admin-1",
		Email:         "admin@example.com",
		Name:          "Admin",
		Role:          auth.RoleSuperAdmin,
		IsActive:      true,
		CompanyActive: true,
	}, nil
}

func (f *fakeAdminUserStore) GetMachineByID(context.Context, string) (*auth.MachineRecord, error) {
	return nil, auth.ErrMachineNotFound
}

func (f *fakeAdminUserStore) TouchUser(context.Context, string, time.Time) error {
	return nil
}

func (f *fakeAdminUserStore) EnableAdminTOTP(ctx context.Context, userID, secret string, updatedAt time.Time) error {
	if f.enableAdminTOTPFn != nil {
		return f.enableAdminTOTPFn(ctx, userID, secret, updatedAt)
	}
	return nil
}

func adminAccessCookie(t *testing.T, svc *auth.Service) *http.Cookie {
	t.Helper()

	token, err := svc.GenerateAdminSessionToken(auth.TokenSubject{
		UserID: "admin-1",
		Email:  "admin@example.com",
		Name:   "Admin",
		Role:   auth.RoleSuperAdmin,
	})
	if err != nil {
		t.Fatalf("GenerateAdminSessionToken failed: %v", err)
	}

	return &http.Cookie{Name: adminCookieName, Value: token, Path: "/admin"}
}

func newAdminTestHandler(authSvc *auth.Service, users auth.UserStore, rawAdminHost string, adminHostConfigured bool) *Handler {
	return newAdminTestHandlerWithLimiters(authSvc, users, rawAdminHost, adminHostConfigured, nil, nil)
}

func newAdminTestHandlerWithLimiters(authSvc *auth.Service, users auth.UserStore, rawAdminHost string, adminHostConfigured bool, passwordLimiter, totpLimiter *auth.RateLimiter) *Handler {
	if users == nil {
		users = &fakeAdminUserStore{}
	}
	return &Handler{
		svc:                 &fakeAdminService{},
		authSvc:             authSvc,
		users:               users,
		adminHost:           NormalizeConfiguredAdminHost(rawAdminHost),
		adminHostConfigured: adminHostConfigured,
		passwordLimiter:     passwordLimiter,
		totpLimiter:         totpLimiter,
	}
}

func newAdminObservabilityMetrics(t *testing.T) *observability.Metrics {
	t.Helper()

	metrics, err := observability.NewMetrics("admin-test")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}
	return metrics
}

func adminAuthAbuseCounterValue(t *testing.T, metrics *observability.Metrics, surface, reason string) float64 {
	t.Helper()

	families, err := metrics.Registry.Gather()
	if err != nil {
		t.Fatalf("Gather() error = %v", err)
	}
	for _, family := range families {
		if family.GetName() != "remotex_auth_abuse_events_total" {
			continue
		}
		for _, metric := range family.GetMetric() {
			if adminMetricLabelValue(metric.GetLabel(), "surface") == surface && adminMetricLabelValue(metric.GetLabel(), "reason") == reason {
				return metric.GetCounter().GetValue()
			}
		}
		return 0
	}
	return 0
}

func adminMetricLabelValue(labels []*dto.LabelPair, name string) string {
	for _, label := range labels {
		if label.GetName() == name {
			return label.GetValue()
		}
	}
	return ""
}

func markSecureAdminRequest(req *http.Request) {
	req.Header.Set("X-Forwarded-Proto", "https")
}

func TestCanonicalAdminHostGuard(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))

	t.Run("exact_host_allows_login_page", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, nil, " Admin.Example.Com ", true)
		req := httptest.NewRequest(http.MethodGet, "/login", nil)
		req.Host = "admin.example.com"
		markSecureAdminRequest(req)
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		cookies := w.Result().Cookies()
		if len(cookies) != 1 || cookies[0].Name != csrfCookieName {
			t.Fatalf("cookies = %+v, want one %s cookie", cookies, csrfCookieName)
		}
	})

	t.Run("host_with_port_is_normalized", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, nil, " Admin.Example.Com ", true)
		req := httptest.NewRequest(http.MethodGet, "/login", nil)
		req.Host = "ADMIN.EXAMPLE.COM:8443"
		markSecureAdminRequest(req)
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		cookies := w.Result().Cookies()
		if len(cookies) != 1 || cookies[0].Name != csrfCookieName {
			t.Fatalf("cookies = %+v, want one %s cookie", cookies, csrfCookieName)
		}
	})

	t.Run("wrong_host_returns_disguised_404_before_auth_or_csrf", func(t *testing.T) {
		users := &fakeAdminUserStore{}
		handler := newAdminTestHandler(authSvc, users, "admin.example.com", true)
		req := httptest.NewRequest(http.MethodGet, "/machines", nil)
		req.Host = "public.example.com"
		req.AddCookie(adminAccessCookie(t, authSvc))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusNotFound {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
		}
		if body := w.Body.String(); body != "404 page not found\n" {
			t.Fatalf("body = %q, want default 404 body", body)
		}
		if location := w.Header().Get("Location"); location != "" {
			t.Fatalf("Location = %q, want empty", location)
		}
		if cookies := w.Result().Cookies(); len(cookies) != 0 {
			t.Fatalf("cookies = %+v, want none", cookies)
		}
		if users.getUserByIDCalls != 0 {
			t.Fatalf("GetUserByID calls = %d, want 0", users.getUserByIDCalls)
		}
	})

	t.Run("blank_configured_host_fails_closed", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, nil, "   ", true)
		req := httptest.NewRequest(http.MethodGet, "/login", nil)
		req.Host = "admin.example.com"
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusNotFound {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
		}
		if cookies := w.Result().Cookies(); len(cookies) != 0 {
			t.Fatalf("cookies = %+v, want none", cookies)
		}
	})

	t.Run("path_like_configured_host_fails_closed", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, nil, "admin.example.com/admin", true)
		req := httptest.NewRequest(http.MethodGet, "/login", nil)
		req.Host = "admin.example.com"
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusNotFound {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
		}
		if cookies := w.Result().Cookies(); len(cookies) != 0 {
			t.Fatalf("cookies = %+v, want none", cookies)
		}
	})

	t.Run("unset_config_preserves_existing_flow", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, nil, "", false)
		req := httptest.NewRequest(http.MethodGet, "/login", nil)
		req.Host = "public.example.com"
		markSecureAdminRequest(req)
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		cookies := w.Result().Cookies()
		if len(cookies) != 1 || cookies[0].Name != csrfCookieName {
			t.Fatalf("cookies = %+v, want one %s cookie", cookies, csrfCookieName)
		}
	})
}

func TestCSRFMiddlewareAllowsMultipartUploadForm(t *testing.T) {
	handler := &Handler{}
	token := "csrf-token-123"
	payload := []byte("zip payload")

	body := &bytes.Buffer{}
	writer := multipart.NewWriter(body)
	if err := writer.WriteField("csrf_token", token); err != nil {
		t.Fatalf("WriteField() error = %v", err)
	}
	part, err := writer.CreateFormFile("package", "release.zip")
	if err != nil {
		t.Fatalf("CreateFormFile() error = %v", err)
	}
	if _, err := part.Write(payload); err != nil {
		t.Fatalf("Write() error = %v", err)
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	req := httptest.NewRequest(http.MethodPost, "/admin/releases/upload", body)
	req.Header.Set("Content-Type", writer.FormDataContentType())
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: token, Path: "/admin"})
	w := httptest.NewRecorder()

	next := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := csrfFromCtx(r); got != token {
			t.Fatalf("csrfFromCtx() = %q, want %q", got, token)
		}
		file, header, err := r.FormFile("package")
		if err != nil {
			t.Fatalf("FormFile() error = %v", err)
		}
		defer file.Close()

		if header.Filename != "release.zip" {
			t.Fatalf("Filename = %q, want %q", header.Filename, "release.zip")
		}
		got, err := io.ReadAll(file)
		if err != nil {
			t.Fatalf("ReadAll() error = %v", err)
		}
		if !bytes.Equal(got, payload) {
			t.Fatalf("payload = %q, want %q", string(got), string(payload))
		}
		w.WriteHeader(http.StatusNoContent)
	})

	handler.csrfProtect(next).ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNoContent)
	}
}

func TestDashboardRendersHealthSnapshot(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	checkedAt := time.Date(2026, 4, 3, 15, 4, 5, 0, time.UTC)
	componentCheckedAt := checkedAt.Add(-30 * time.Second)
	lastRunAt := checkedAt.Add(-15 * time.Second)
	lastSuccessAt := checkedAt.Add(-20 * time.Second)
	lastCount := int64(7)

	handler := &Handler{
		svc:     &fakeAdminService{},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
		healthSnapshot: func(context.Context) observability.HealthSnapshot {
			return observability.HealthSnapshot{
				Service:   "api",
				Status:    observability.StatusDegraded,
				CheckedAt: checkedAt,
				Components: map[string]observability.ComponentStatus{
					"database": {
						Status:    observability.StatusHealthy,
						Mode:      "primary",
						Detail:    "database ping ok",
						CheckedAt: &componentCheckedAt,
					},
					"redis": {
						Status:    observability.StatusDegraded,
						Mode:      "fallback_only",
						Detail:    "redis ping failed; using in-memory fallback",
						LastError: "redis down",
						CheckedAt: &componentCheckedAt,
					},
				},
				Monitors: map[string]observability.MonitorStatus{
					"heartbeat": {
						Status:          observability.StatusHealthy,
						IntervalSeconds: 30,
						LastRunAt:       &lastRunAt,
						LastSuccessAt:   &lastSuccessAt,
						LastCount:       &lastCount,
					},
				},
			}
		},
	}

	req := httptest.NewRequest(http.MethodGet, "/", nil)
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	body := w.Body.String()
	for _, needle := range []string{
		"Saude do backend",
		"Snapshot interno de",
		">/health<",
		"fallback_only",
		"redis ping failed; using in-memory fallback",
		"redis down",
		"heartbeat",
		"Count: 7",
		">degraded<",
	} {
		if !strings.Contains(body, needle) {
			t.Fatalf("response body missing %q: %s", needle, body)
		}
	}
}

func TestMachinesPageRendersSearchResults(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := &Handler{
		svc: &fakeAdminService{
			searchMachinesFn: func(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
				if query != "host-cliente" {
					t.Fatalf("query = %q, want %q", query, "host-cliente")
				}
				if limit != 100 {
					t.Fatalf("limit = %d, want 100", limit)
				}
				now := time.Date(2026, 3, 17, 12, 0, 0, 0, time.UTC)
				return []MachineSummary{{
					ID:          "machine-1",
					Name:        "Host Cliente 01",
					MachineKey:  "key-123",
					CompanyID:   "company-1",
					CompanyName: "Empresa XPTO",
					IsOnline:    true,
					UpdatedAt:   now,
				}}, nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	req := httptest.NewRequest(http.MethodGet, "/machines?q=host-cliente", nil)
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	body := w.Body.String()
	if !strings.Contains(body, "Host Cliente 01") {
		t.Fatalf("response body missing machine name: %s", body)
	}
	if !strings.Contains(body, "Empresa XPTO") {
		t.Fatalf("response body missing company name: %s", body)
	}
}

func TestMachinesPageRendersBlockedMachineStatusAndReason(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := &Handler{
		svc: &fakeAdminService{
			searchMachinesFn: func(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
				if query != "bloqueada" {
					t.Fatalf("query = %q, want %q", query, "bloqueada")
				}
				if limit != 100 {
					t.Fatalf("limit = %d, want 100", limit)
				}
				now := time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC)
				heartbeat := now.Add(-1 * time.Minute)
				return []MachineSummary{{
					ID:            "machine-blocked",
					Name:          "Host Bloqueado",
					MachineKey:    "key-blocked",
					CompanyID:     "company-1",
					CompanyName:   "Empresa XPTO",
					IsOnline:      true,
					LastHeartbeat: &heartbeat,
					UpdatedAt:     now,
					State:         machinelimit.StateBlocked,
					BlockReason:   machinelimit.BlockReasonMachineLimitExceeded,
				}}, nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	req := httptest.NewRequest(http.MethodGet, "/machines?q=bloqueada", nil)
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	body := w.Body.String()
	for _, needle := range []string{"Host Bloqueado", "Bloqueada", "Limite de maquinas excedido", "Online"} {
		if !strings.Contains(body, needle) {
			t.Fatalf("response body missing %q: %s", needle, body)
		}
	}
	if strings.Contains(body, "machine_limit_exceeded") {
		t.Fatalf("response body leaked canonical reason code: %s", body)
	}
}

func TestMachinesPageJSONIncludesMachineLimitStateParity(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	now := time.Date(2026, 4, 7, 12, 0, 0, 0, time.UTC)
	handler := &Handler{
		svc: &fakeAdminService{
			searchMachinesFn: func(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
				if query != "host" {
					t.Fatalf("query = %q, want %q", query, "host")
				}
				if limit != 100 {
					t.Fatalf("limit = %d, want 100", limit)
				}
				return []MachineSummary{
					{
						ID:          "machine-blocked",
						Name:        "Host Bloqueado",
						MachineKey:  "key-blocked",
						CompanyID:   "company-1",
						CompanyName: "Empresa XPTO",
						IsOnline:    true,
						UpdatedAt:   now,
						State:       machinelimit.StateBlocked,
						BlockReason: machinelimit.BlockReasonMachineLimitExceeded,
					},
					{
						ID:          "machine-detached",
						Name:        "Host Solto",
						MachineKey:  "key-detached",
						CompanyID:   "",
						CompanyName: "Sem empresa",
						IsOnline:    false,
						UpdatedAt:   now.Add(-1 * time.Minute),
					},
				}, nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	req := httptest.NewRequest(http.MethodGet, "/machines?q=host", nil)
	req.Header.Set("Accept", "application/json")
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	var resp []MachineSummary
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if len(resp) != 2 {
		t.Fatalf("len(resp) = %d, want 2", len(resp))
	}
	if resp[0].State != machinelimit.StateBlocked || resp[0].BlockReason != machinelimit.BlockReasonMachineLimitExceeded {
		t.Fatalf("blocked machine state = %q/%q, want blocked/machine_limit_exceeded", resp[0].State, resp[0].BlockReason)
	}
	if resp[1].CompanyID != "" || resp[1].CompanyName != "Sem empresa" {
		t.Fatalf("detached machine company fields = %+v, want detached labels", resp[1])
	}
	if resp[1].State != "" || resp[1].BlockReason != "" {
		t.Fatalf("detached machine state = %q/%q, want empty additive fields", resp[1].State, resp[1].BlockReason)
	}
}

func TestMachinesPageRendersDetachedMachine(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := &Handler{
		svc: &fakeAdminService{
			searchMachinesFn: func(ctx context.Context, query string, limit int) ([]MachineSummary, error) {
				now := time.Date(2026, 3, 17, 12, 0, 0, 0, time.UTC)
				return []MachineSummary{{
					ID:          "machine-detached",
					Name:        "Host Orfao",
					MachineKey:  "key-detached",
					CompanyID:   "",
					CompanyName: "Sem empresa",
					IsOnline:    false,
					UpdatedAt:   now,
				}}, nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	req := httptest.NewRequest(http.MethodGet, "/machines?q=orfao", nil)
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	body := w.Body.String()
	if !strings.Contains(body, "Sem empresa") {
		t.Fatalf("response body missing detached company label: %s", body)
	}
	if !strings.Contains(body, "Ja desvinculada") {
		t.Fatalf("response body missing detached status label: %s", body)
	}
	if strings.Contains(body, `action="/admin/machines/machine-detached/detach"`) {
		t.Fatalf("response body unexpectedly renders detach form for detached machine: %s", body)
	}
}

func TestDetachMachineRedirectsBackToSearch(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	called := false
	handler := &Handler{
		svc: &fakeAdminService{
			detachMachineFn: func(ctx context.Context, machineID string) error {
				called = true
				if machineID != "machine-1" {
					t.Fatalf("machineID = %q, want %q", machineID, "machine-1")
				}
				return nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	form := url.Values{}
	form.Set("csrf_token", "csrf-123")
	form.Set("q", "host-cliente")
	req := httptest.NewRequest(http.MethodPost, "/machines/machine-1/detach", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: "csrf-123", Path: "/admin"})
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if !called {
		t.Fatal("expected DetachMachine to be called")
	}
	if w.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
	}
	location := w.Header().Get("Location")
	if !strings.Contains(location, "/admin/machines?q=host-cliente&message=") {
		t.Fatalf("unexpected redirect location: %s", location)
	}
}

func TestAdminPasswordRateLimitExportsDistinctMetric(t *testing.T) {
	metrics := newAdminObservabilityMetrics(t)
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	hash, err := authSvc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}

	users := &fakeAdminUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*auth.UserRecord, error) {
			return &auth.UserRecord{
				ID:            "admin-password",
				Email:         email,
				Name:          "Admin",
				Role:          auth.RoleSuperAdmin,
				PasswordHash:  hash,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}
	passwordLimiter, totpLimiter := newAdminRateLimiters(2, time.Minute)
	handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)

	for attempt := 1; attempt <= 3; attempt++ {
		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"wrong-pass"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-password")
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		wantStatus := http.StatusUnauthorized
		if attempt == 3 {
			wantStatus = http.StatusTooManyRequests
		}
		if w.Code != wantStatus {
			t.Fatalf("attempt %d status = %d, want %d", attempt, w.Code, wantStatus)
		}
	}

	if got := adminAuthAbuseCounterValue(t, metrics, observability.AuthAbuseSurfaceAdminPassword, observability.AuthAbuseReasonLimited); got != 1 {
		t.Fatalf("admin password limited counter = %v, want 1", got)
	}
	if got := adminAuthAbuseCounterValue(t, metrics, observability.AuthAbuseSurfaceAdminTOTP, observability.AuthAbuseReasonLimited); got != 0 {
		t.Fatalf("admin totp limited counter = %v, want 0", got)
	}
}

func TestAdminTOTPRateLimitExportsDistinctMetric(t *testing.T) {
	metrics := newAdminObservabilityMetrics(t)
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	hash, err := authSvc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}
	enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
	if err != nil {
		t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
	}
	secret := enrollment.Secret

	users := &fakeAdminUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*auth.UserRecord, error) {
			return &auth.UserRecord{
				ID:               "admin-mfa",
				Email:            email,
				Name:             "Admin",
				Role:             auth.RoleSuperAdmin,
				PasswordHash:     hash,
				IsActive:         true,
				CompanyActive:    true,
				AdminTOTPEnabled: true,
				AdminTOTPSecret:  &secret,
			}, nil
		},
		getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
			return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true, AdminTOTPEnabled: true, AdminTOTPSecret: &secret}, nil
		},
	}
	passwordLimiter, totpLimiter := newAdminRateLimiters(2, time.Minute)
	handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)

	loginReq := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"secret123"}`))
	markSecureAdminRequest(loginReq)
	addJSONCSRF(loginReq, "csrf-login")
	loginResp := httptest.NewRecorder()
	handler.Routes().ServeHTTP(loginResp, loginReq)
	if loginResp.Code != http.StatusOK {
		t.Fatalf("password phase status = %d, want %d", loginResp.Code, http.StatusOK)
	}
	challengeCookie := cookieNamed(loginResp.Result().Cookies(), adminChallengeCookieName)
	if challengeCookie == nil || challengeCookie.Value == "" {
		t.Fatalf("missing %s cookie", adminChallengeCookieName)
	}

	for attempt := 1; attempt <= 3; attempt++ {
		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-verify")
		req.AddCookie(challengeCookie)
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		wantStatus := http.StatusUnauthorized
		if attempt == 3 {
			wantStatus = http.StatusTooManyRequests
		}
		if w.Code != wantStatus {
			t.Fatalf("attempt %d status = %d, want %d", attempt, w.Code, wantStatus)
		}
	}

	if got := adminAuthAbuseCounterValue(t, metrics, observability.AuthAbuseSurfaceAdminTOTP, observability.AuthAbuseReasonLimited); got != 1 {
		t.Fatalf("admin totp limited counter = %v, want 1", got)
	}
	if got := adminAuthAbuseCounterValue(t, metrics, observability.AuthAbuseSurfaceAdminPassword, observability.AuthAbuseReasonLimited); got != 0 {
		t.Fatalf("admin password limited counter = %v, want 0", got)
	}
}
