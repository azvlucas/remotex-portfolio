package auth

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"reflect"
	"testing"
	"time"

	dto "github.com/prometheus/client_model/go"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"

	chimiddleware "github.com/go-chi/chi/v5/middleware"
)

type fakeRateLimitStore struct {
	incrKeys   []string
	delKeys    []string
	expireKeys []string
	counts     map[string]int64
}

func (f *fakeRateLimitStore) Incr(_ context.Context, key string) (int64, error) {
	f.incrKeys = append(f.incrKeys, key)
	if f.counts == nil {
		f.counts = make(map[string]int64)
	}
	f.counts[key]++
	return f.counts[key], nil
}

func (f *fakeRateLimitStore) Expire(_ context.Context, key string, _ time.Duration) error {
	f.expireKeys = append(f.expireKeys, key)
	return nil
}

func (f *fakeRateLimitStore) Del(_ context.Context, key string) error {
	f.delKeys = append(f.delKeys, key)
	delete(f.counts, key)
	return nil
}

type failingRateLimitStore struct {
	err error
}

func (f failingRateLimitStore) Incr(context.Context, string) (int64, error) {
	return 0, f.err
}

func (f failingRateLimitStore) Expire(context.Context, string, time.Duration) error {
	return f.err
}

func (f failingRateLimitStore) Del(context.Context, string) error {
	return f.err
}

func newAuthTestMetrics(t *testing.T) *observability.Metrics {
	t.Helper()

	metrics, err := observability.NewMetrics("auth-test")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}
	return metrics
}

func authAbuseCounterValue(t *testing.T, metrics *observability.Metrics, surface, reason string) float64 {
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
			if labelValue(metric.GetLabel(), "surface") == surface && labelValue(metric.GetLabel(), "reason") == reason {
				return metric.GetCounter().GetValue()
			}
		}
		return 0
	}
	return 0
}

func labelValue(labels []*dto.LabelPair, name string) string {
	for _, label := range labels {
		if label.GetName() == name {
			return label.GetValue()
		}
	}
	return ""
}

func TestRateLimitClientIPStripsPortFromDirectRemoteAddr(t *testing.T) {
	req := httptest.NewRequest(http.MethodPost, "/auth/login", nil)
	req.RemoteAddr = "203.0.113.10:54321"

	if got := rateLimitClientIP(req); got != "203.0.113.10" {
		t.Fatalf("rateLimitClientIP() = %q, want %q", got, "203.0.113.10")
	}
}

func TestRateLimitClientIPUsesCanonicalRemoteAddrAfterRealIP(t *testing.T) {
	req := httptest.NewRequest(http.MethodPost, "/auth/login", nil)
	req.RemoteAddr = "10.0.0.5:60000"
	req.Header.Set("X-Forwarded-For", "198.51.100.20, 10.0.0.5")

	var got string
	chimiddleware.RealIP(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = rateLimitClientIP(r)
		w.WriteHeader(http.StatusNoContent)
	})).ServeHTTP(httptest.NewRecorder(), req)

	if got != "198.51.100.20" {
		t.Fatalf("rateLimitClientIP() after RealIP = %q, want %q", got, "198.51.100.20")
	}
}

func TestRateLimitClientIPIgnoresRawForwardingHeaders(t *testing.T) {
	req := httptest.NewRequest(http.MethodPost, "/auth/login", nil)
	req.RemoteAddr = "203.0.113.10:54321"
	req.Header.Set("X-Forwarded-For", "198.51.100.20, 10.0.0.5")
	req.Header.Set("X-Real-IP", "198.51.100.21")

	if got := rateLimitClientIP(req); got != "203.0.113.10" {
		t.Fatalf("rateLimitClientIP() = %q, want %q", got, "203.0.113.10")
	}
}

func TestRateLimitKeyNamespacesBuckets(t *testing.T) {
	passwordKey, err := rateLimitKey(RateLimitBucketAdminPassword, "198.51.100.40:4321")
	if err != nil {
		t.Fatalf("rateLimitKey(admin password) error = %v", err)
	}
	totpKey, err := rateLimitKey(RateLimitBucketAdminTOTP, "198.51.100.40:4321")
	if err != nil {
		t.Fatalf("rateLimitKey(admin totp) error = %v", err)
	}
	loginKey, err := rateLimitKey(RateLimitBucketLogin, "198.51.100.40:4321")
	if err != nil {
		t.Fatalf("rateLimitKey(login) error = %v", err)
	}

	if passwordKey != "remotex:ratelimit:admin_password:198.51.100.40" {
		t.Fatalf("passwordKey = %q", passwordKey)
	}
	if totpKey != "remotex:ratelimit:admin_totp:198.51.100.40" {
		t.Fatalf("totpKey = %q", totpKey)
	}
	if loginKey != "remotex:ratelimit:login:198.51.100.40" {
		t.Fatalf("loginKey = %q", loginKey)
	}
	if passwordKey == totpKey || passwordKey == loginKey || totpKey == loginKey {
		t.Fatalf("expected distinct keys, got password=%q totp=%q login=%q", passwordKey, totpKey, loginKey)
	}
}

func TestLoginRateLimitRejectsMissingStoreConfiguration(t *testing.T) {
	metrics := newAuthTestMetrics(t)
	rl := NewBucketRateLimiter(nil, RateLimiterConfig{MaxAttempts: 2, Window: time.Minute}, RateLimitBucketLogin)

	if rl.Allow(context.Background(), "198.51.100.70") {
		t.Fatal("Allow() = true, want false")
	}
	if got := authAbuseCounterValue(t, metrics, observability.AuthAbuseSurfacePublicLogin, observability.AuthAbuseReasonStoreError); got != 1 {
		t.Fatalf("store_error counter = %v, want 1", got)
	}
}

func TestLoginRateLimiterResetUsesPublicBucketKey(t *testing.T) {
	store := &fakeRateLimitStore{}
	rl := NewBucketRateLimiter(store, RateLimiterConfig{
		MaxAttempts: 5,
		Window:      15 * time.Minute,
	}, RateLimitBucketLogin)

	ip := normalizeClientIP("198.51.100.31:65432")
	if !rl.Allow(context.Background(), ip) {
		t.Fatal("Allow() = false, want true")
	}

	rl.Reset(context.Background(), ip)

	wantKeys := []string{"remotex:ratelimit:login:198.51.100.31"}
	if !reflect.DeepEqual(store.incrKeys, wantKeys) {
		t.Fatalf("Allow key = %v, want %v", store.incrKeys, wantKeys)
	}
	if !reflect.DeepEqual(store.delKeys, wantKeys) {
		t.Fatalf("Reset key = %v, want %v", store.delKeys, wantKeys)
	}
}

func TestBucketRateLimiterRejectsInvalidBucket(t *testing.T) {
	metrics := newAuthTestMetrics(t)
	store := &fakeRateLimitStore{}
	rl := NewBucketRateLimiter(store, RateLimiterConfig{MaxAttempts: 2, Window: time.Minute}, "admin password")

	if rl.Allow(context.Background(), "198.51.100.30") {
		t.Fatal("Allow() = true, want false")
	}
	if len(store.incrKeys) != 0 {
		t.Fatalf("incrKeys = %v, want none", store.incrKeys)
	}
	if got := authAbuseCounterValue(t, metrics, observability.AuthAbuseSurfaceUnknown, observability.AuthAbuseReasonRejected); got != 1 {
		t.Fatalf("rejected counter = %v, want 1", got)
	}
}

func TestRateLimiterRecordsPublicLoginDenials(t *testing.T) {
	metrics := newAuthTestMetrics(t)
	store := &fakeRateLimitStore{}
	rl := NewBucketRateLimiter(store, RateLimiterConfig{MaxAttempts: 2, Window: time.Minute}, RateLimitBucketLogin)

	for attempt := 1; attempt <= 2; attempt++ {
		if !rl.Allow(context.Background(), "198.51.100.77") {
			t.Fatalf("attempt %d Allow() = false, want true", attempt)
		}
	}
	if got := authAbuseCounterValue(t, metrics, observability.AuthAbuseSurfacePublicLogin, observability.AuthAbuseReasonLimited); got != 0 {
		t.Fatalf("limited counter before denial = %v, want 0", got)
	}
	if rl.Allow(context.Background(), "198.51.100.77") {
		t.Fatal("third Allow() = true, want false")
	}
	if got := authAbuseCounterValue(t, metrics, observability.AuthAbuseSurfacePublicLogin, observability.AuthAbuseReasonLimited); got != 1 {
		t.Fatalf("limited counter after denial = %v, want 1", got)
	}
}

func TestRateLimiterRecordsPublicLoginStoreErrors(t *testing.T) {
	metrics := newAuthTestMetrics(t)
	rl := NewBucketRateLimiter(failingRateLimitStore{err: errors.New("redis down")}, RateLimiterConfig{MaxAttempts: 2, Window: time.Minute}, RateLimitBucketLogin)

	if rl.Allow(context.Background(), "198.51.100.78") {
		t.Fatal("Allow() = true, want false")
	}
	if got := authAbuseCounterValue(t, metrics, observability.AuthAbuseSurfacePublicLogin, observability.AuthAbuseReasonStoreError); got != 1 {
		t.Fatalf("store_error counter = %v, want 1", got)
	}
}

func TestRateLimiterResetUsesSameNamespacedKeyAsAllow(t *testing.T) {
	store := &fakeRateLimitStore{}
	rl := NewBucketRateLimiter(store, RateLimiterConfig{
		MaxAttempts: 5,
		Window:      15 * time.Minute,
	}, RateLimitBucketAdminPassword)

	ip := normalizeClientIP("198.51.100.30:65432")
	if !rl.Allow(context.Background(), ip) {
		t.Fatal("Allow() = false, want true")
	}

	rl.Reset(context.Background(), ip)

	wantKeys := []string{"remotex:ratelimit:admin_password:198.51.100.30"}
	if !reflect.DeepEqual(store.incrKeys, wantKeys) {
		t.Fatalf("Allow key = %v, want %v", store.incrKeys, wantKeys)
	}
	if !reflect.DeepEqual(store.delKeys, wantKeys) {
		t.Fatalf("Reset key = %v, want %v", store.delKeys, wantKeys)
	}
}

func TestFallbackRateLimitStoreEnforcesBoundaryWhenPrimaryFails(t *testing.T) {
	store := NewFallbackRateLimitStore(
		failingRateLimitStore{err: errors.New("redis down")},
		NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 8}),
	)
	rl := NewBucketRateLimiter(store, RateLimiterConfig{MaxAttempts: 2, Window: time.Minute}, RateLimitBucketAdminTOTP)

	if !rl.Allow(context.Background(), "198.51.100.55") {
		t.Fatal("first Allow() = false, want true")
	}
	if !rl.Allow(context.Background(), "198.51.100.55") {
		t.Fatal("second Allow() = false, want true")
	}
	if rl.Allow(context.Background(), "198.51.100.55") {
		t.Fatal("third Allow() = true, want false")
	}

	rl.Reset(context.Background(), "198.51.100.55")
	if !rl.Allow(context.Background(), "198.51.100.55") {
		t.Fatal("Allow() after Reset = false, want true")
	}
}
