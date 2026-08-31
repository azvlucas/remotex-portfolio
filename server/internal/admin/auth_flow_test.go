package admin

import (
	"context"
	"encoding/json"
	"html"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/pquerna/otp"
	"github.com/pquerna/otp/totp"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

func addJSONCSRF(req *http.Request, token string) {
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-CSRF-Token", token)
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: token, Path: "/admin"})
}

func decodeAdminLoginResponse(t *testing.T, w *httptest.ResponseRecorder) adminLoginResponse {
	t.Helper()
	var resp adminLoginResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	return resp
}

func mustGenerateAdminChallengeCookie(t *testing.T, svc *auth.Service, scope, userID, email, secret, otpAuthURL string, ttl time.Duration) *http.Cookie {
	t.Helper()
	token, err := svc.GenerateAdminChallengeToken(auth.TokenSubject{
		UserID: userID,
		Email:  email,
		Name:   "Admin",
		Role:   auth.RoleSuperAdmin,
	}, scope, secret, otpAuthURL, ttl)
	if err != nil {
		t.Fatalf("GenerateAdminChallengeToken failed: %v", err)
	}
	return &http.Cookie{Name: adminChallengeCookieName, Value: token, Path: "/admin"}
}

func mustGenerateTOTPCode(t *testing.T, secret string) string {
	t.Helper()
	code, err := totp.GenerateCodeCustom(secret, time.Now().UTC(), totp.ValidateOpts{
		Period:    30,
		Skew:      1,
		Digits:    otp.DigitsSix,
		Algorithm: otp.AlgorithmSHA1,
	})
	if err != nil {
		t.Fatalf("GenerateCodeCustom failed: %v", err)
	}
	return code
}

func newAdminRateLimiters(maxAttempts int, window time.Duration) (*auth.RateLimiter, *auth.RateLimiter) {
	store := auth.NewInMemoryRateLimitStore(auth.InMemoryRateLimitStoreConfig{MaxEntries: 16})
	cfg := auth.RateLimiterConfig{MaxAttempts: maxAttempts, Window: window}
	return auth.NewBucketRateLimiter(store, cfg, auth.RateLimitBucketAdminPassword), auth.NewBucketRateLimiter(store, cfg, auth.RateLimitBucketAdminTOTP)
}

func TestAdminTotpSetup(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	hash, err := authSvc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}

	t.Run("malformed login json is rejected", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, &fakeAdminUserStore{}, "", false)
		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader("{"))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusBadRequest)
		}
	})

	t.Run("unenrolled super admin gets setup_required over json", func(t *testing.T) {
		users := &fakeAdminUserStore{
			getUserByEmailFn: func(ctx context.Context, email string) (*auth.UserRecord, error) {
				return &auth.UserRecord{
					ID:            "admin-setup",
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

		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"secret123"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		resp := decodeAdminLoginResponse(t, w)
		if resp.Status != string(adminTOTPStateSetupRequired) {
			t.Fatalf("status = %q, want %q", resp.Status, adminTOTPStateSetupRequired)
		}
		if resp.TOTPSecret == "" || resp.OTPAuthURL == "" {
			t.Fatalf("expected setup payload, got %+v", resp)
		}
		challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
		if challengeCookie == nil {
			t.Fatalf("missing %s cookie", adminChallengeCookieName)
		}
		claims, err := authSvc.ValidateAdminChallengeToken(challengeCookie.Value)
		if err != nil {
			t.Fatalf("ValidateAdminChallengeToken failed: %v", err)
		}
		if claims.Scope != auth.ScopeAdminChallengeSetup {
			t.Fatalf("claims.Scope = %q, want %q", claims.Scope, auth.ScopeAdminChallengeSetup)
		}
		if claims.PendingTOTPSecret != resp.TOTPSecret {
			t.Fatalf("claims.PendingTOTPSecret = %q, want response secret", claims.PendingTOTPSecret)
		}
		if claims.PendingTOTPURL != resp.OTPAuthURL {
			t.Fatalf("claims.PendingTOTPURL = %q, want response URL", claims.PendingTOTPURL)
		}
		if adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName); adminCookie == nil || adminCookie.MaxAge != -1 {
			t.Fatalf("expected cleared %s cookie during setup phase", adminCookieName)
		}
	})

	t.Run("setup page renders secret and otpauth uri", func(t *testing.T) {
		enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
		if err != nil {
			t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
		}
		handler := newAdminTestHandler(authSvc, &fakeAdminUserStore{}, "", false)

		req := httptest.NewRequest(http.MethodGet, "/setup", nil)
		markSecureAdminRequest(req)
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeSetup, "admin-setup", "admin@example.com", enrollment.Secret, enrollment.OTPAuthURL, time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		body := w.Body.String()
		if !strings.Contains(body, enrollment.Secret) {
			t.Fatalf("setup page missing secret: %s", body)
		}
		if !strings.Contains(body, enrollment.OTPAuthURL) && !strings.Contains(body, html.EscapeString(enrollment.OTPAuthURL)) {
			t.Fatalf("setup page missing otpauth URL: %s", body)
		}
	})

	t.Run("setup rejects malformed json body", func(t *testing.T) {
		enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
		if err != nil {
			t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
		}
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodPost, "/setup", strings.NewReader("{"))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeSetup, "admin-setup", "admin@example.com", enrollment.Secret, enrollment.OTPAuthURL, time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusBadRequest)
		}
	})

	t.Run("setup rejects invalid code", func(t *testing.T) {
		enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
		if err != nil {
			t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
		}
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodPost, "/setup", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeSetup, "admin-setup", "admin@example.com", enrollment.Secret, enrollment.OTPAuthURL, time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
		}
		if adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName); adminCookie != nil && adminCookie.Value != "" {
			t.Fatalf("unexpected admin session cookie on invalid setup code: %+v", adminCookie)
		}
	})

	t.Run("valid setup code persists totp and mints final session", func(t *testing.T) {
		enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
		if err != nil {
			t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
		}
		persistedSecret := ""
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true}, nil
			},
			enableAdminTOTPFn: func(ctx context.Context, userID, secret string, updatedAt time.Time) error {
				persistedSecret = secret
				return nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)
		code := mustGenerateTOTPCode(t, enrollment.Secret)

		req := httptest.NewRequest(http.MethodPost, "/setup", strings.NewReader(`{"code":"`+code+`"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeSetup, "admin-setup", "admin@example.com", enrollment.Secret, enrollment.OTPAuthURL, time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		if persistedSecret != enrollment.Secret {
			t.Fatalf("persistedSecret = %q, want %q", persistedSecret, enrollment.Secret)
		}
		adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName)
		if adminCookie == nil || adminCookie.Value == "" {
			t.Fatalf("missing final %s cookie", adminCookieName)
		}
		if _, err := authSvc.ValidateAdminSessionToken(adminCookie.Value); err != nil {
			t.Fatalf("ValidateAdminSessionToken failed: %v", err)
		}
		challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
		if challengeCookie == nil || challengeCookie.MaxAge != -1 {
			t.Fatalf("expected cleared %s cookie after setup completion", adminChallengeCookieName)
		}
	})
}

func TestAdminTotpChallenge(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	hash, err := authSvc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}
	enrollment, err := authSvc.GenerateAdminTOTPEnrollment("admin@example.com")
	if err != nil {
		t.Fatalf("GenerateAdminTOTPEnrollment failed: %v", err)
	}

	t.Run("enrolled super admin gets mfa_required over json", func(t *testing.T) {
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
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"secret123"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		resp := decodeAdminLoginResponse(t, w)
		if resp.Status != string(adminTOTPStateMFARequired) {
			t.Fatalf("status = %q, want %q", resp.Status, adminTOTPStateMFARequired)
		}
		if resp.TOTPSecret != "" || resp.OTPAuthURL != "" {
			t.Fatalf("unexpected setup payload for MFA challenge: %+v", resp)
		}
		challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
		if challengeCookie == nil {
			t.Fatalf("missing %s cookie", adminChallengeCookieName)
		}
		claims, err := authSvc.ValidateAdminChallengeToken(challengeCookie.Value)
		if err != nil {
			t.Fatalf("ValidateAdminChallengeToken failed: %v", err)
		}
		if claims.Scope != auth.ScopeAdminChallengeMFA {
			t.Fatalf("claims.Scope = %q, want %q", claims.Scope, auth.ScopeAdminChallengeMFA)
		}
		if adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName); adminCookie == nil || adminCookie.MaxAge != -1 {
			t.Fatalf("expected cleared %s cookie during MFA phase", adminCookieName)
		}
	})

	t.Run("verify page renders pending challenge", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, &fakeAdminUserStore{}, "", false)
		req := httptest.NewRequest(http.MethodGet, "/verify", nil)
		markSecureAdminRequest(req)
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		if !strings.Contains(w.Body.String(), "admin@example.com") {
			t.Fatalf("verify page missing account hint: %s", w.Body.String())
		}
	})

	t.Run("verify rejects missing challenge cookie", func(t *testing.T) {
		handler := newAdminTestHandler(authSvc, &fakeAdminUserStore{}, "", false)
		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
		}
	})

	t.Run("verify rejects expired challenge token", func(t *testing.T) {
		secret := enrollment.Secret
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true, AdminTOTPEnabled: true, AdminTOTPSecret: &secret}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)
		challengeCookie := mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", 20*time.Millisecond)
		time.Sleep(50 * time.Millisecond)

		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"`+mustGenerateTOTPCode(t, secret)+`"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(challengeCookie)
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
		}
	})

	t.Run("verify rejects malformed and invalid codes", func(t *testing.T) {
		secret := enrollment.Secret
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true, AdminTOTPEnabled: true, AdminTOTPSecret: &secret}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"abc"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", time.Minute))
		w := httptest.NewRecorder()
		handler.Routes().ServeHTTP(w, req)
		if w.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusBadRequest)
		}

		req = httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-456")
		req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: "csrf-456", Path: "/admin"})
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", time.Minute))
		w = httptest.NewRecorder()
		handler.Routes().ServeHTTP(w, req)
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
		}
	})

	t.Run("verify rechecks inactive user before minting final session", func(t *testing.T) {
		secret := enrollment.Secret
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: false, CompanyActive: true, AdminTOTPEnabled: true, AdminTOTPSecret: &secret}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)

		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"`+mustGenerateTOTPCode(t, secret)+`"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusForbidden {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusForbidden)
		}
		if adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName); adminCookie != nil && adminCookie.Value != "" {
			t.Fatalf("unexpected admin session cookie on inactive-user recheck: %+v", adminCookie)
		}
	})

	t.Run("valid verify code is the only path that mints final session", func(t *testing.T) {
		secret := enrollment.Secret
		users := &fakeAdminUserStore{
			getUserByIDFn: func(ctx context.Context, userID string) (*auth.UserRecord, error) {
				return &auth.UserRecord{ID: userID, Email: "admin@example.com", Name: "Admin", Role: auth.RoleSuperAdmin, IsActive: true, CompanyActive: true, AdminTOTPEnabled: true, AdminTOTPSecret: &secret}, nil
			},
		}
		handler := newAdminTestHandler(authSvc, users, "", false)
		code := mustGenerateTOTPCode(t, secret)

		req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"`+code+`"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, "csrf-123")
		req.AddCookie(mustGenerateAdminChallengeCookie(t, authSvc, auth.ScopeAdminChallengeMFA, "admin-mfa", "admin@example.com", "", "", time.Minute))
		w := httptest.NewRecorder()

		handler.Routes().ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}
		adminCookie := cookieNamed(w.Result().Cookies(), adminCookieName)
		if adminCookie == nil || adminCookie.Value == "" {
			t.Fatalf("missing final %s cookie", adminCookieName)
		}
		if _, err := authSvc.ValidateAdminSessionToken(adminCookie.Value); err != nil {
			t.Fatalf("ValidateAdminSessionToken failed: %v", err)
		}
		challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
		if challengeCookie == nil || challengeCookie.MaxAge != -1 {
			t.Fatalf("expected cleared %s cookie after MFA completion", adminChallengeCookieName)
		}
	})
}

func TestAdminPasswordRateLimit(t *testing.T) {
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

	t.Run("repeated bad passwords hit 429 with retry-after", func(t *testing.T) {
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
			if attempt == 3 {
				if got := w.Header().Get("Retry-After"); got != "60" {
					t.Fatalf("Retry-After = %q, want %q", got, "60")
				}
			}
		}
	})

	t.Run("successful password resets only the password bucket", func(t *testing.T) {
		passwordLimiter, totpLimiter := newAdminRateLimiters(2, time.Minute)
		handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)

		badReq := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"wrong-pass"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-password-1")
		badResp := httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusUnauthorized {
			t.Fatalf("first bad password status = %d, want %d", badResp.Code, http.StatusUnauthorized)
		}

		goodReq := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"secret123"}`))
		markSecureAdminRequest(goodReq)
		addJSONCSRF(goodReq, "csrf-password-2")
		goodResp := httptest.NewRecorder()
		handler.Routes().ServeHTTP(goodResp, goodReq)
		if goodResp.Code != http.StatusOK {
			t.Fatalf("good password status = %d, want %d", goodResp.Code, http.StatusOK)
		}
		resp := decodeAdminLoginResponse(t, goodResp)
		if resp.Status != string(adminTOTPStateSetupRequired) {
			t.Fatalf("status = %q, want %q", resp.Status, adminTOTPStateSetupRequired)
		}
		if adminCookie := cookieNamed(goodResp.Result().Cookies(), adminCookieName); adminCookie == nil || adminCookie.MaxAge != -1 {
			t.Fatalf("expected cleared %s cookie after password phase", adminCookieName)
		}

		badReq = httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"wrong-pass"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-password-3")
		badResp = httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusUnauthorized {
			t.Fatalf("bad password after success status = %d, want %d", badResp.Code, http.StatusUnauthorized)
		}
	})
}

func TestAdminTOTPRateLimit(t *testing.T) {
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

	loginChallenge := func(t *testing.T, handler *Handler, csrf string) *http.Cookie {
		t.Helper()
		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(`{"email":"admin@example.com","password":"secret123"}`))
		markSecureAdminRequest(req)
		addJSONCSRF(req, csrf)
		w := httptest.NewRecorder()
		handler.Routes().ServeHTTP(w, req)
		if w.Code != http.StatusOK {
			t.Fatalf("password phase status = %d, want %d", w.Code, http.StatusOK)
		}
		challengeCookie := cookieNamed(w.Result().Cookies(), adminChallengeCookieName)
		if challengeCookie == nil || challengeCookie.Value == "" {
			t.Fatalf("missing %s cookie", adminChallengeCookieName)
		}
		return challengeCookie
	}

	t.Run("repeated bad totp codes hit 429 with retry-after", func(t *testing.T) {
		passwordLimiter, totpLimiter := newAdminRateLimiters(2, time.Minute)
		handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)
		challengeCookie := loginChallenge(t, handler, "csrf-totp-login")

		for attempt := 1; attempt <= 3; attempt++ {
			req := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
			markSecureAdminRequest(req)
			addJSONCSRF(req, "csrf-totp-verify")
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
			if attempt == 3 {
				if got := w.Header().Get("Retry-After"); got != "60" {
					t.Fatalf("Retry-After = %q, want %q", got, "60")
				}
			}
		}
	})

	t.Run("successful password does not reset the totp bucket", func(t *testing.T) {
		passwordLimiter, totpLimiter := newAdminRateLimiters(1, time.Minute)
		handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)
		challengeCookie := loginChallenge(t, handler, "csrf-mfa-password-1")

		badReq := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-mfa-1")
		badReq.AddCookie(challengeCookie)
		badResp := httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusUnauthorized {
			t.Fatalf("first bad totp status = %d, want %d", badResp.Code, http.StatusUnauthorized)
		}

		challengeCookie = loginChallenge(t, handler, "csrf-mfa-password-2")
		badReq = httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-mfa-2")
		badReq.AddCookie(challengeCookie)
		badResp = httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusTooManyRequests {
			t.Fatalf("bad totp after another successful password status = %d, want %d", badResp.Code, http.StatusTooManyRequests)
		}
	})

	t.Run("successful totp resets the totp bucket", func(t *testing.T) {
		passwordLimiter, totpLimiter := newAdminRateLimiters(2, time.Minute)
		handler := newAdminTestHandlerWithLimiters(authSvc, users, "", false, passwordLimiter, totpLimiter)
		challengeCookie := loginChallenge(t, handler, "csrf-mfa-reset-password-1")

		badReq := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-mfa-reset-1")
		badReq.AddCookie(challengeCookie)
		badResp := httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusUnauthorized {
			t.Fatalf("first bad totp status = %d, want %d", badResp.Code, http.StatusUnauthorized)
		}

		goodReq := httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"`+mustGenerateTOTPCode(t, secret)+`"}`))
		markSecureAdminRequest(goodReq)
		addJSONCSRF(goodReq, "csrf-mfa-reset-2")
		goodReq.AddCookie(challengeCookie)
		goodResp := httptest.NewRecorder()
		handler.Routes().ServeHTTP(goodResp, goodReq)
		if goodResp.Code != http.StatusOK {
			t.Fatalf("valid totp status = %d, want %d", goodResp.Code, http.StatusOK)
		}

		challengeCookie = loginChallenge(t, handler, "csrf-mfa-reset-password-2")
		badReq = httptest.NewRequest(http.MethodPost, "/verify", strings.NewReader(`{"code":"123456"}`))
		markSecureAdminRequest(badReq)
		addJSONCSRF(badReq, "csrf-mfa-reset-3")
		badReq.AddCookie(challengeCookie)
		badResp = httptest.NewRecorder()
		handler.Routes().ServeHTTP(badResp, badReq)
		if badResp.Code != http.StatusUnauthorized {
			t.Fatalf("bad totp after successful reset status = %d, want %d", badResp.Code, http.StatusUnauthorized)
		}
	})
}
