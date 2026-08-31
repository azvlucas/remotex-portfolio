package admin

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/audit"
	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
)

const adminCookieName = "remotex_admin_token"
const adminChallengeCookieName = "remotex_admin_challenge"
const csrfCookieName = "remotex_csrf_token"
const releaseUploadMaxBytes int64 = 64 << 20
const adminChallengeTTL = 5 * time.Minute

type adminService interface {
	ListCompanies(ctx context.Context) ([]CompanySummary, error)
	GetDashboardStats(ctx context.Context) (DashboardStats, error)
	GetCompanyDetail(ctx context.Context, companyID string) (*CompanyDetail, error)
	SearchMachines(ctx context.Context, query string, limit int) ([]MachineSummary, error)
	DetachMachine(ctx context.Context, machineID string) error
	UpdateCompanyBlockDays(ctx context.Context, companyID string, days *int) error
	UpdateConcurrentSessionLimit(ctx context.Context, companyID string, limit *int) error
	UpdateMachineLimit(ctx context.Context, companyID string, limit *int) error
	CreateCompany(ctx context.Context, name string) (*CompanySummary, error)
	CreateCompanyUserWithRole(ctx context.Context, companyID, name, email, password string, role auth.Role) (*CompanyUser, error)
	SetUserActive(ctx context.Context, userID string, active bool) error
	UpdateUserRole(ctx context.Context, userID string, role auth.Role) error
	SetCompanyActive(ctx context.Context, companyID string, active bool) error
}

type Handler struct {
	svc                 adminService
	authSvc             *auth.Service
	users               auth.UserStore
	releases            *releases.Service
	viewerReleases      *releases.Service
	updaterReleases     *releases.Service
	audit               *audit.Service
	healthSnapshot      func(context.Context) observability.HealthSnapshot
	adminHost           string
	adminHostConfigured bool
	passwordLimiter     *auth.RateLimiter
	totpLimiter         *auth.RateLimiter
}

func NewHandler(svc *Service, authSvc *auth.Service, users auth.UserStore, releaseSvc *releases.Service, viewerReleaseSvc *releases.Service, updaterReleaseSvc *releases.Service, auditSvc *audit.Service, adminHost string, adminHostConfigured bool, passwordLimiter, totpLimiter *auth.RateLimiter, healthSnapshot func(context.Context) observability.HealthSnapshot) *Handler {
	return &Handler{
		svc:                 svc,
		authSvc:             authSvc,
		users:               users,
		releases:            releaseSvc,
		viewerReleases:      viewerReleaseSvc,
		updaterReleases:     updaterReleaseSvc,
		audit:               auditSvc,
		healthSnapshot:      healthSnapshot,
		adminHost:           adminHost,
		adminHostConfigured: adminHostConfigured,
		passwordLimiter:     passwordLimiter,
		totpLimiter:         totpLimiter,
	}
}

func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Use(h.requireCanonicalHost)
	r.Use(h.requireSecureTransport)
	r.Use(h.csrfProtect)
	r.Get("/login", h.LoginPage)
	r.Post("/login", h.Login)
	r.Get("/setup", h.SetupPage)
	r.Post("/setup", h.Setup)
	r.Get("/verify", h.VerifyPage)
	r.Post("/verify", h.Verify)

	r.Group(func(r chi.Router) {
		r.Use(h.requireAdmin)
		r.Get("/", h.Dashboard)
		r.Get("/companies", h.Dashboard)
		r.Get("/machines", h.MachinesPage)
		r.Get("/releases", h.ReleasesPage)
		r.Post("/releases/upload", h.UploadRelease)
		r.Post("/releases/{version}/activate", h.ActivateRelease)
		r.Post("/releases/{version}/delete", h.DeleteRelease)
		r.Get("/releases/viewer", h.ViewerReleasesPage)
		r.Post("/releases/viewer/upload", h.UploadViewerRelease)
		r.Post("/releases/viewer/{version}/activate", h.ActivateViewerRelease)
		r.Post("/releases/viewer/{version}/delete", h.DeleteViewerRelease)
		r.Get("/releases/updater", h.UpdaterReleasesPage)
		r.Post("/releases/updater/upload", h.UploadUpdaterRelease)
		r.Post("/releases/updater/{version}/activate", h.ActivateUpdaterRelease)
		r.Post("/releases/updater/{version}/delete", h.DeleteUpdaterRelease)
		r.Post("/companies", h.CreateCompany)
		r.Get("/companies/{id}", h.CompanyDetail)
		r.Get("/companies/{id}/logs", h.CompanyLogs)
		r.Post("/machines/{id}/detach", h.DetachMachine)
		r.Post("/companies/{id}/activate", h.ActivateCompany)
		r.Post("/companies/{id}/deactivate", h.DeactivateCompany)
		r.Post("/companies/{id}/block-days", h.UpdateBlockDays)
		r.Post("/companies/{id}/concurrent-limit", h.UpdateConcurrentLimit)
		r.Post("/companies/{id}/machine-limit", h.UpdateMachineLimit)
		r.Post("/companies/{id}/users", h.CreateCompanyUser)
		r.Post("/users/{id}/activate", h.ActivateUser)
		r.Post("/users/{id}/deactivate", h.DeactivateUser)
		r.Post("/users/{id}/role", h.UpdateUserRole)
		r.Post("/logout", h.Logout)
	})

	return r
}

type loginPageData struct {
	Error     string
	CSRFToken string
}

type adminSetupPageData struct {
	Error      string
	CSRFToken  string
	Email      string
	Secret     string
	OTPAuthURL string
}

type adminVerifyPageData struct {
	Error     string
	CSRFToken string
	Email     string
}

type dashboardPageData struct {
	Error     string
	Message   string
	Stats     DashboardStats
	Health    *dashboardHealthView
	Companies []CompanySummary
	CSRFToken string
}

type companyPageData struct {
	Error     string
	Message   string
	Detail    CompanyDetail
	CSRFToken string
}

type machinesPageData struct {
	Error     string
	Message   string
	Query     string
	Machines  []MachineSummary
	CSRFToken string
}

type releasesPageData struct {
	Error     string
	Message   string
	Current   *releases.ResolvedRelease
	Releases  []releases.PublishedRelease
	CSRFToken string
}

type companyLogsPageData struct {
	Error      string
	Message    string
	Company    CompanySummary
	Events     []audit.Event
	Filter     audit.CompanyEventsFilter
	Total      int
	Page       int
	PageSize   int
	TotalPages int
	PrevPage   int
	NextPage   int
	CSRFToken  string
}

type dashboardHealthView struct {
	Service     string
	Status      string
	StatusClass string
	CheckedAt   string
	Components  []dashboardHealthComponentView
	Monitors    []dashboardHealthMonitorView
}

type dashboardHealthComponentView struct {
	Name        string
	Status      string
	StatusClass string
	Mode        string
	Detail      string
	LastError   string
	CheckedAt   string
}

type dashboardHealthMonitorView struct {
	Name          string
	Status        string
	StatusClass   string
	IntervalLabel string
	LastRunAt     string
	LastSuccessAt string
	LastFailureAt string
	LastCount     string
	LastError     string
}

type adminLoginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

type adminTOTPCodeRequest struct {
	Code string `json:"code"`
}

type adminLoginResponse struct {
	Status     string `json:"status"`
	TOTPSecret string `json:"totp_secret,omitempty"`
	OTPAuthURL string `json:"otpauth_url,omitempty"`
}

type adminTOTPState string

const (
	adminTOTPStateSetupRequired adminTOTPState = "setup_required"
	adminTOTPStateMFARequired   adminTOTPState = "mfa_required"
)

var errInconsistentAdminTOTPState = errors.New("inconsistent admin totp state")

func classifyAdminTOTPState(user *auth.UserRecord) (adminTOTPState, error) {
	if user == nil {
		return "", errInconsistentAdminTOTPState
	}
	secret := ""
	if user.AdminTOTPSecret != nil {
		secret = strings.TrimSpace(*user.AdminTOTPSecret)
	}

	switch {
	case !user.AdminTOTPEnabled && secret == "":
		return adminTOTPStateSetupRequired, nil
	case user.AdminTOTPEnabled && secret != "":
		return adminTOTPStateMFARequired, nil
	default:
		return "", errInconsistentAdminTOTPState
	}
}

func buildDashboardHealthView(snapshot observability.HealthSnapshot) *dashboardHealthView {
	view := &dashboardHealthView{
		Service:     snapshot.Service,
		Status:      string(snapshot.Status),
		StatusClass: healthStatusClass(snapshot.Status),
		CheckedAt:   formatDashboardHealthTime(&snapshot.CheckedAt),
	}

	componentNames := make([]string, 0, len(snapshot.Components))
	for name := range snapshot.Components {
		componentNames = append(componentNames, name)
	}
	sort.Strings(componentNames)
	for _, name := range componentNames {
		component := snapshot.Components[name]
		view.Components = append(view.Components, dashboardHealthComponentView{
			Name:        name,
			Status:      string(component.Status),
			StatusClass: healthStatusClass(component.Status),
			Mode:        component.Mode,
			Detail:      component.Detail,
			LastError:   component.LastError,
			CheckedAt:   formatDashboardHealthTime(component.CheckedAt),
		})
	}

	monitorNames := make([]string, 0, len(snapshot.Monitors))
	for name := range snapshot.Monitors {
		monitorNames = append(monitorNames, name)
	}
	sort.Strings(monitorNames)
	for _, name := range monitorNames {
		monitor := snapshot.Monitors[name]
		view.Monitors = append(view.Monitors, dashboardHealthMonitorView{
			Name:          name,
			Status:        string(monitor.Status),
			StatusClass:   healthStatusClass(monitor.Status),
			IntervalLabel: formatDashboardHealthInterval(monitor.IntervalSeconds),
			LastRunAt:     formatDashboardHealthTime(monitor.LastRunAt),
			LastSuccessAt: formatDashboardHealthTime(monitor.LastSuccessAt),
			LastFailureAt: formatDashboardHealthTime(monitor.LastFailureAt),
			LastCount:     formatDashboardHealthCount(monitor.LastCount),
			LastError:     monitor.LastError,
		})
	}

	return view
}

func healthStatusClass(status observability.Status) string {
	switch status {
	case observability.StatusHealthy:
		return "green"
	case observability.StatusDegraded:
		return "orange"
	case observability.StatusStarting:
		return "blue"
	default:
		return "gray"
	}
}

func formatDashboardHealthTime(ts *time.Time) string {
	if ts == nil || ts.IsZero() {
		return "—"
	}
	return ts.UTC().Format("2006-01-02 15:04:05 MST")
}

func formatDashboardHealthInterval(seconds int64) string {
	if seconds <= 0 {
		return "—"
	}
	return (time.Duration(seconds) * time.Second).String()
}

func formatDashboardHealthCount(count *int64) string {
	if count == nil {
		return "—"
	}
	return strconv.FormatInt(*count, 10)
}

func (h *Handler) LoginPage(w http.ResponseWriter, r *http.Request) {
	if _, err := h.currentAdmin(r); err == nil {
		http.Redirect(w, r, "/admin", http.StatusSeeOther)
		return
	}
	if challenge, err := h.currentAdminChallenge(r); err == nil {
		http.Redirect(w, r, h.challengeRedirectTarget(challenge), http.StatusSeeOther)
		return
	}

	renderTemplate(w, http.StatusOK, loginTemplate, loginPageData{
		Error:     r.URL.Query().Get("error"),
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) Login(w http.ResponseWriter, r *http.Request) {
	creds, err := readAdminLoginRequest(r)
	if err != nil {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusBadRequest, "corpo de login invÃ¡lido")
		return
	}
	if creds.Email == "" || creds.Password == "" {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusBadRequest, "email e senha sÃ£o obrigatÃ³rios")
		return
	}
	if !h.allowAdminPasswordAttempt(w, r) {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()

	user, err := h.users.GetUserByEmail(ctx, creds.Email)
	if err != nil || user == nil || !h.authSvc.CheckPassword(creds.Password, user.PasswordHash) {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusUnauthorized, "credenciais invÃ¡lidas")
		return
	}
	if user.Role != auth.RoleSuperAdmin {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusForbidden, "acesso administrativo requerido")
		return
	}
	if !user.IsActive {
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusForbidden, "usuÃ¡rio inativo")
		return
	}

	state, err := classifyAdminTOTPState(user)
	if err != nil {
		slog.Error("admin totp state invalid at password step", "user_id", user.ID)
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusInternalServerError, "configuraÃ§Ã£o administrativa invÃ¡lida")
		return
	}

	subject := auth.TokenSubject{
		UserID: user.ID,
		Email:  user.Email,
		Name:   user.Name,
		Role:   user.Role,
	}

	var challengeToken string
	var response adminLoginResponse

	switch state {
	case adminTOTPStateSetupRequired:
		enrollment, enrollmentErr := h.authSvc.GenerateAdminTOTPEnrollment(user.Email)
		if enrollmentErr != nil {
			slog.Error("admin totp enrollment generation failed", "err", enrollmentErr, "user_id", user.ID)
			h.clearAdminSessionCookie(w, r)
			h.clearAdminChallengeCookie(w, r)
			h.respondLoginError(w, r, http.StatusInternalServerError, "erro interno")
			return
		}
		challengeToken, err = h.authSvc.GenerateAdminChallengeToken(subject, auth.ScopeAdminChallengeSetup, enrollment.Secret, enrollment.OTPAuthURL, adminChallengeTTL)
		if err != nil {
			slog.Error("admin setup challenge token generation failed", "err", err, "user_id", user.ID)
			h.clearAdminSessionCookie(w, r)
			h.clearAdminChallengeCookie(w, r)
			h.respondLoginError(w, r, http.StatusInternalServerError, "erro interno")
			return
		}
		response = adminLoginResponse{Status: string(adminTOTPStateSetupRequired), TOTPSecret: enrollment.Secret, OTPAuthURL: enrollment.OTPAuthURL}
	case adminTOTPStateMFARequired:
		challengeToken, err = h.authSvc.GenerateAdminChallengeToken(subject, auth.ScopeAdminChallengeMFA, "", "", adminChallengeTTL)
		if err != nil {
			slog.Error("admin mfa challenge token generation failed", "err", err, "user_id", user.ID)
			h.clearAdminSessionCookie(w, r)
			h.clearAdminChallengeCookie(w, r)
			h.respondLoginError(w, r, http.StatusInternalServerError, "erro interno")
			return
		}
		response = adminLoginResponse{Status: string(adminTOTPStateMFARequired)}
	default:
		h.clearAdminSessionCookie(w, r)
		h.clearAdminChallengeCookie(w, r)
		h.respondLoginError(w, r, http.StatusInternalServerError, "configuraÃ§Ã£o administrativa invÃ¡lida")
		return
	}

	h.clearAdminSessionCookie(w, r)
	h.setAdminChallengeCookie(w, r, challengeToken)
	h.passwordLimiter.ResetRequest(r)
	slog.Info("admin login advanced to pending challenge", "user_id", user.ID, "phase", response.Status, "bucket", auth.RateLimitBucketAdminPassword)

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, response)
		return
	}

	target := "/admin/verify"
	if response.Status == string(adminTOTPStateSetupRequired) {
		target = "/admin/setup"
	}
	http.Redirect(w, r, target, http.StatusSeeOther)
}

func (h *Handler) SetupPage(w http.ResponseWriter, r *http.Request) {
	if _, err := h.currentAdmin(r); err == nil {
		http.Redirect(w, r, "/admin", http.StatusSeeOther)
		return
	}

	challenge, ok := h.requireChallenge(w, r, auth.ScopeAdminChallengeSetup)
	if !ok {
		return
	}

	h.renderSetupPage(w, r, http.StatusOK, challenge, r.URL.Query().Get("error"))
}

func (h *Handler) Setup(w http.ResponseWriter, r *http.Request) {
	challenge, ok := h.requireChallenge(w, r, auth.ScopeAdminChallengeSetup)
	if !ok {
		return
	}

	code, err := readAdminTOTPCode(r)
	if err != nil {
		h.renderSetupError(w, r, http.StatusBadRequest, challenge, "corpo de verificaÃ§Ã£o invÃ¡lido")
		return
	}
	if !h.allowAdminTOTPAttempt(w, r) {
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()

	user, ok := h.loadChallengeUserForPhase(w, r, ctx, challenge, adminTOTPStateSetupRequired)
	if !ok {
		return
	}

	valid, err := h.authSvc.ValidateTOTPCode(challenge.PendingTOTPSecret, code, time.Now())
	if err != nil {
		if errors.Is(err, auth.ErrTOTPCodeMalformed) {
			h.renderSetupError(w, r, http.StatusBadRequest, challenge, "cÃ³digo TOTP invÃ¡lido")
			return
		}
		slog.Error("admin totp setup validation failed", "err", err, "user_id", user.ID)
		h.renderSetupError(w, r, http.StatusInternalServerError, challenge, "erro interno")
		return
	}
	if !valid {
		slog.Warn("admin totp setup rejected", "user_id", user.ID, "phase", "invalid_code", "bucket", auth.RateLimitBucketAdminTOTP)
		h.renderSetupError(w, r, http.StatusUnauthorized, challenge, "cÃ³digo TOTP invÃ¡lido")
		return
	}

	if err := h.users.EnableAdminTOTP(ctx, user.ID, challenge.PendingTOTPSecret, time.Now()); err != nil {
		slog.Error("admin totp setup persist failed", "err", err, "user_id", user.ID)
		h.renderSetupError(w, r, http.StatusInternalServerError, challenge, "erro interno")
		return
	}

	h.totpLimiter.ResetRequest(r)
	slog.Info("admin totp setup completed", "user_id", user.ID, "phase", string(adminTOTPStateSetupRequired), "bucket", auth.RateLimitBucketAdminTOTP)
	h.completeAdminLogin(w, r, ctx, user)
}

func (h *Handler) VerifyPage(w http.ResponseWriter, r *http.Request) {
	if _, err := h.currentAdmin(r); err == nil {
		http.Redirect(w, r, "/admin", http.StatusSeeOther)
		return
	}

	challenge, ok := h.requireChallenge(w, r, auth.ScopeAdminChallengeMFA)
	if !ok {
		return
	}

	h.renderVerifyPage(w, r, http.StatusOK, challenge, r.URL.Query().Get("error"))
}

func (h *Handler) Verify(w http.ResponseWriter, r *http.Request) {
	challenge, ok := h.requireChallenge(w, r, auth.ScopeAdminChallengeMFA)
	if !ok {
		return
	}

	code, err := readAdminTOTPCode(r)
	if err != nil {
		h.renderVerifyError(w, r, http.StatusBadRequest, challenge, "corpo de verificaÃ§Ã£o invÃ¡lido")
		return
	}
	if !h.allowAdminTOTPAttempt(w, r) {
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()

	user, ok := h.loadChallengeUserForPhase(w, r, ctx, challenge, adminTOTPStateMFARequired)
	if !ok {
		return
	}

	valid, err := h.authSvc.ValidateTOTPCode(strings.TrimSpace(*user.AdminTOTPSecret), code, time.Now())
	if err != nil {
		if errors.Is(err, auth.ErrTOTPCodeMalformed) {
			h.renderVerifyError(w, r, http.StatusBadRequest, challenge, "cÃ³digo TOTP invÃ¡lido")
			return
		}
		slog.Error("admin totp challenge validation failed", "err", err, "user_id", user.ID)
		h.renderVerifyError(w, r, http.StatusInternalServerError, challenge, "erro interno")
		return
	}
	if !valid {
		slog.Warn("admin totp challenge rejected", "user_id", user.ID, "phase", "invalid_code", "bucket", auth.RateLimitBucketAdminTOTP)
		h.renderVerifyError(w, r, http.StatusUnauthorized, challenge, "cÃ³digo TOTP invÃ¡lido")
		return
	}

	h.totpLimiter.ResetRequest(r)
	slog.Info("admin totp challenge completed", "user_id", user.ID, "phase", string(adminTOTPStateMFARequired), "bucket", auth.RateLimitBucketAdminTOTP)
	h.completeAdminLogin(w, r, ctx, user)
}

func (h *Handler) Logout(w http.ResponseWriter, r *http.Request) {
	h.clearAdminSessionCookie(w, r)
	h.clearAdminChallengeCookie(w, r)
	http.Redirect(w, r, "/admin/login", http.StatusSeeOther)
}

func (h *Handler) Dashboard(w http.ResponseWriter, r *http.Request) {
	companies, err := h.svc.ListCompanies(r.Context())
	if err != nil {
		slog.Error("admin list companies failed", "err", err)
		renderTemplate(w, http.StatusInternalServerError, dashboardTemplate, dashboardPageData{
			Error:     "falha ao carregar empresas",
			CSRFToken: csrfFromCtx(r),
		})
		return
	}

	stats, err := h.svc.GetDashboardStats(r.Context())
	if err != nil {
		slog.Error("admin get dashboard stats failed", "err", err)
	}

	var healthView *dashboardHealthView
	if h.healthSnapshot != nil {
		snapshot := h.healthSnapshot(r.Context())
		healthView = buildDashboardHealthView(snapshot)
	}

	renderTemplate(w, http.StatusOK, dashboardTemplate, dashboardPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Stats:     stats,
		Health:    healthView,
		Companies: companies,
		CSRFToken: csrfFromCtx(r),
	})
}

func readAdminLoginRequest(r *http.Request) (adminLoginRequest, error) {
	var req adminLoginRequest
	if requestUsesJSON(r) {
		if err := decodeJSONBody(r, &req); err != nil {
			return adminLoginRequest{}, err
		}
	} else {
		req.Email, req.Password = readCredentials(r)
	}
	req.Email = strings.TrimSpace(strings.ToLower(req.Email))
	return req, nil
}

func readAdminTOTPCode(r *http.Request) (string, error) {
	if requestUsesJSON(r) {
		var req adminTOTPCodeRequest
		if err := decodeJSONBody(r, &req); err != nil {
			return "", err
		}
		return strings.TrimSpace(req.Code), nil
	}
	return strings.TrimSpace(readParam(r, "code")), nil
}

func (h *Handler) allowAdminPasswordAttempt(w http.ResponseWriter, r *http.Request) bool {
	if h.passwordLimiter == nil {
		return true
	}
	decision := h.passwordLimiter.CheckRequest(r)
	if decision.Allowed {
		return true
	}
	h.respondAdminRateLimit(w, r, "muitas tentativas de senha; tente novamente mais tarde", auth.RateLimitBucketAdminPassword)
	return false
}

func (h *Handler) allowAdminTOTPAttempt(w http.ResponseWriter, r *http.Request) bool {
	if h.totpLimiter == nil {
		return true
	}
	decision := h.totpLimiter.CheckRequest(r)
	if decision.Allowed {
		return true
	}
	h.respondAdminRateLimit(w, r, "muitas tentativas de TOTP; tente novamente mais tarde", auth.RateLimitBucketAdminTOTP)
	return false
}

func (h *Handler) respondAdminRateLimit(w http.ResponseWriter, r *http.Request, message, bucket string) {
	retryAfter := 0
	switch bucket {
	case auth.RateLimitBucketAdminPassword:
		if h.passwordLimiter != nil {
			retryAfter = h.passwordLimiter.RetryAfterSeconds()
		}
	case auth.RateLimitBucketAdminTOTP:
		if h.totpLimiter != nil {
			retryAfter = h.totpLimiter.RetryAfterSeconds()
		}
	}
	if retryAfter > 0 {
		w.Header().Set("Retry-After", strconv.Itoa(retryAfter))
	}
	slog.Warn("admin auth rate limited", "bucket", bucket, "ip", r.RemoteAddr, "path", r.URL.Path, "retry_after_sec", retryAfter)
	if wantsJSON(r) {
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": message})
		return
	}
	if bucket == auth.RateLimitBucketAdminTOTP {
		if challenge, err := h.currentAdminChallenge(r); err == nil {
			if challenge.Scope == auth.ScopeAdminChallengeSetup {
				h.renderSetupPage(w, r, http.StatusTooManyRequests, challenge, message)
				return
			}
			h.renderVerifyPage(w, r, http.StatusTooManyRequests, challenge, message)
			return
		}
	}
	renderTemplate(w, http.StatusTooManyRequests, loginTemplate, loginPageData{Error: message, CSRFToken: csrfFromCtx(r)})
}

func (h *Handler) loadChallengeUserForPhase(w http.ResponseWriter, r *http.Request, ctx context.Context, challenge *auth.Claims, expected adminTOTPState) (*auth.UserRecord, bool) {
	user, err := h.users.GetUserByID(ctx, challenge.UserID)
	if err != nil {
		if errors.Is(err, auth.ErrUserNotFound) {
			slog.Warn("admin challenge user missing", "user_id", challenge.UserID, "phase", string(expected))
			h.rejectChallenge(w, r, http.StatusUnauthorized, "desafio expirado")
			return nil, false
		}
		slog.Error("admin challenge user lookup failed", "err", err, "user_id", challenge.UserID)
		h.rejectChallenge(w, r, http.StatusInternalServerError, "erro interno")
		return nil, false
	}
	if !user.IsActive {
		slog.Warn("admin challenge user inactive", "user_id", user.ID, "phase", string(expected))
		h.rejectChallenge(w, r, http.StatusForbidden, "usuÃ¡rio inativo")
		return nil, false
	}
	if user.Role != auth.RoleSuperAdmin {
		slog.Warn("admin challenge role rejected", "user_id", user.ID, "role", user.Role, "phase", string(expected))
		h.rejectChallenge(w, r, http.StatusForbidden, "acesso administrativo requerido")
		return nil, false
	}

	state, err := classifyAdminTOTPState(user)
	if err != nil {
		slog.Error("admin challenge totp state invalid", "user_id", user.ID, "phase", string(expected))
		h.rejectChallenge(w, r, http.StatusInternalServerError, "configuraÃ§Ã£o administrativa invÃ¡lida")
		return nil, false
	}
	if state != expected {
		slog.Warn("admin challenge state mismatch", "user_id", user.ID, "phase", string(expected), "actual", state)
		h.rejectChallenge(w, r, http.StatusUnauthorized, "desafio expirado")
		return nil, false
	}
	if expected == adminTOTPStateMFARequired && user.AdminTOTPSecret == nil {
		slog.Error("admin challenge missing persisted totp secret", "user_id", user.ID)
		h.rejectChallenge(w, r, http.StatusInternalServerError, "configuraÃ§Ã£o administrativa invÃ¡lida")
		return nil, false
	}

	return user, true
}

func (h *Handler) requireChallenge(w http.ResponseWriter, r *http.Request, expectedScope string) (*auth.Claims, bool) {
	challenge, err := h.currentAdminChallenge(r)
	if err != nil {
		message := "desafio expirado"
		status := http.StatusUnauthorized
		if errors.Is(err, auth.ErrTokenInvalid) {
			message = "desafio invÃ¡lido"
		}
		h.rejectChallenge(w, r, status, message)
		return nil, false
	}
	if challenge.Scope != expectedScope {
		http.Redirect(w, r, h.challengeRedirectTarget(challenge), http.StatusSeeOther)
		return nil, false
	}
	return challenge, true
}

func (h *Handler) challengeRedirectTarget(challenge *auth.Claims) string {
	if challenge != nil && challenge.Scope == auth.ScopeAdminChallengeSetup {
		return "/admin/setup"
	}
	return "/admin/verify"
}

func (h *Handler) renderSetupPage(w http.ResponseWriter, r *http.Request, status int, challenge *auth.Claims, errMsg string) {
	renderTemplate(w, status, adminSetupTemplate, adminSetupPageData{
		Error:      errMsg,
		CSRFToken:  csrfFromCtx(r),
		Email:      challenge.Email,
		Secret:     challenge.PendingTOTPSecret,
		OTPAuthURL: challenge.PendingTOTPURL,
	})
}

func (h *Handler) renderVerifyPage(w http.ResponseWriter, r *http.Request, status int, challenge *auth.Claims, errMsg string) {
	renderTemplate(w, status, adminVerifyTemplate, adminVerifyPageData{
		Error:     errMsg,
		CSRFToken: csrfFromCtx(r),
		Email:     challenge.Email,
	})
}

func (h *Handler) renderSetupError(w http.ResponseWriter, r *http.Request, status int, challenge *auth.Claims, errMsg string) {
	if wantsJSON(r) {
		writeJSON(w, status, map[string]string{"error": errMsg})
		return
	}
	h.renderSetupPage(w, r, status, challenge, errMsg)
}

func (h *Handler) renderVerifyError(w http.ResponseWriter, r *http.Request, status int, challenge *auth.Claims, errMsg string) {
	if wantsJSON(r) {
		writeJSON(w, status, map[string]string{"error": errMsg})
		return
	}
	h.renderVerifyPage(w, r, status, challenge, errMsg)
}

func (h *Handler) rejectChallenge(w http.ResponseWriter, r *http.Request, status int, message string) {
	h.clearAdminSessionCookie(w, r)
	h.clearAdminChallengeCookie(w, r)
	if wantsJSON(r) {
		writeJSON(w, status, map[string]string{"error": message})
		return
	}
	http.Redirect(w, r, "/admin/login?error="+url.QueryEscape(message), http.StatusSeeOther)
}

func (h *Handler) completeAdminLogin(w http.ResponseWriter, r *http.Request, ctx context.Context, user *auth.UserRecord) {
	adminToken, err := h.authSvc.GenerateAdminSessionToken(auth.TokenSubject{
		UserID: user.ID,
		Email:  user.Email,
		Name:   user.Name,
		Role:   user.Role,
	})
	if err != nil {
		slog.Error("admin final session token generation failed", "err", err, "user_id", user.ID)
		if wantsJSON(r) {
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "erro interno"})
			return
		}
		http.Redirect(w, r, "/admin/login?error=erro+interno", http.StatusSeeOther)
		return
	}

	if err := h.users.TouchUser(ctx, user.ID, time.Now()); err != nil && !errors.Is(err, context.DeadlineExceeded) {
		slog.Warn("admin login touch user failed", "err", err, "user_id", user.ID)
	}

	h.setAdminSessionCookie(w, r, adminToken)
	h.clearAdminChallengeCookie(w, r)
	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
		return
	}
	http.Redirect(w, r, "/admin", http.StatusSeeOther)
}

func (h *Handler) setAdminSessionCookie(w http.ResponseWriter, r *http.Request, value string) {
	http.SetCookie(w, &http.Cookie{
		Name:     adminCookieName,
		Value:    value,
		Path:     "/admin",
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
		Secure:   isAdminSecureRequest(r),
	})
}

func (h *Handler) clearAdminSessionCookie(w http.ResponseWriter, r *http.Request) {
	http.SetCookie(w, &http.Cookie{
		Name:     adminCookieName,
		Value:    "",
		Path:     "/admin",
		HttpOnly: true,
		MaxAge:   -1,
		SameSite: http.SameSiteLaxMode,
		Secure:   isAdminSecureRequest(r),
	})
}

func (h *Handler) setAdminChallengeCookie(w http.ResponseWriter, r *http.Request, value string) {
	http.SetCookie(w, &http.Cookie{
		Name:     adminChallengeCookieName,
		Value:    value,
		Path:     "/admin",
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
		Secure:   isAdminSecureRequest(r),
	})
}

func (h *Handler) clearAdminChallengeCookie(w http.ResponseWriter, r *http.Request) {
	http.SetCookie(w, &http.Cookie{
		Name:     adminChallengeCookieName,
		Value:    "",
		Path:     "/admin",
		HttpOnly: true,
		MaxAge:   -1,
		SameSite: http.SameSiteLaxMode,
		Secure:   isAdminSecureRequest(r),
	})
}
