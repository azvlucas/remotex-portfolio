package admin

import (
	"context"
	"crypto/tls"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

func cookieNamed(cookies []*http.Cookie, name string) *http.Cookie {
	for _, cookie := range cookies {
		if cookie.Name == name {
			return cookie
		}
	}
	return nil
}

func TestAdminSecureTransportRejectsInsecureRequestsBeforeCSRForAuth(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	users := &fakeAdminUserStore{}
	handler := newAdminTestHandler(authSvc, users, "", false)

	req := httptest.NewRequest(http.MethodGet, "/machines", nil)
	req.Host = "admin.example.com"
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
	}
	if body := w.Body.String(); body != "404 page not found\n" {
		t.Fatalf("body = %q, want default 404 body", body)
	}
	if cookies := w.Result().Cookies(); len(cookies) != 0 {
		t.Fatalf("cookies = %+v, want none", cookies)
	}
	if users.getUserByIDCalls != 0 {
		t.Fatalf("GetUserByID calls = %d, want 0", users.getUserByIDCalls)
	}
}

func TestAdminSecureTransportMarksCSRFCookieSecureBehindForwardedHTTPS(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := newAdminTestHandler(authSvc, nil, "", false)

	req := httptest.NewRequest(http.MethodGet, "/login", nil)
	req.Host = "admin.example.com"
	req.Header.Set("X-Forwarded-Proto", " HTTPS ")
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	csrfCookie := cookieNamed(w.Result().Cookies(), csrfCookieName)
	if csrfCookie == nil {
		t.Fatalf("missing %s cookie", csrfCookieName)
	}
	if !csrfCookie.Secure {
		t.Fatalf("%s Secure = false, want true", csrfCookieName)
	}
}

func TestAdminSecureTransportMarksAdminChallengeCookieSecureBehindForwardedHTTPS(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	hash, err := authSvc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("HashPassword() error = %v", err)
	}

	users := &fakeAdminUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*auth.UserRecord, error) {
			return &auth.UserRecord{
				ID:            "admin-1",
				Email:         email,
				Name:          "Admin",
				Role:          auth.RoleSuperAdmin,
				PasswordHash:  hash,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}
	handler := newAdminTestHandler(authSvc, users, "", false)

	form := url.Values{}
	form.Set("email", "admin@example.com")
	form.Set("password", "secret123")
	form.Set("csrf_token", "csrf-123")
	req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
	req.Host = "admin.example.com"
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("X-Forwarded-Proto", "https")
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: "csrf-123", Path: "/admin"})
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
	}
	challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
	if challengeCookie == nil {
		t.Fatalf("missing %s cookie", adminChallengeCookieName)
	}
	if !challengeCookie.Secure {
		t.Fatalf("%s Secure = false, want true", adminChallengeCookieName)
	}
	if _, err := authSvc.ValidateAdminChallengeToken(challengeCookie.Value); err != nil {
		t.Fatalf("ValidateAdminChallengeToken() error = %v", err)
	}
	if adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName); adminCookie == nil || adminCookie.MaxAge != -1 {
		t.Fatalf("expected cleared %s cookie during challenge phase", adminCookieName)
	}
}

func TestAdminSecureTransportAllowsDirectTLS(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := newAdminTestHandler(authSvc, nil, "", false)

	req := httptest.NewRequest(http.MethodGet, "/login", nil)
	req.Host = "admin.example.com"
	req.TLS = &tls.ConnectionState{}
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	csrfCookie := cookieNamed(w.Result().Cookies(), csrfCookieName)
	if csrfCookie == nil {
		t.Fatalf("missing %s cookie", csrfCookieName)
	}
	if !csrfCookie.Secure {
		t.Fatalf("%s Secure = false, want true", csrfCookieName)
	}
}

func TestAdminSecureTransportMarksLogoutCookieSecureBehindForwardedHTTPS(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := newAdminTestHandler(authSvc, &fakeAdminUserStore{}, "", false)

	form := url.Values{}
	form.Set("csrf_token", "csrf-123")
	req := httptest.NewRequest(http.MethodPost, "/logout", strings.NewReader(form.Encode()))
	req.Host = "admin.example.com"
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("X-Forwarded-Proto", "https")
	req.AddCookie(adminAccessCookie(t, authSvc))
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: "csrf-123", Path: "/admin"})
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
	}
	logoutCookie := cookieNamed(w.Result().Cookies(), adminCookieName)
	if logoutCookie == nil {
		t.Fatalf("missing %s cookie", adminCookieName)
	}
	if !logoutCookie.Secure {
		t.Fatalf("%s Secure = false, want true", adminCookieName)
	}
}

func TestCurrentAdminRequiresAdminSession(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))

	t.Run("rejects generic public token in admin cookie", func(t *testing.T) {
		users := &fakeAdminUserStore{}
		handler := newAdminTestHandler(authSvc, users, "", false)
		pair, err := authSvc.GenerateTokenPair(auth.TokenSubject{
			UserID: "admin-1",
			Email:  "admin@example.com",
			Name:   "Admin",
			Role:   auth.RoleSuperAdmin,
		})
		if err != nil {
			t.Fatalf("GenerateTokenPair() error = %v", err)
		}

		req := httptest.NewRequest(http.MethodGet, "/machines", nil)
		markSecureAdminRequest(req)
		req.AddCookie(&http.Cookie{Name: adminCookieName, Value: pair.AccessToken, Path: "/admin"})
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusSeeOther {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
		}
		if location := w.Header().Get("Location"); !strings.HasPrefix(location, "/admin/login?error=") {
			t.Fatalf("Location = %q, want /admin/login redirect", location)
		}
		if users.getUserByIDCalls != 0 {
			t.Fatalf("GetUserByID calls = %d, want 0", users.getUserByIDCalls)
		}
	})

	t.Run("accepts dedicated admin session and reloads user", func(t *testing.T) {
		users := &fakeAdminUserStore{}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodGet, "/machines", nil)
		markSecureAdminRequest(req)
		req.AddCookie(adminAccessCookie(t, authSvc))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		if users.getUserByIDCalls != 1 {
			t.Fatalf("GetUserByID calls = %d, want 1", users.getUserByIDCalls)
		}
	})

	t.Run("rejects inactive admin after role reload", func(t *testing.T) {
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{
					ID:            userID,
					Email:         "admin@example.com",
					Name:          "Admin",
					Role:          auth.RoleSuperAdmin,
					IsActive:      false,
					CompanyActive: true,
				}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodGet, "/machines", nil)
		markSecureAdminRequest(req)
		req.AddCookie(adminAccessCookie(t, authSvc))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusSeeOther {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
		}
		if location := w.Header().Get("Location"); !strings.HasPrefix(location, "/admin/login?error=") {
			t.Fatalf("Location = %q, want /admin/login redirect", location)
		}
		if users.getUserByIDCalls != 1 {
			t.Fatalf("GetUserByID calls = %d, want 1", users.getUserByIDCalls)
		}
	})
}
