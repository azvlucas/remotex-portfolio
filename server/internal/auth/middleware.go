package auth

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

type contextKey string

const (
	userContextKey  contextKey = "auth_user"
	adminCookieName            = "remotex_admin_token"
)

// UserFromContext extrai os claims do usuário autenticado do contexto.
func UserFromContext(ctx context.Context) (*Claims, bool) {
	claims, ok := ctx.Value(userContextKey).(*Claims)
	return claims, ok
}

// JWTAuthMiddleware valida o JWT e sincroniza status/escopo do usuário a partir do banco.
func JWTAuthMiddleware(svc *Service, users UserStore) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			tokenStr := extractToken(r)
			if tokenStr == "" {
				writeAuthError(w, http.StatusUnauthorized, missingAuthorizationMessage(r))
				return
			}

			claims, err := svc.ValidateToken(tokenStr)
			if err != nil {
				msg := "invalid token"
				if err == ErrTokenExpired {
					msg = "token expired"
				}
				writeAuthError(w, http.StatusUnauthorized, msg)
				return
			}

			if claims.IsMachine() {
				if users != nil {
					machine, err := users.GetMachineByID(r.Context(), claims.MachineID)
					if err != nil {
						if err == ErrMachineNotFound {
							writeAuthError(w, http.StatusUnauthorized, "machine not found")
							return
						}
						slog.Error("auth middleware machine lookup failed", "machine_id", claims.MachineID, "err", err)
						writeAuthError(w, http.StatusInternalServerError, "internal error")
						return
					}
					if machine.CompanyID != claims.CompanyID {
						writeAuthError(w, http.StatusUnauthorized, "machine not found")
						return
					}

					claims.Name = machine.Name
				}
				ctx := context.WithValue(r.Context(), userContextKey, claims)
				next.ServeHTTP(w, r.WithContext(ctx))
				return
			}

			if users != nil {
				user, err := users.GetUserByID(r.Context(), claims.UserID)
				if err != nil {
					if err == ErrUserNotFound {
						writeAuthError(w, http.StatusUnauthorized, "user not found")
						return
					}
					slog.Error("auth middleware lookup failed", "user_id", claims.UserID, "err", err)
					writeAuthError(w, http.StatusInternalServerError, "internal error")
					return
				}
				if !user.IsActive {
					writeAuthError(w, http.StatusForbidden, "user is inactive")
					return
				}
				if user.CompanyID != nil && !user.CompanyActive {
					writeAuthError(w, http.StatusForbidden, "company is inactive")
					return
				}

				claims.Email = user.Email
				claims.Name = user.Name
				claims.Role = user.Role
				if user.CompanyID != nil {
					claims.CompanyID = *user.CompanyID
				} else {
					claims.CompanyID = ""
				}
			}

			ctx := context.WithValue(r.Context(), userContextKey, claims)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

// RequireAdminMiddleware limita o acesso ao super admin.
func RequireAdminMiddleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, ok := UserFromContext(r.Context())
			if !ok || claims == nil {
				writeAuthError(w, http.StatusUnauthorized, "unauthorized")
				return
			}
			if claims.Role != RoleSuperAdmin {
				writeAuthError(w, http.StatusForbidden, "admin access required")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// RequireCompanyMiddleware exige que o token pertença a um usuário de empresa ativo.
func RequireCompanyMiddleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, ok := UserFromContext(r.Context())
			if !ok || claims == nil {
				writeAuthError(w, http.StatusUnauthorized, "unauthorized")
				return
			}
			if claims.CompanyID == "" {
				writeAuthError(w, http.StatusForbidden, "company access required")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// RequireCompanyManagerMiddleware limita o acesso a company_manager ou super_admin com empresa.
func RequireCompanyManagerMiddleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, ok := UserFromContext(r.Context())
			if !ok || claims == nil {
				writeAuthError(w, http.StatusUnauthorized, "unauthorized")
				return
			}
			if claims.CompanyID == "" {
				writeAuthError(w, http.StatusForbidden, "company access required")
				return
			}
			if claims.Role != RoleCompanyManager && claims.Role != RoleSuperAdmin {
				writeAuthError(w, http.StatusForbidden, "manager access required")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// RequireHumanCompanyMiddleware limita o acesso a usuários humanos da empresa.
func RequireHumanCompanyMiddleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, ok := UserFromContext(r.Context())
			if !ok || claims == nil {
				writeAuthError(w, http.StatusUnauthorized, "unauthorized")
				return
			}
			if claims.IsMachine() {
				writeAuthError(w, http.StatusForbidden, "human user required")
				return
			}
			if claims.CompanyID == "" {
				writeAuthError(w, http.StatusForbidden, "company access required")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

func missingAuthorizationMessage(r *http.Request) string {
	authHeader := strings.TrimSpace(r.Header.Get("Authorization"))
	if authHeader != "" {
		parts := strings.Fields(authHeader)
		if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") || strings.TrimSpace(parts[1]) == "" {
			return "malformed Authorization header"
		}
	}

	queryValues := r.URL.Query()
	if strings.TrimSpace(queryValues.Get("token")) != "" {
		return "query-string bearer token is no longer supported; use Authorization header"
	}

	return "missing authorization token"
}

func extractToken(r *http.Request) string {
	authHeader := strings.TrimSpace(r.Header.Get("Authorization"))
	if authHeader != "" {
		parts := strings.Fields(authHeader)
		if len(parts) == 2 && strings.EqualFold(parts[0], "Bearer") {
			return strings.TrimSpace(parts[1])
		}
	}

	if cookie, err := r.Cookie(adminCookieName); err == nil && strings.TrimSpace(cookie.Value) != "" {
		return strings.TrimSpace(cookie.Value)
	}

	return ""
}

func writeAuthError(w http.ResponseWriter, status int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write([]byte(`{"error":"` + message + `"}`))
}

// ——— Rate Limiting ————————————————————————————————————————————————————————————————

const (
	RateLimitBucketLogin         = "login"
	RateLimitBucketAdminPassword = "admin_password"
	RateLimitBucketAdminTOTP     = "admin_totp"
)

// RateLimiterConfig configura o rate limiter.
type RateLimiterConfig struct {
	MaxAttempts int
	Window      time.Duration
}

// InMemoryRateLimitStoreConfig configura o fallback local do rate limiter.
type InMemoryRateLimitStoreConfig struct {
	MaxEntries int
	Now        func() time.Time
}

type rateLimitStore interface {
	Incr(ctx context.Context, key string) (int64, error)
	Expire(ctx context.Context, key string, ttl time.Duration) error
	Del(ctx context.Context, key string) error
}

type redisRateLimitStore struct {
	client *redis.Client
}

func (s redisRateLimitStore) Incr(ctx context.Context, key string) (int64, error) {
	return s.client.Incr(ctx, key).Result()
}

func (s redisRateLimitStore) Expire(ctx context.Context, key string, ttl time.Duration) error {
	return s.client.Expire(ctx, key, ttl).Err()
}

func (s redisRateLimitStore) Del(ctx context.Context, key string) error {
	return s.client.Del(ctx, key).Err()
}

type inMemoryRateLimitEntry struct {
	count       int64
	expiresAt   time.Time
	lastTouched time.Time
}

type inMemoryRateLimitStore struct {
	maxEntries int
	now        func() time.Time
	mu         sync.Mutex
	entries    map[string]inMemoryRateLimitEntry
}

func (s *inMemoryRateLimitStore) Incr(_ context.Context, key string) (int64, error) {
	now := s.now()

	s.mu.Lock()
	defer s.mu.Unlock()

	s.pruneExpiredLocked(now)

	entry, ok := s.entries[key]
	if !ok {
		s.evictIfNeededLocked(now)
		entry = inMemoryRateLimitEntry{}
	}
	entry.count++
	entry.lastTouched = now
	s.entries[key] = entry
	return entry.count, nil
}

func (s *inMemoryRateLimitStore) Expire(_ context.Context, key string, ttl time.Duration) error {
	now := s.now()

	s.mu.Lock()
	defer s.mu.Unlock()

	entry, ok := s.entries[key]
	if !ok {
		return nil
	}
	entry.expiresAt = now.Add(ttl)
	entry.lastTouched = now
	s.entries[key] = entry
	return nil
}

func (s *inMemoryRateLimitStore) Del(_ context.Context, key string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.entries, key)
	return nil
}

func (s *inMemoryRateLimitStore) pruneExpiredLocked(now time.Time) {
	for key, entry := range s.entries {
		if !entry.expiresAt.IsZero() && !now.Before(entry.expiresAt) {
			delete(s.entries, key)
		}
	}
}

func (s *inMemoryRateLimitStore) evictIfNeededLocked(now time.Time) {
	if len(s.entries) < s.maxEntries {
		return
	}

	s.pruneExpiredLocked(now)
	if len(s.entries) < s.maxEntries {
		return
	}

	var oldestKey string
	var oldestTime time.Time
	first := true
	for key, entry := range s.entries {
		candidate := entry.lastTouched
		if candidate.IsZero() {
			candidate = entry.expiresAt
		}
		if first || candidate.Before(oldestTime) {
			oldestKey = key
			oldestTime = candidate
			first = false
		}
	}
	if oldestKey != "" {
		delete(s.entries, oldestKey)
	}
}

type fallbackRateLimitStore struct {
	primary  rateLimitStore
	fallback rateLimitStore
}

func (s fallbackRateLimitStore) Incr(ctx context.Context, key string) (int64, error) {
	var fallbackCount int64
	var fallbackErr error
	if s.fallback != nil {
		fallbackCount, fallbackErr = s.fallback.Incr(ctx, key)
	}
	if s.primary == nil {
		return fallbackCount, fallbackErr
	}

	count, err := s.primary.Incr(ctx, key)
	if err == nil {
		return count, nil
	}
	if s.fallback != nil && fallbackErr == nil {
		return fallbackCount, nil
	}
	return 0, err
}

func (s fallbackRateLimitStore) Expire(ctx context.Context, key string, ttl time.Duration) error {
	var fallbackErr error
	if s.fallback != nil {
		fallbackErr = s.fallback.Expire(ctx, key, ttl)
	}
	if s.primary == nil {
		return fallbackErr
	}

	primaryErr := s.primary.Expire(ctx, key, ttl)
	if primaryErr == nil || (s.fallback != nil && fallbackErr == nil) {
		return nil
	}
	return primaryErr
}

func (s fallbackRateLimitStore) Del(ctx context.Context, key string) error {
	var fallbackErr error
	if s.fallback != nil {
		fallbackErr = s.fallback.Del(ctx, key)
	}
	if s.primary == nil {
		return fallbackErr
	}

	primaryErr := s.primary.Del(ctx, key)
	if primaryErr == nil || (s.fallback != nil && fallbackErr == nil) {
		return nil
	}
	return primaryErr
}

// NewRedisRateLimitStore cria um store Redis para rate limiting.
func NewRedisRateLimitStore(rdb *redis.Client) rateLimitStore {
	if rdb == nil {
		return nil
	}
	return redisRateLimitStore{client: rdb}
}

// NewInMemoryRateLimitStore cria um store local e limitado para fallback de rate limiting.
func NewInMemoryRateLimitStore(cfg InMemoryRateLimitStoreConfig) rateLimitStore {
	if cfg.MaxEntries <= 0 {
		cfg.MaxEntries = 2048
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &inMemoryRateLimitStore{
		maxEntries: cfg.MaxEntries,
		now:        cfg.Now,
		entries:    make(map[string]inMemoryRateLimitEntry),
	}
}

// NewFallbackRateLimitStore combina o store primário com um fallback local.
func NewFallbackRateLimitStore(primary, fallback rateLimitStore) rateLimitStore {
	switch {
	case primary == nil:
		return fallback
	case fallback == nil:
		return primary
	default:
		return fallbackRateLimitStore{primary: primary, fallback: fallback}
	}
}

// RateLimiter usa um store para contar tentativas por IP e bucket.
type RateLimiter struct {
	store     rateLimitStore
	cfg       RateLimiterConfig
	bucket    string
	bucketErr error
}

// RateLimitDecision descreve o resultado de uma verificação de rate limit.
type RateLimitDecision struct {
	Allowed bool
	Reason  string
}

// NewRateLimiter cria o rate limiter padrão de login público.
// Quando Redis não está disponível, usa fallback local limitado para evitar fail-open.
func NewRateLimiter(rdb *redis.Client, cfg RateLimiterConfig) *RateLimiter {
	store := NewRedisRateLimitStore(rdb)
	if store == nil {
		store = NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 2048})
	}
	return NewBucketRateLimiter(store, cfg, RateLimitBucketLogin)
}

// NewBucketRateLimiter cria um rate limiter namespaced para um bucket específico.
func NewBucketRateLimiter(store rateLimitStore, cfg RateLimiterConfig, bucket string) *RateLimiter {
	if cfg.MaxAttempts <= 0 {
		cfg.MaxAttempts = 5
	}
	if cfg.Window <= 0 {
		cfg.Window = 15 * time.Minute
	}

	bucket = strings.TrimSpace(bucket)
	return &RateLimiter{
		store:     store,
		cfg:       cfg,
		bucket:    bucket,
		bucketErr: validateRateLimitBucket(bucket),
	}
}

// Allow verifica se o IP pode fazer mais uma tentativa.
func (rl *RateLimiter) Allow(ctx context.Context, ip string) bool {
	return rl.Check(ctx, ip).Allowed
}

// Check verifica se o IP pode fazer mais uma tentativa e classifica a decisÃ£o.
func (rl *RateLimiter) Check(ctx context.Context, ip string) RateLimitDecision {
	if rl == nil {
		return RateLimitDecision{Allowed: true}
	}
	if rl.bucketErr != nil {
		slog.Error("rate limiter bucket rejected", "bucket", rl.bucket, "err", rl.bucketErr)
		rl.recordAuthAbuse(observability.AuthAbuseReasonRejected)
		return RateLimitDecision{Reason: observability.AuthAbuseReasonRejected}
	}
	if rl.store == nil {
		slog.Error("rate limiter store rejected", "bucket", rl.bucket, "err", "missing store")
		rl.recordAuthAbuse(observability.AuthAbuseReasonStoreError)
		return RateLimitDecision{Reason: observability.AuthAbuseReasonStoreError}
	}

	key, err := rateLimitKey(rl.bucket, ip)
	if err != nil {
		slog.Error("rate limiter key rejected", "bucket", rl.bucket, "err", err)
		rl.recordAuthAbuse(observability.AuthAbuseReasonRejected)
		return RateLimitDecision{Reason: observability.AuthAbuseReasonRejected}
	}

	count, err := rl.store.Incr(ctx, key)
	if err != nil {
		slog.Warn("rate limiter store error", "bucket", rl.bucket, "err", err)
		rl.recordAuthAbuse(observability.AuthAbuseReasonStoreError)
		return RateLimitDecision{Reason: observability.AuthAbuseReasonStoreError}
	}

	if count == 1 {
		if err := rl.store.Expire(ctx, key, rl.cfg.Window); err != nil {
			slog.Warn("rate limiter store error", "bucket", rl.bucket, "err", err)
			rl.recordAuthAbuse(observability.AuthAbuseReasonStoreError)
		}
	}

	if count > int64(rl.cfg.MaxAttempts) {
		rl.recordAuthAbuse(observability.AuthAbuseReasonLimited)
		return RateLimitDecision{Reason: observability.AuthAbuseReasonLimited}
	}
	return RateLimitDecision{Allowed: true}
}

// AllowRequest verifica o rate limit para o IP normalizado do request.
func (rl *RateLimiter) AllowRequest(r *http.Request) bool {
	return rl.CheckRequest(r).Allowed
}

// CheckRequest verifica o rate limit para o IP normalizado do request.
func (rl *RateLimiter) CheckRequest(r *http.Request) RateLimitDecision {
	if rl == nil {
		return RateLimitDecision{Allowed: true}
	}
	return rl.Check(r.Context(), rateLimitClientIP(r))
}

// Reset limpa o contador de tentativas para um IP.
func (rl *RateLimiter) Reset(ctx context.Context, ip string) {
	if rl == nil || rl.store == nil || rl.bucketErr != nil {
		return
	}

	key, err := rateLimitKey(rl.bucket, ip)
	if err != nil {
		slog.Error("rate limiter reset key rejected", "bucket", rl.bucket, "err", err)
		return
	}
	if err := rl.store.Del(ctx, key); err != nil {
		slog.Warn("rate limiter store error", "bucket", rl.bucket, "err", err)
	}
}

// ResetRequest limpa o contador associado ao IP normalizado do request.
func (rl *RateLimiter) ResetRequest(r *http.Request) {
	if rl == nil {
		return
	}
	rl.Reset(r.Context(), rateLimitClientIP(r))
}

// RetryAfterSeconds expõe o valor de Retry-After associado ao bucket.
func (rl *RateLimiter) RetryAfterSeconds() int {
	if rl == nil || rl.cfg.Window <= 0 {
		return 0
	}
	return int(rl.cfg.Window.Seconds())
}

// Bucket retorna o nome do bucket configurado.
func (rl *RateLimiter) Bucket() string {
	if rl == nil {
		return ""
	}
	return rl.bucket
}

// RateLimitMiddleware aplica rate limiting por IP.
func RateLimitMiddleware(rl *RateLimiter) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !rl.CheckRequest(r).Allowed {
				w.Header().Set("Retry-After", fmt.Sprintf("%d", rl.RetryAfterSeconds()))
				writeAuthError(w, http.StatusTooManyRequests, "too many attempts, try again later")
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

func (rl *RateLimiter) recordAuthAbuse(reason string) {
	if rl == nil || reason == "" {
		return
	}
	observability.RecordAuthAbuse(rateLimitSurface(rl.bucket), reason)
}

func rateLimitSurface(bucket string) string {
	switch strings.TrimSpace(bucket) {
	case RateLimitBucketLogin:
		return observability.AuthAbuseSurfacePublicLogin
	case RateLimitBucketAdminPassword:
		return observability.AuthAbuseSurfaceAdminPassword
	case RateLimitBucketAdminTOTP:
		return observability.AuthAbuseSurfaceAdminTOTP
	default:
		return observability.AuthAbuseSurfaceUnknown
	}
}

func loginRateLimitKey(ip string) string {
	key, err := rateLimitKey(RateLimitBucketLogin, ip)
	if err != nil {
		return ""
	}
	return key
}

func rateLimitKey(bucket, ip string) (string, error) {
	if err := validateRateLimitBucket(bucket); err != nil {
		return "", err
	}
	ip = normalizeClientIP(ip)
	if ip == "" {
		return "", fmt.Errorf("missing client key")
	}
	return fmt.Sprintf("remotex:ratelimit:%s:%s", bucket, ip), nil
}

func validateRateLimitBucket(bucket string) error {
	if bucket == "" {
		return fmt.Errorf("missing bucket")
	}
	for _, r := range bucket {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' {
			continue
		}
		return fmt.Errorf("invalid bucket %q", bucket)
	}
	return nil
}

func rateLimitClientIP(r *http.Request) string {
	if r == nil {
		return ""
	}

	return normalizeClientIP(r.RemoteAddr)
}

func normalizeClientIP(raw string) string {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return ""
	}

	if strings.Contains(trimmed, ",") {
		trimmed = strings.TrimSpace(strings.Split(trimmed, ",")[0])
	}

	if host, _, err := net.SplitHostPort(trimmed); err == nil {
		return strings.TrimSpace(host)
	}

	if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
		return strings.Trim(trimmed, "[]")
	}

	return trimmed
}
