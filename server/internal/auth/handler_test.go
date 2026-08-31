package auth

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

type fakeUserStore struct {
	emailExistsFn     func(ctx context.Context, email string) (bool, error)
	createUserFn      func(ctx context.Context, input CreateUserInput) (*UserRecord, error)
	getUserByEmailFn  func(ctx context.Context, email string) (*UserRecord, error)
	getUserByIDFn     func(ctx context.Context, userID string) (*UserRecord, error)
	getMachineByIDFn  func(ctx context.Context, machineID string) (*MachineRecord, error)
	touchUserFn       func(ctx context.Context, userID string, updatedAt time.Time) error
	enableAdminTOTPFn func(ctx context.Context, userID, secret string, updatedAt time.Time) error
}

func (f *fakeUserStore) EmailExists(ctx context.Context, email string) (bool, error) {
	if f.emailExistsFn != nil {
		return f.emailExistsFn(ctx, email)
	}
	return false, nil
}

func (f *fakeUserStore) CreateUser(ctx context.Context, input CreateUserInput) (*UserRecord, error) {
	if f.createUserFn != nil {
		return f.createUserFn(ctx, input)
	}
	return &UserRecord{ID: "user-1", Email: input.Email, Name: input.Name, Role: RoleCompanyUser, IsActive: true, CompanyActive: true}, nil
}

func (f *fakeUserStore) GetUserByEmail(ctx context.Context, email string) (*UserRecord, error) {
	if f.getUserByEmailFn != nil {
		return f.getUserByEmailFn(ctx, email)
	}
	return nil, ErrUserNotFound
}

func (f *fakeUserStore) GetUserByID(ctx context.Context, userID string) (*UserRecord, error) {
	if f.getUserByIDFn != nil {
		return f.getUserByIDFn(ctx, userID)
	}
	return &UserRecord{ID: userID, Email: "user@example.com", Name: "User", Role: RoleCompanyUser, IsActive: true, CompanyActive: true}, nil
}

func (f *fakeUserStore) GetMachineByID(ctx context.Context, machineID string) (*MachineRecord, error) {
	if f.getMachineByIDFn != nil {
		return f.getMachineByIDFn(ctx, machineID)
	}
	return &MachineRecord{ID: machineID, CompanyID: "company-1", Name: "Host Machine"}, nil
}

func (f *fakeUserStore) TouchUser(ctx context.Context, userID string, updatedAt time.Time) error {
	if f.touchUserFn != nil {
		return f.touchUserFn(ctx, userID, updatedAt)
	}
	return nil
}

func (f *fakeUserStore) EnableAdminTOTP(ctx context.Context, userID, secret string, updatedAt time.Time) error {
	if f.enableAdminTOTPFn != nil {
		return f.enableAdminTOTPFn(ctx, userID, secret, updatedAt)
	}
	return nil
}

func TestLoginSuccess(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			return &UserRecord{
				ID:            "user-123",
				Email:         email,
				PasswordHash:  hash,
				Name:          "Lucas",
				Role:          RoleCompanyUser,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}

	handler := newHandlerWithStore(svc, store, nil)
	router := handler.Routes()

	body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"secret123"}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()

	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp authResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.UserID != "user-123" {
		t.Fatalf("user_id = %q, want %q", resp.UserID, "user-123")
	}
	if resp.AccessToken == "" || resp.RefreshToken == "" {
		t.Fatal("expected access and refresh tokens in response")
	}
}

func TestLoginInvalidPassword(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			return &UserRecord{
				ID:            "user-123",
				Email:         email,
				PasswordHash:  hash,
				Role:          RoleCompanyUser,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}

	handler := newHandlerWithStore(svc, store, nil)
	router := handler.Routes()

	body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"wrong-pass"}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()

	router.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}
}

func TestLoginRejectsEmptyCredentials(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	handler := newHandlerWithStore(svc, &fakeUserStore{}, nil)
	router := handler.Routes()

	body := bytes.NewBufferString(`{"email":"   ","password":""}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()

	router.ServeHTTP(w, req)

	if w.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusBadRequest)
	}

	var resp errorResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Error != "email and password are required" {
		t.Fatalf("error = %q, want %q", resp.Error, "email and password are required")
	}
}

func TestLoginRateLimitFallbackOnlyReturnsRetryAfter(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			return &UserRecord{
				ID:            "user-123",
				Email:         email,
				PasswordHash:  hash,
				Role:          RoleCompanyUser,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}
	limiter := NewBucketRateLimiter(
		NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 16}),
		RateLimiterConfig{MaxAttempts: 2, Window: 30 * time.Second},
		RateLimitBucketLogin,
	)
	handler := newHandlerWithStore(svc, store, limiter)
	router := handler.Routes()

	for attempt := 1; attempt <= 2; attempt++ {
		body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"wrong-pass"}`)
		req := httptest.NewRequest(http.MethodPost, "/login", body)
		req.RemoteAddr = "127.0.0.1:54321"
		w := httptest.NewRecorder()
		router.ServeHTTP(w, req)
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("attempt %d status = %d, want %d", attempt, w.Code, http.StatusUnauthorized)
		}
	}

	body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"wrong-pass"}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusTooManyRequests)
	}
	if got := w.Header().Get("Retry-After"); got != "30" {
		t.Fatalf("Retry-After = %q, want %q", got, "30")
	}

	var resp errorResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Error != "too many attempts, try again later" {
		t.Fatalf("error = %q, want %q", resp.Error, "too many attempts, try again later")
	}
}

func TestLoginRateLimitPrimaryStoreFailureReturnsRetryAfter(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			return &UserRecord{
				ID:            "user-123",
				Email:         email,
				PasswordHash:  hash,
				Role:          RoleCompanyUser,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}
	limiter := NewBucketRateLimiter(
		NewFallbackRateLimitStore(
			failingRateLimitStore{err: context.DeadlineExceeded},
			NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 16}),
		),
		RateLimiterConfig{MaxAttempts: 2, Window: 45 * time.Second},
		RateLimitBucketLogin,
	)
	handler := newHandlerWithStore(svc, store, limiter)
	router := handler.Routes()

	for attempt := 1; attempt <= 2; attempt++ {
		body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"wrong-pass"}`)
		req := httptest.NewRequest(http.MethodPost, "/login", body)
		req.RemoteAddr = "127.0.0.1:54321"
		w := httptest.NewRecorder()
		router.ServeHTTP(w, req)
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("attempt %d status = %d, want %d", attempt, w.Code, http.StatusUnauthorized)
		}
	}

	body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"wrong-pass"}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusTooManyRequests)
	}
	if got := w.Header().Get("Retry-After"); got != "45" {
		t.Fatalf("Retry-After = %q, want %q", got, "45")
	}
}

func TestLoginRateLimitSuccessfulLoginResetsPublicBucket(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			companyID := "company-1"
			return &UserRecord{
				ID:            "user-123",
				Email:         email,
				PasswordHash:  hash,
				Name:          "Lucas",
				Role:          RoleCompanyUser,
				CompanyID:     &companyID,
				IsActive:      true,
				CompanyActive: true,
			}, nil
		},
	}
	limiter := NewBucketRateLimiter(
		NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 16}),
		RateLimiterConfig{MaxAttempts: 2, Window: time.Minute},
		RateLimitBucketLogin,
	)
	handler := newHandlerWithStore(svc, store, limiter)
	router := handler.Routes()

	attempts := []struct {
		password           string
		wantStatus         int
		wantRetryAfter     string
		wantAccessTokenSet bool
	}{
		{password: "wrong-pass", wantStatus: http.StatusUnauthorized},
		{password: "secret123", wantStatus: http.StatusOK, wantAccessTokenSet: true},
		{password: "wrong-pass", wantStatus: http.StatusUnauthorized},
		{password: "wrong-pass", wantStatus: http.StatusUnauthorized},
		{password: "wrong-pass", wantStatus: http.StatusTooManyRequests, wantRetryAfter: "60"},
	}

	for idx, attempt := range attempts {
		body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"` + attempt.password + `"}`)
		req := httptest.NewRequest(http.MethodPost, "/login", body)
		req.RemoteAddr = "127.0.0.1:54321"
		w := httptest.NewRecorder()
		router.ServeHTTP(w, req)

		if w.Code != attempt.wantStatus {
			t.Fatalf("attempt %d status = %d, want %d", idx+1, w.Code, attempt.wantStatus)
		}
		if attempt.wantRetryAfter != "" {
			if got := w.Header().Get("Retry-After"); got != attempt.wantRetryAfter {
				t.Fatalf("attempt %d Retry-After = %q, want %q", idx+1, got, attempt.wantRetryAfter)
			}
		}
		if attempt.wantAccessTokenSet {
			var resp authResponse
			if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
				t.Fatalf("attempt %d decode response: %v", idx+1, err)
			}
			if resp.AccessToken == "" {
				t.Fatalf("attempt %d expected access token", idx+1)
			}
		}
	}
}

func TestPublicLoginRejectsSuperAdmin(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	hash, err := svc.HashPassword("secret123")
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	t.Run("rejects super admin on public login", func(t *testing.T) {
		store := &fakeUserStore{
			getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
				return &UserRecord{
					ID:            "admin-123",
					Email:         email,
					PasswordHash:  hash,
					Name:          "Admin",
					Role:          RoleSuperAdmin,
					IsActive:      true,
					CompanyActive: true,
				}, nil
			},
		}

		handler := newHandlerWithStore(svc, store, nil)
		router := handler.Routes()

		body := bytes.NewBufferString(`{"email":"admin@example.com","password":"secret123"}`)
		req := httptest.NewRequest(http.MethodPost, "/login", body)
		req.RemoteAddr = "127.0.0.1:54321"
		w := httptest.NewRecorder()

		router.ServeHTTP(w, req)

		if w.Code != http.StatusForbidden {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusForbidden)
		}

		var resp errorResponse
		if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
			t.Fatalf("decode response: %v", err)
		}
		if resp.Error != "admin login requires /admin/login" {
			t.Fatalf("error = %q, want %q", resp.Error, "admin login requires /admin/login")
		}
	})

	t.Run("keeps company user login working", func(t *testing.T) {
		store := &fakeUserStore{
			getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
				companyID := "company-1"
				return &UserRecord{
					ID:            "user-123",
					Email:         email,
					PasswordHash:  hash,
					Name:          "Company User",
					Role:          RoleCompanyUser,
					CompanyID:     &companyID,
					IsActive:      true,
					CompanyActive: true,
				}, nil
			},
		}

		handler := newHandlerWithStore(svc, store, nil)
		router := handler.Routes()

		body := bytes.NewBufferString(`{"email":"user@example.com","password":"secret123"}`)
		req := httptest.NewRequest(http.MethodPost, "/login", body)
		req.RemoteAddr = "127.0.0.1:54321"
		w := httptest.NewRecorder()

		router.ServeHTTP(w, req)

		if w.Code != http.StatusOK {
			t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
		}

		var resp authResponse
		if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
			t.Fatalf("decode response: %v", err)
		}
		if resp.Role != RoleCompanyUser {
			t.Fatalf("role = %q, want %q", resp.Role, RoleCompanyUser)
		}
		if resp.AccessToken == "" || resp.RefreshToken == "" {
			t.Fatal("expected access and refresh tokens in response")
		}
	})
}

func TestLoginStoreTimeoutReturnsInternalError(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	store := &fakeUserStore{
		getUserByEmailFn: func(ctx context.Context, email string) (*UserRecord, error) {
			<-ctx.Done()
			return nil, ctx.Err()
		},
	}

	handler := newHandlerWithStore(svc, store, nil)
	handler.queryTimeout = 20 * time.Millisecond
	router := handler.Routes()

	body := bytes.NewBufferString(`{"email":"lucas@example.com","password":"secret123"}`)
	req := httptest.NewRequest(http.MethodPost, "/login", body)
	req.RemoteAddr = "127.0.0.1:54321"
	w := httptest.NewRecorder()

	start := time.Now()
	router.ServeHTTP(w, req)
	elapsed := time.Since(start)

	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusInternalServerError)
	}
	if elapsed > 500*time.Millisecond {
		t.Fatalf("login took too long: %v", elapsed)
	}

	var resp errorResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Error != "internal error" {
		t.Fatalf("error = %q, want %q", resp.Error, "internal error")
	}
}

func TestRefreshRejectsDeletedMachine(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	store := &fakeUserStore{
		getMachineByIDFn: func(ctx context.Context, machineID string) (*MachineRecord, error) {
			return nil, ErrMachineNotFound
		},
	}

	handler := newHandlerWithStore(svc, store, nil)
	router := handler.Routes()

	pair, err := svc.GenerateTokenPair(TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: SubjectTypeMachine,
		MachineID:   "machine-1",
	})
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}

	body := bytes.NewBufferString(`{"refresh_token":"` + pair.RefreshToken + `"}`)
	req := httptest.NewRequest(http.MethodPost, "/refresh", body)
	w := httptest.NewRecorder()

	router.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}

	var resp errorResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Error != "machine not found" {
		t.Fatalf("error = %q, want %q", resp.Error, "machine not found")
	}
}

func TestRefreshRejectsDetachedMachine(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))
	store := &fakeUserStore{
		getMachineByIDFn: func(ctx context.Context, machineID string) (*MachineRecord, error) {
			return &MachineRecord{ID: machineID, CompanyID: "", Name: "Detached Host"}, nil
		},
	}

	handler := newHandlerWithStore(svc, store, nil)
	router := handler.Routes()

	pair, err := svc.GenerateTokenPair(TokenSubject{
		UserID:      "machine-1",
		Name:        "Host Machine",
		Role:        RoleMachineAgent,
		CompanyID:   "company-1",
		SubjectType: SubjectTypeMachine,
		MachineID:   "machine-1",
	})
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}

	body := bytes.NewBufferString(`{"refresh_token":"` + pair.RefreshToken + `"}`)
	req := httptest.NewRequest(http.MethodPost, "/refresh", body)
	w := httptest.NewRecorder()

	router.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusUnauthorized)
	}

	var resp errorResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Error != "machine not found" {
		t.Fatalf("error = %q, want %q", resp.Error, "machine not found")
	}
}
