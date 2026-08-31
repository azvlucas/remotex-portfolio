package main

import (
	"context"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/redis/go-redis/v9"

	"github.com/azvlucas/remotex-portfolio/server/internal/admin"
	"github.com/azvlucas/remotex-portfolio/server/internal/audit"
	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/backup"
	"github.com/azvlucas/remotex-portfolio/server/internal/bootstrap"
	"github.com/azvlucas/remotex-portfolio/server/internal/installer"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinegroups"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
	"github.com/azvlucas/remotex-portfolio/server/internal/machines"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	"github.com/azvlucas/remotex-portfolio/server/internal/recordings"
	"github.com/azvlucas/remotex-portfolio/server/internal/relay"
	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
	"github.com/azvlucas/remotex-portfolio/server/internal/reports"
	"github.com/azvlucas/remotex-portfolio/server/internal/sessions"
	"github.com/azvlucas/remotex-portfolio/server/internal/turn"
	"github.com/azvlucas/remotex-portfolio/server/internal/update"
	"github.com/azvlucas/remotex-portfolio/server/internal/users"
)

func main() {
	const (
		startupTimeout      = 15 * time.Second
		healthProbeTimeout  = 1 * time.Second
		observabilityTarget = "api"
	)

	// ── Logger ──────────────────────────────────────────────────────────
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	})))

	// ── Config ──────────────────────────────────────────────────────────
	port := envOr("API_PORT", "8081")
	dbURL := requireEnv("DATABASE_URL")
	redisAddr := envOr("REDIS_ADDR", "")
	jwtSecret := requireEnv("JWT_SECRET")
	r2AccountID := envOr("R2_ACCOUNT_ID", "")
	r2AccessKey := envOr("R2_ACCESS_KEY_ID", "")
	r2SecretKey := envOr("R2_SECRET_ACCESS_KEY", "")
	r2Bucket := envOr("R2_BUCKET_NAME", "remotex-backups")
	turnSecret := requireEnv("TURN_SECRET")
	turnServers := envOr("TURN_SERVERS", "localhost:3478")
	relayAuthSecret := envOr("RELAY_AUTH_SECRET", jwtSecret)
	relayPublicHost := envOr("RELAY_PUBLIC_HOST", "")
	relayPublicPort := envOr("RELAY_PUBLIC_PORT", "9100")
	relayTicketTTLSeconds := envOr("RELAY_TICKET_TTL_SEC", "900")
	adminHostRaw, adminHostConfigured := os.LookupEnv("ADMIN_HOST")
	adminHost := admin.NormalizeConfiguredAdminHost(adminHostRaw)
	if adminHostConfigured && adminHost == "" {
		slog.Warn("admin host guard configured with invalid host; admin routes will fail closed")
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	metrics, err := observability.NewMetrics(observabilityTarget)
	if err != nil {
		slog.Error("failed to initialize observability registry", "err", err)
		os.Exit(1)
	}
	statusRecorder := observability.NewRecorder()

	// ── PostgreSQL connection pool ──────────────────────────────────────
	poolCfg, err := pgxpool.ParseConfig(dbURL)
	if err != nil {
		slog.Error("invalid database url", "err", err)
		os.Exit(1)
	}
	poolCfg.ConnConfig.ConnectTimeout = 10 * time.Second
	poolCfg.MaxConns = 20
	poolCfg.MinConns = 2

	db, err := pgxpool.NewWithConfig(ctx, poolCfg)
	if err != nil {
		slog.Error("failed to connect to database", "err", err)
		os.Exit(1)
	}
	defer db.Close()

	dbPingCtx, dbPingCancel := context.WithTimeout(ctx, startupTimeout)
	defer dbPingCancel()
	if err := db.Ping(dbPingCtx); err != nil {
		slog.Error("database ping failed", "err", err)
		os.Exit(1)
	}
	slog.Info("database connected", "url", redactDatabaseURL(dbURL))

	// ── DBMigration ─────────────────────────────────────────────────────
	authSvc := auth.NewService(auth.DefaultConfig(jwtSecret))
	userStore := auth.NewPostgresUserStore(db)

	migrateCtx, migrateCancel := context.WithTimeout(ctx, startupTimeout)
	defer migrateCancel()
	if err := bootstrap.Migrate(migrateCtx, db); err != nil {
		slog.Error("database bootstrap migration failed", "err", err)
		os.Exit(1)
	}
	superAdminCtx, superAdminCancel := context.WithTimeout(ctx, startupTimeout)
	defer superAdminCancel()
	if err := bootstrap.EnsureSuperAdmin(superAdminCtx, db, userStore, authSvc, bootstrap.SuperAdminConfigFromEnv()); err != nil {
		slog.Error("super admin bootstrap failed", "err", err)
		os.Exit(1)
	}

	// ── Redis (opcional, para rate limiting) ────────────────────────────
	var rdb *redis.Client
	if redisAddr != "" {
		rdb = redis.NewClient(&redis.Options{
			Addr:     redisAddr,
			Password: envOr("REDIS_PASSWORD", ""),
		})
		redisPingCtx, redisPingCancel := context.WithTimeout(ctx, 5*time.Second)
		defer redisPingCancel()
		if err := rdb.Ping(redisPingCtx).Err(); err != nil {
			slog.Warn("redis not available; auth rate limiting will use in-memory fallback only", "err", err)
			rdb = nil
		} else {
			defer rdb.Close()
			slog.Info("redis connected", "addr", redisAddr)
		}
	}

	// ── Services ────────────────────────────────────────────────────────
	loginRateLimitCfg := auth.RateLimiterConfig{
		MaxAttempts: 5,
		Window:      15 * time.Minute,
	}
	loginFallbackStore := auth.NewInMemoryRateLimitStore(auth.InMemoryRateLimitStoreConfig{MaxEntries: 2048})
	loginRateLimitStore := auth.NewFallbackRateLimitStore(auth.NewRedisRateLimitStore(rdb), loginFallbackStore)
	if rdb == nil {
		slog.Info("public login rate limiting configured with in-memory fallback only", "bucket", auth.RateLimitBucketLogin)
	} else {
		slog.Info("public login rate limiting configured with redis primary and in-memory fallback", "bucket", auth.RateLimitBucketLogin)
	}
	rateLimiter := auth.NewBucketRateLimiter(loginRateLimitStore, loginRateLimitCfg, auth.RateLimitBucketLogin)

	adminFallbackStore := auth.NewInMemoryRateLimitStore(auth.InMemoryRateLimitStoreConfig{MaxEntries: 2048})
	adminRateLimitStore := auth.NewFallbackRateLimitStore(auth.NewRedisRateLimitStore(rdb), adminFallbackStore)
	if rdb == nil {
		slog.Info("admin auth rate limiting configured with in-memory fallback only", "buckets", []string{auth.RateLimitBucketAdminPassword, auth.RateLimitBucketAdminTOTP})
	} else {
		slog.Info("admin auth rate limiting configured with redis primary and in-memory fallback", "buckets", []string{auth.RateLimitBucketAdminPassword, auth.RateLimitBucketAdminTOTP})
	}
	adminPasswordLimiter := auth.NewBucketRateLimiter(adminRateLimitStore, loginRateLimitCfg, auth.RateLimitBucketAdminPassword)
	adminTOTPLimiter := auth.NewBucketRateLimiter(adminRateLimitStore, loginRateLimitCfg, auth.RateLimitBucketAdminTOTP)

	machineRepo := machines.NewRepository(db)
	machineLimitRepo := machinelimit.NewRepository(db)
	machineLimitSvc := machinelimit.NewService(machineLimitRepo)
	machineSvc := machines.NewService(machineRepo, machineLimitSvc)
	auditSvc := audit.NewService(db)
	machineHandler := machines.NewHandler(machineSvc, auditSvc)
	machineGroupRepo := machinegroups.NewRepository(db)
	machineGroupSvc := machinegroups.NewService(machineGroupRepo)
	machineGroupHandler := machinegroups.NewHandler(machineGroupSvc)

	sessionSvc := sessions.NewService(db, auditSvc, machineLimitSvc)
	relayPortValue, err := strconv.Atoi(relayPublicPort)
	if err != nil || relayPortValue <= 0 || relayPortValue > 65535 {
		relayPortValue = 9100
	}
	relayTTLValue, err := strconv.Atoi(relayTicketTTLSeconds)
	if err != nil || relayTTLValue <= 0 {
		relayTTLValue = 900
	}
	relayIssuer := relay.NewTicketIssuer(
		relayAuthSecret,
		relayPublicHost,
		relayPortValue,
		time.Duration(relayTTLValue)*time.Second,
	)
	sessionHandler := sessions.NewHandler(sessionSvc, relayIssuer)

	authHandler := auth.NewHandler(authSvc, db, rateLimiter)

	backupRepo := backup.NewRepository(db)
	backupSvc := backup.NewService(backupRepo, backup.R2Config{
		AccountID:       r2AccountID,
		AccessKeyID:     r2AccessKey,
		SecretAccessKey: r2SecretKey,
		BucketName:      r2Bucket,
	})
	backupHandler := backup.NewHandler(backupSvc)

	recordingRepo := recordings.NewRepository(db)
	recordingSvc := recordings.NewService(recordingRepo, backup.R2Config{
		AccountID:       r2AccountID,
		AccessKeyID:     r2AccessKey,
		SecretAccessKey: r2SecretKey,
		BucketName:      r2Bucket,
	})
	recordingHandler := recordings.NewHandler(recordingSvc)

	turnHandler := turn.NewHandler(turn.Config{
		Secret:  turnSecret,
		Servers: strings.Split(turnServers, ","),
	})

	releaseConfig := releases.ConfigFromEnv()
	releaseSvc := releases.NewService(releaseConfig)
	viewerReleaseSvc := releases.NewService(releases.Config{
		RootPath:         envOr("VIEWER_RELEASES_ROOT_PATH", "/data/releases/viewer"),
		FallbackRootPath: envOr("VIEWER_RELEASES_FALLBACK_ROOT", "/data/viewer"),
		PublicKeyPath:    envOr("RELEASES_PUBLIC_KEY_PATH", releaseConfig.PublicKeyPath),
	})
	updaterReleaseSvc := releases.NewService(releases.Config{
		RootPath:         envOr("UPDATER_RELEASES_ROOT_PATH", "/data/releases/updater"),
		FallbackRootPath: envOr("UPDATER_RELEASES_FALLBACK_ROOT", "/data/updater"),
		PublicKeyPath:    envOr("RELEASES_PUBLIC_KEY_PATH", releaseConfig.PublicKeyPath),
	})
	updateCfg := update.ConfigFromEnv()
	if release, err := releaseSvc.ResolveCurrent(); err != nil {
		slog.Warn("update release not ready", "err", err)
	} else {
		slog.Info("update release ready", "version", release.Version, "download_url", updateCfg.DownloadURL, "source", release.Source)
	}
	if release, err := viewerReleaseSvc.ResolveCurrent(); err != nil {
		slog.Warn("viewer release not ready", "err", err)
	} else {
		slog.Info("viewer release ready", "version", release.Version, "source", release.Source)
	}
	if release, err := updaterReleaseSvc.ResolveCurrent(); err != nil {
		slog.Warn("updater release not ready", "err", err)
	} else {
		slog.Info("updater release ready", "version", release.Version, "source", release.Source)
	}
	updateHandler := update.NewHandler(updateCfg, releaseSvc, viewerReleaseSvc, updaterReleaseSvc)

	installerSvc := installer.NewService(installer.ConfigFromEnv(), releaseSvc)
	installerHandler := installer.NewHandler(installerSvc, authSvc, machineSvc)
	usersSvc := users.NewService(db, authSvc)
	usersHandler := users.NewHandler(usersSvc)
	reportsRepo := reports.NewRepository(db)
	reportsSvc := reports.NewService(reportsRepo)
	reportsHandler := reports.NewHandler(reportsSvc)
	adminSvc := admin.NewService(db, authSvc, machineLimitSvc)

	statusRecorder.RegisterMonitor("heartbeat", 30*time.Second)
	statusRecorder.RegisterMonitor("ghost_session", 60*time.Second)
	statusRecorder.RegisterMonitor("auto_block", time.Hour)

	initialMonitorCtx, initialMonitorCancel := context.WithTimeout(ctx, startupTimeout)
	runHeartbeatMonitorPass(initialMonitorCtx, machineSvc, statusRecorder)
	runGhostSessionMonitorPass(initialMonitorCtx, sessionSvc, statusRecorder)
	runAutoBlockMonitorPass(initialMonitorCtx, adminSvc, statusRecorder)
	initialMonitorCancel()

	healthChecks := map[string]observability.ComponentCheck{
		"database": databaseComponentCheck(db),
		"redis":    redisComponentCheck(redisAddr, rdb),
	}
	healthHandler := observability.NewHealthHandler(observabilityTarget, statusRecorder, healthChecks, healthProbeTimeout)
	adminHandler := admin.NewHandler(adminSvc, authSvc, userStore, releaseSvc, viewerReleaseSvc, updaterReleaseSvc, auditSvc, adminHost, adminHostConfigured, adminPasswordLimiter, adminTOTPLimiter, func(ctx context.Context) observability.HealthSnapshot {
		return observability.BuildHealthSnapshot(ctx, observabilityTarget, statusRecorder, healthChecks, healthProbeTimeout, time.Now())
	})

	// ── Router ──────────────────────────────────────────────────────────
	r := chi.NewRouter()
	r.Use(middleware.RealIP)
	r.Use(middleware.RequestID)
	r.Use(metrics.Middleware)
	r.Use(middleware.Recoverer)

	r.Handle("/metrics", metrics.Handler())
	r.Handle("/health", healthHandler)

	// Auth (sem JWT middleware).
	r.Mount("/auth", authHandler.Routes())
	r.Mount("/admin", adminHandler.Routes())
	r.Route("/host", func(r chi.Router) {
		r.Post("/enroll", installerHandler.Enroll)

		r.Group(func(r chi.Router) {
			r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
			r.Use(auth.RequireHumanCompanyMiddleware())
			r.Post("/installer", installerHandler.GenerateInstaller)
		})
	})

	// Rotas protegidas por JWT para usuários humanos.
	r.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
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
			r.Mount("/machine-groups", machineGroupHandler.Routes())
			r.Mount("/machine-subgroups", machineGroupHandler.SubgroupRoutes())
			r.Mount("/sessions", sessionHandler.Routes())
		})
	})

	// Rotas de gerenciamento de usuários (company_manager ou super_admin).
	r.Route("/users/company", func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Use(auth.RequireCompanyManagerMiddleware())
		r.Mount("/", usersHandler.Routes())
	})

	// Rotas de relatórios (company_manager ou super_admin).
	r.Route("/reports", func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Use(auth.RequireCompanyManagerMiddleware())
		r.Mount("/", reportsHandler.Routes())
	})

	// Rotas protegidas por JWT para usuários e agentes de máquina.
	r.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, userStore))
		r.Use(auth.RequireCompanyMiddleware())
		r.Mount("/backup", backupHandler.Routes())
		r.Mount("/recordings", recordingHandler.Routes())
		r.Mount("/turn", turnHandler.Routes())
		r.Mount("/update", updateHandler.Routes())
	})

	// ── Background: heartbeat monitor ───────────────────────────────────
	go heartbeatMonitor(ctx, machineSvc, statusRecorder)

	// ── Background: ghost session monitor ───────────────────────────────
	go ghostSessionMonitor(ctx, sessionSvc, statusRecorder)

	// ── Background: auto-block expired companies ────────────────────────
	go autoBlockMonitor(ctx, adminSvc, statusRecorder)

	// ── HTTP Server ─────────────────────────────────────────────────────
	srv := &http.Server{
		Addr:         ":" + port,
		Handler:      r,
		ReadTimeout:  15 * time.Second,
		WriteTimeout: 15 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	go func() {
		slog.Info("api server starting", "port", port)
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
	cancel()

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		slog.Error("server shutdown error", "err", err)
	}
	slog.Info("api server stopped")
}

// heartbeatMonitor roda em background e marca máquinas sem heartbeat como offline.
func heartbeatMonitor(ctx context.Context, svc *machines.Service, recorder *observability.Recorder) {
	const (
		checkInterval = 30 * time.Second
		staleTimeout  = 90 * time.Second
	)

	ticker := time.NewTicker(checkInterval)
	defer ticker.Stop()

	slog.Info("heartbeat monitor started", "interval", checkInterval, "timeout", staleTimeout)

	for {
		select {
		case <-ctx.Done():
			slog.Info("heartbeat monitor stopped")
			return
		case <-ticker.C:
			runHeartbeatMonitorPass(ctx, svc, recorder)
		}
	}
}

func runHeartbeatMonitorPass(ctx context.Context, svc *machines.Service, recorder *observability.Recorder) {
	const staleTimeout = 90 * time.Second

	count, err := svc.MarkStaleOffline(ctx, staleTimeout)
	recorder.RecordMonitorResult("heartbeat", int(count), err)
	if err != nil {
		slog.Warn("heartbeat monitor error", "err", err)
		return
	}
	if count > 0 {
		slog.Info("machines marked offline", "count", count)
	}
}

// ghostSessionMonitor closes sessions whose machine went offline (crash, network loss, etc)
// or whose viewer has not sent a heartbeat within the viewer timeout.
func ghostSessionMonitor(ctx context.Context, svc *sessions.Service, recorder *observability.Recorder) {
	const (
		checkInterval = 60 * time.Second
		viewerTimeout = 2 * time.Minute
	)

	ticker := time.NewTicker(checkInterval)
	defer ticker.Stop()

	slog.Info("ghost session monitor started", "interval", checkInterval, "viewer_timeout", viewerTimeout)

	for {
		select {
		case <-ctx.Done():
			slog.Info("ghost session monitor stopped")
			return
		case <-ticker.C:
			runGhostSessionMonitorPass(ctx, svc, recorder)
		}
	}
}

func runGhostSessionMonitorPass(ctx context.Context, svc *sessions.Service, recorder *observability.Recorder) {
	const viewerTimeout = 2 * time.Minute

	count, err := svc.CloseOrphanSessions(ctx, viewerTimeout)
	recorder.RecordMonitorResult("ghost_session", count, err)
	if err != nil {
		slog.Warn("ghost session monitor error", "err", err)
		return
	}
	if count > 0 {
		slog.Info("ghost sessions closed", "count", count)
	}
}

func autoBlockMonitor(ctx context.Context, svc *admin.Service, recorder *observability.Recorder) {
	const checkInterval = 1 * time.Hour

	ticker := time.NewTicker(checkInterval)
	defer ticker.Stop()

	slog.Info("auto-block monitor started", "interval", checkInterval)

	for {
		select {
		case <-ctx.Done():
			slog.Info("auto-block monitor stopped")
			return
		case <-ticker.C:
			runAutoBlockMonitorPass(ctx, svc, recorder)
		}
	}
}

func runAutoBlockMonitorPass(ctx context.Context, svc *admin.Service, recorder *observability.Recorder) {
	count, err := svc.AutoBlockExpiredCompanies(ctx)
	recorder.RecordMonitorResult("auto_block", count, err)
	if err != nil {
		slog.Warn("auto-block monitor error", "err", err)
		return
	}
	if count > 0 {
		slog.Info("companies auto-blocked", "count", count)
	}
}

func databaseComponentCheck(db *pgxpool.Pool) observability.ComponentCheck {
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

func redisComponentCheck(redisAddr string, rdb *redis.Client) observability.ComponentCheck {
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
		case rdb == nil:
			return observability.ComponentStatus{
				Status:    observability.StatusDegraded,
				Mode:      "fallback_only",
				Detail:    "redis unavailable; using in-memory fallback",
				CheckedAt: &checkedAt,
			}
		default:
			status := observability.ComponentStatus{
				Status:    observability.StatusHealthy,
				Mode:      "redis_primary",
				Detail:    "redis ping ok",
				CheckedAt: &checkedAt,
			}
			if err := rdb.Ping(ctx).Err(); err != nil {
				status.Status = observability.StatusDegraded
				status.Mode = "fallback_only"
				status.Detail = "redis ping failed; using in-memory fallback"
				status.LastError = err.Error()
			}
			return status
		}
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

func redactDatabaseURL(raw string) string {
	parsed, err := url.Parse(raw)
	if err != nil {
		return "<invalid>"
	}
	return parsed.Redacted()
}
