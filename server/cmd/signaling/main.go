package main

import (
	"context"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/go-chi/chi/v5"
	chimiddleware "github.com/go-chi/chi/v5/middleware"
	"github.com/gorilla/websocket"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	"github.com/azvlucas/remotex-portfolio/server/internal/relay"
	"github.com/azvlucas/remotex-portfolio/server/internal/signaling"
	redispkg "github.com/azvlucas/remotex-portfolio/server/pkg/redis"
)

func main() {
	const (
		startupTimeout      = 15 * time.Second
		healthProbeTimeout  = 1 * time.Second
		observabilityTarget = "signaling"
	)

	// ── Logger ──────────────────────────────────────────────────────────
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	})))

	metrics, err := observability.NewMetrics(observabilityTarget)
	if err != nil {
		slog.Error("failed to initialize observability registry", "err", err)
		os.Exit(1)
	}
	statusRecorder := observability.NewRecorder()

	// ── Config (env vars com defaults) ──────────────────────────────────
	port := envOr("SIGNAL_PORT", "8080")
	dbURL := requireEnv("DATABASE_URL")
	jwtSecret := requireEnv("JWT_SECRET")
	redisAddr := envOr("REDIS_ADDR", "") // vazio = modo single-node
	allowedOrigins := envOr("ALLOWED_ORIGINS", "")
	relayAuthSecret := envOr("RELAY_AUTH_SECRET", jwtSecret)

	// ── Auth service ────────────────────────────────────────────────────
	authSvc := auth.NewService(auth.DefaultConfig(jwtSecret))
	poolCfg, err := pgxpool.ParseConfig(dbURL)
	if err != nil {
		slog.Error("invalid database url", "err", err)
		os.Exit(1)
	}
	poolCfg.ConnConfig.ConnectTimeout = 10 * time.Second

	db, err := pgxpool.NewWithConfig(context.Background(), poolCfg)
	if err != nil {
		slog.Error("failed to connect to database", "err", err)
		os.Exit(1)
	}
	defer db.Close()
	dbPingCtx, dbPingCancel := context.WithTimeout(context.Background(), startupTimeout)
	defer dbPingCancel()
	if err := db.Ping(dbPingCtx); err != nil {
		slog.Error("database ping failed", "err", err)
		os.Exit(1)
	}
	userStore := auth.NewPostgresUserStore(db)
	authorizer := signaling.NewAuthorizer(db)

	// ── Redis (opcional) ────────────────────────────────────────────────
	var (
		redisBus        *redispkg.PubSub
		redisStartupErr error
	)
	if redisAddr != "" {
		redisBus, err = redispkg.New(redispkg.Config{
			Addr:     redisAddr,
			Password: envOr("REDIS_PASSWORD", ""),
		})
		if err != nil {
			redisStartupErr = err
			slog.Error("failed to connect to redis, running in single-node mode", "err", err)
		} else {
			defer redisBus.Close()
		}
	} else {
		slog.Info("redis not configured, running in single-node mode")
	}

	nodeID := envOr("NODE_ID", "signaling-unknown")

	// ── Hub ─────────────────────────────────────────────────────────────
	hub := signaling.NewHub(redisBus, authorizer, nodeID)
	observability.SetSignalingActiveConnections(hub.ActiveClientCount())
	go hub.Run()

	// ── Video relay (TCP) ───────────────────────────────────────────────
	relayPort := envOr("RELAY_PORT", "9100")
	videoRelay := relay.New(redisBus, nodeID, relayPort, relayAuthSecret)
	go func() {
		if err := videoRelay.ListenAndServe(":" + relayPort); err != nil {
			slog.Error("relay failed to start", "err", err)
		}
	}()

	healthHandler := observability.NewHealthHandler(observabilityTarget, statusRecorder, map[string]observability.ComponentCheck{
		"database":  signalingDatabaseComponentCheck(db),
		"redis":     signalingRedisComponentCheck(redisAddr, redisBus, redisStartupErr),
		"relay":     relayComponentCheck(videoRelay),
		"websocket": websocketComponentCheck(hub),
	}, healthProbeTimeout)

	// ── HTTP / WebSocket ────────────────────────────────────────────────
	upgrader := websocket.Upgrader{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
		CheckOrigin:     checkOrigin(allowedOrigins),
	}

	r := chi.NewRouter()
	r.Use(chimiddleware.RealIP)
	r.Use(chimiddleware.RequestID)
	r.Use(metrics.Middleware)
	r.Use(chimiddleware.Recoverer)

	r.Handle("/metrics", metrics.Handler())
	r.Handle("/health", healthHandler)

	// WebSocket endpoint (com JWT auth).
	r.Group(func(r chi.Router) {
		r.Use(websocketAuthMetricsMiddleware)
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Use(auth.RequireCompanyMiddleware())

		r.Get("/ws", func(w http.ResponseWriter, r *http.Request) {
			claims, ok := auth.UserFromContext(r.Context())
			if !ok || claims == nil || claims.CompanyID == "" || claims.UserID == "" {
				http.Error(w, "unauthorized", http.StatusUnauthorized)
				return
			}

			conn, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				observability.RecordSignalingEvent(observability.SignalingSurfaceWebSocketUpgrade, observability.SignalingReasonUpgradeFailed)
				slog.Error("websocket upgrade failed", "err", err)
				return
			}

			client := signaling.NewClient(hub, conn, claims)
			hub.Register(client)

			slog.Info("websocket connected",
				"client", client.ID(),
				"user_id", claims.UserID,
				"machine_id", claims.MachineID,
				"remote_addr", r.RemoteAddr,
			)
		})
	})

	// ── Server ──────────────────────────────────────────────────────────
	srv := &http.Server{
		Addr:        ":" + port,
		Handler:     r,
		IdleTimeout: 120 * time.Second,
		// ReadTimeout and WriteTimeout are intentionally omitted: they apply
		// to the entire connection lifetime and would kill long-lived
		// WebSocket connections after the configured duration.  The gorilla
		// websocket client already manages per-message deadlines and
		// ping/pong keepalives (see client.go pongWait / pingInterval).
	}

	// Iniciar em goroutine.
	go func() {
		slog.Info("signaling server starting", "port", port)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("server error", "err", err)
			os.Exit(1)
		}
	}()

	// ── Graceful shutdown ───────────────────────────────────────────────
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	sig := <-quit

	slog.Info("shutting down", "signal", sig.String())

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	if err := srv.Shutdown(ctx); err != nil {
		slog.Error("server shutdown error", "err", err)
	}

	slog.Info("server stopped")
}

func websocketAuthMetricsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		wrapped := chimiddleware.NewWrapResponseWriter(w, r.ProtoMajor)
		next.ServeHTTP(wrapped, r)

		switch wrapped.Status() {
		case http.StatusUnauthorized:
			observability.RecordSignalingEvent(observability.SignalingSurfaceWebSocketAuth, observability.SignalingReasonUnauthorized)
		case http.StatusForbidden:
			observability.RecordSignalingEvent(observability.SignalingSurfaceWebSocketAuth, observability.SignalingReasonForbidden)
		}
	})
}

func websocketComponentCheck(hub *signaling.Hub) observability.ComponentCheck {
	return func(context.Context) observability.ComponentStatus {
		checkedAt := time.Now().UTC()
		count := int64(0)
		if hub != nil {
			count = int64(hub.ActiveClientCount())
		}
		return observability.ComponentStatus{
			Status:    observability.StatusHealthy,
			Mode:      "authenticated_clients",
			Detail:    "signaling hub active",
			Count:     &count,
			CheckedAt: &checkedAt,
		}
	}
}

func signalingDatabaseComponentCheck(db *pgxpool.Pool) observability.ComponentCheck {
	return func(ctx context.Context) observability.ComponentStatus {
		checkedAt := time.Now().UTC()
		status := observability.ComponentStatus{
			Status:    observability.StatusHealthy,
			Mode:      "primary",
			Detail:    "database ping ok",
			CheckedAt: &checkedAt,
		}
		if err := db.Ping(ctx); err != nil {
			status.Status = observability.StatusDegraded
			status.Detail = "database ping failed"
			status.LastError = err.Error()
		}
		return status
	}
}

func signalingRedisComponentCheck(redisAddr string, redisBus *redispkg.PubSub, startupErr error) observability.ComponentCheck {
	return func(ctx context.Context) observability.ComponentStatus {
		checkedAt := time.Now().UTC()
		switch {
		case strings.TrimSpace(redisAddr) == "":
			return observability.ComponentStatus{
				Status:    observability.StatusDisabled,
				Mode:      "disabled",
				Detail:    "redis not configured",
				CheckedAt: &checkedAt,
			}
		case redisBus == nil:
			status := observability.ComponentStatus{
				Status:    observability.StatusDegraded,
				Mode:      "single_node",
				Detail:    "redis unavailable; cross-node signaling disabled",
				CheckedAt: &checkedAt,
			}
			if startupErr != nil {
				status.LastError = startupErr.Error()
			}
			return status
		default:
			status := observability.ComponentStatus{
				Status:    observability.StatusHealthy,
				Mode:      "redis_primary",
				Detail:    "redis ping ok",
				CheckedAt: &checkedAt,
			}
			if err := boundedRedisPing(ctx, redisBus); err != nil {
				status.Status = observability.StatusDegraded
				status.Mode = "single_node"
				status.Detail = "redis ping failed; cross-node signaling disabled"
				status.LastError = err.Error()
			}
			return status
		}
	}
}

func relayComponentCheck(videoRelay *relay.Relay) observability.ComponentCheck {
	return func(context.Context) observability.ComponentStatus {
		if videoRelay == nil {
			checkedAt := time.Now().UTC()
			return observability.ComponentStatus{
				Status:    observability.StatusUnknown,
				Mode:      "unknown",
				Detail:    "relay not configured",
				CheckedAt: &checkedAt,
			}
		}
		return videoRelay.HealthStatus()
	}
}

func boundedRedisPing(ctx context.Context, redisBus *redispkg.PubSub) error {
	if redisBus == nil {
		return context.Canceled
	}

	result := make(chan error, 1)
	go func() {
		result <- redisBus.Ping()
	}()

	select {
	case err := <-result:
		return err
	case <-ctx.Done():
		return ctx.Err()
	}
}

func checkOrigin(allowedOrigins string) func(r *http.Request) bool {
	if allowedOrigins == "*" {
		return func(r *http.Request) bool { return true }
	}
	if allowedOrigins == "" {
		return func(r *http.Request) bool {
			return r.Header.Get("Origin") == ""
		}
	}

	origins := strings.Split(allowedOrigins, ",")
	validOrigins := make(map[string]bool)
	for _, o := range origins {
		validOrigins[strings.TrimSpace(o)] = true
	}

	return func(r *http.Request) bool {
		origin := r.Header.Get("Origin")
		if origin == "" {
			return true // Allow non-browser clients (like native apps)
		}
		return validOrigins[origin]
	}
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func requireEnv(key string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	slog.Error("missing required environment variable", "key", key)
	os.Exit(1)
	return ""
}
