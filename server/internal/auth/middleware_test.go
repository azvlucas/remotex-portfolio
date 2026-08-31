package auth

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/go-chi/chi/v5"
)

func testAuthService(t *testing.T) *Service {
	t.Helper()
	return NewService(DefaultConfig("test-secret"))
}

func testAccessToken(t *testing.T, svc *Service, subject TokenSubject) string {
	t.Helper()
	tokenPair, err := svc.GenerateTokenPair(subject)
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}
	return tokenPair.AccessToken
}

func TestExtractTokenAuthorizationHeader(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/protected?client_id=viewer-1", nil)
	req.Header.Set("Authorization", "Bearer header-token")

	if got := extractToken(req); got != "header-token" {
		t.Fatalf("extractToken() = %q, want %q", got, "header-token")
	}
}

func TestExtractTokenFallsBackToAdminCookie(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/admin/protected", nil)
	req.AddCookie(&http.Cookie{Name: adminCookieName, Value: "cookie-token", Path: "/admin"})

	if got := extractToken(req); got != "cookie-token" {
		t.Fatalf("extractToken() = %q, want %q", got, "cookie-token")
	}
}

func TestExtractTokenRejectsMalformedBearerHeader(t *testing.T) {
	tests := []struct {
		name   string
		header string
	}{
		{name: "missing token", header: "Bearer"},
		{name: "too many fields", header: "Bearer token extra"},
		{name: "wrong scheme", header: "Basic token"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/protected", nil)
			req.Header.Set("Authorization", tc.header)

			if got := extractToken(req); got != "" {
				t.Fatalf("extractToken() = %q, want empty", got)
			}
		})
	}
}

func TestExtractTokenRejectsQueryToken(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/ws?client_id=viewer-1&token=query-token", nil)

	if got := extractToken(req); got != "" {
		t.Fatalf("extractToken() = %q, want empty", got)
	}
}

func TestJWTAuthMiddlewareAllowsValidBearerHeader(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Role:      RoleCompanyUser,
		CompanyID: "company-1",
	})

	companyID := "company-1"
	store := &fakeUserStore{
		getUserByIDFn: func(ctx context.Context, userID string) (*UserRecord, error) {
			return &UserRecord{
				ID:            userID,
				Email:         "user@example.com",
				Name:          "Active User",
				CompanyID:     &companyID,
				Role:          RoleCompanyUser,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		claims, ok := UserFromContext(r.Context())
		if !ok || claims == nil || claims.UserID != "user-1" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected?client_id=viewer-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNoContent)
	}
}

func TestJWTAuthMiddlewareAllowsAdminCookie(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID: "admin-1",
		Email:  "admin@example.com",
		Role:   RoleSuperAdmin,
	})

	store := &fakeUserStore{
		getUserByIDFn: func(ctx context.Context, userID string) (*UserRecord, error) {
			return &UserRecord{
				ID:            userID,
				Email:         "admin@example.com",
				Name:          "Admin",
				Role:          RoleSuperAdmin,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.AddCookie(&http.Cookie{Name: adminCookieName, Value: token, Path: "/admin"})
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNoContent)
	}
}

func TestJWTAuthMiddlewareRejectsQueryToken(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Role:      RoleCompanyUser,
		CompanyID: "company-1",
	})

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, nil))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected?client_id=viewer-1&token="+token, nil)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
	if got := w.Body.String(); got != "{\"error\":\"query-string bearer token is no longer supported; use Authorization header\"}" {
		t.Fatalf("body = %q", got)
	}
}

func TestJWTAuthMiddlewareRejectsMalformedAuthorizationHeader(t *testing.T) {
	svc := testAuthService(t)

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, nil))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.Header.Set("Authorization", "Bearer")
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
	if got := w.Body.String(); got != "{\"error\":\"malformed Authorization header\"}" {
		t.Fatalf("body = %q", got)
	}
}

func TestJWTAuthMiddlewareRejectsInactiveUser(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Role:      RoleCompanyUser,
		CompanyID: "company-1",
	})

	store := &fakeUserStore{
		getUserByIDFn: func(ctx context.Context, userID string) (*UserRecord, error) {
			companyID := "company-1"
			return &UserRecord{
				ID:            userID,
				Email:         "user@example.com",
				Name:          "Inactive User",
				CompanyID:     &companyID,
				Role:          RoleCompanyUser,
				IsActive:      false,
				CompanyActive: true,
			}, nil
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusForbidden)
	}
}

func TestRequireCompanyMiddlewareRejectsAdminWithoutCompany(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID: "admin-1",
		Email:  "admin@example.com",
		Role:   RoleSuperAdmin,
	})

	store := &fakeUserStore{
		getUserByIDFn: func(ctx context.Context, userID string) (*UserRecord, error) {
			return &UserRecord{
				ID:            userID,
				Email:         "admin@example.com",
				Name:          "Admin",
				Role:          RoleSuperAdmin,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Use(RequireCompanyMiddleware())
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusForbidden)
	}
}

func TestJWTAuthMiddlewareRejectsDeletedMachine(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	store := &fakeUserStore{
		getMachineByIDFn: func(ctx context.Context, machineID string) (*MachineRecord, error) {
			return nil, ErrMachineNotFound
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
}

func TestJWTAuthMiddlewareRejectsDetachedMachine(t *testing.T) {
	svc := testAuthService(t)
	token := testAccessToken(t, svc, TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: SubjectTypeMachine,
		MachineID:   "machine-1",
	})

	store := &fakeUserStore{
		getMachineByIDFn: func(ctx context.Context, machineID string) (*MachineRecord, error) {
			return &MachineRecord{ID: machineID, CompanyID: "", Name: "Detached Host"}, nil
		},
	}

	r := chi.NewRouter()
	r.Use(JWTAuthMiddleware(svc, store))
	r.Get("/protected", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	req := httptest.NewRequest(http.MethodGet, "/protected", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	w := httptest.NewRecorder()

	r.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
}
