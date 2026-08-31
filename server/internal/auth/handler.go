package auth

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Handler expõe os endpoints de autenticação.
type Handler struct {
	svc          *Service
	users        UserStore
	limiter      *RateLimiter
	queryTimeout time.Duration
}

// NewHandler cria um novo handler de autenticação.
func NewHandler(svc *Service, db *pgxpool.Pool, limiter *RateLimiter) *Handler {
	return newHandlerWithStore(svc, NewPostgresUserStore(db), limiter)
}

func newHandlerWithStore(svc *Service, users UserStore, limiter *RateLimiter) *Handler {
	if limiter == nil {
		limiter = NewBucketRateLimiter(
			NewInMemoryRateLimitStore(InMemoryRateLimitStoreConfig{MaxEntries: 2048}),
			RateLimiterConfig{},
			RateLimitBucketLogin,
		)
	}
	return &Handler{
		svc:          svc,
		users:        users,
		limiter:      limiter,
		queryTimeout: 5 * time.Second,
	}
}

// Routes registra as rotas de autenticação no router.
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Post("/register", h.Register)
	r.With(RateLimitMiddleware(h.limiter)).Post("/login", h.Login)
	r.Post("/refresh", h.Refresh)
	return r
}

type registerRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
	Name     string `json:"name"`
}

type loginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

type refreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

type authResponse struct {
	UserID       string `json:"user_id"`
	Email        string `json:"email"`
	Name         string `json:"name"`
	Role         Role   `json:"role"`
	CompanyID    string `json:"company_id,omitempty"`
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int64  `json:"expires_in"`
}

type errorResponse struct {
	Error string `json:"error"`
}

// Register mantém compatibilidade de rota, mas desabilita auto cadastro público.
func (h *Handler) Register(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusForbidden, errorResponse{Error: "public registration is disabled"})
}

// Login autentica usuários públicos de empresa; super admin deve usar a rota dedicada de admin.
func (h *Handler) Login(w http.ResponseWriter, r *http.Request) {
	var req loginRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	req.Email = strings.TrimSpace(strings.ToLower(req.Email))
	if req.Email == "" || req.Password == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "email and password are required"})
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), h.queryTimeout)
	defer cancel()

	user, err := h.users.GetUserByEmail(ctx, req.Email)
	if err != nil {
		if errors.Is(err, ErrUserNotFound) {
			writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "invalid email or password"})
			return
		}
		slog.Error("db error fetching user for login", "err", err, "email", req.Email)
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
		return
	}

	if !user.IsActive || (user.CompanyID != nil && !user.CompanyActive) {
		writeJSON(w, http.StatusForbidden, errorResponse{Error: "access blocked"})
		return
	}

	if user.Role == RoleSuperAdmin {
		slog.Warn("public super-admin login rejected", "user_id", user.ID, "email", req.Email)
		writeJSON(w, http.StatusForbidden, errorResponse{Error: "admin login requires /admin/login"})
		return
	}

	if !h.svc.CheckPassword(req.Password, user.PasswordHash) {
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "invalid email or password"})
		return
	}

	h.limiter.Reset(ctx, rateLimitClientIP(r))

	tokens, err := h.svc.GenerateTokenPair(tokenSubjectFromRecord(user))
	if err != nil {
		slog.Error("token generation error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
		return
	}

	if err := h.users.TouchUser(ctx, user.ID, time.Now()); err != nil && !errors.Is(err, context.DeadlineExceeded) {
		slog.Warn("db error updating last login timestamp", "err", err, "user_id", user.ID)
	}

	slog.Info("user logged in", "user_id", user.ID, "email", req.Email, "role", user.Role)
	writeJSON(w, http.StatusOK, authResponseFromUser(user, tokens))
}

// Refresh gera um novo par de tokens para um usuário ainda ativo.
func (h *Handler) Refresh(w http.ResponseWriter, r *http.Request) {
	var req refreshRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid request body"})
		return
	}

	if req.RefreshToken == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "refresh_token is required"})
		return
	}

	claims, err := h.svc.ValidateRefreshToken(req.RefreshToken)
	if err != nil {
		msg := "invalid refresh token"
		if err == ErrTokenExpired {
			msg = "refresh token expired, please login again"
		}
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: msg})
		return
	}

	if claims.IsMachine() {
		ctx, cancel := context.WithTimeout(r.Context(), h.queryTimeout)
		defer cancel()

		machine, machineErr := h.users.GetMachineByID(ctx, claims.MachineID)
		if machineErr != nil {
			if errors.Is(machineErr, ErrMachineNotFound) {
				writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "machine not found"})
				return
			}
			slog.Error("db error checking refresh machine", "err", machineErr, "machine_id", claims.MachineID)
			writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
			return
		}
		if machine.CompanyID != claims.CompanyID {
			writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "machine not found"})
			return
		}

		tokens, tokenErr := h.svc.GenerateTokenPair(TokenSubject{
			UserID:      claims.UserID,
			Name:        machine.Name,
			Role:        claims.Role,
			CompanyID:   machine.CompanyID,
			SubjectType: claims.SubjectType,
			MachineID:   machine.ID,
		})
		if tokenErr != nil {
			slog.Error("machine token refresh error", "err", tokenErr, "machine_id", claims.MachineID)
			writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
			return
		}

		writeJSON(w, http.StatusOK, authResponse{
			UserID:       machine.ID,
			Name:         machine.Name,
			Role:         claims.Role,
			CompanyID:    machine.CompanyID,
			AccessToken:  tokens.AccessToken,
			RefreshToken: tokens.RefreshToken,
			ExpiresIn:    tokens.ExpiresIn,
		})
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), h.queryTimeout)
	defer cancel()

	user, err := h.users.GetUserByID(ctx, claims.UserID)
	if err != nil {
		if errors.Is(err, ErrUserNotFound) {
			writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "user not found"})
			return
		}
		slog.Error("db error checking refresh user", "err", err, "user_id", claims.UserID)
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
		return
	}

	if !user.IsActive || (user.CompanyID != nil && !user.CompanyActive) {
		writeJSON(w, http.StatusForbidden, errorResponse{Error: "access blocked"})
		return
	}

	tokens, err := h.svc.GenerateTokenPair(tokenSubjectFromRecord(user))
	if err != nil {
		slog.Error("token generation error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, authResponseFromUser(user, tokens))
}

func authResponseFromUser(user *UserRecord, tokens *TokenPair) authResponse {
	resp := authResponse{
		UserID:       user.ID,
		Email:        user.Email,
		Name:         user.Name,
		Role:         user.Role,
		AccessToken:  tokens.AccessToken,
		RefreshToken: tokens.RefreshToken,
		ExpiresIn:    tokens.ExpiresIn,
	}
	if user.CompanyID != nil {
		resp.CompanyID = *user.CompanyID
	}
	return resp
}

func tokenSubjectFromRecord(user *UserRecord) TokenSubject {
	subject := TokenSubject{
		UserID:      user.ID,
		Email:       user.Email,
		Name:        user.Name,
		Role:        user.Role,
		SubjectType: SubjectTypeUser,
	}
	if user.CompanyID != nil {
		subject.CompanyID = *user.CompanyID
	}
	return subject
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// ValidateTokenFromRequest é uma função utilitária para validar um token
// a partir de um http.Request (usado pelo signaling server).
func ValidateTokenFromRequest(svc *Service, r *http.Request) (*Claims, error) {
	tokenStr := extractToken(r)
	if tokenStr == "" {
		return nil, ErrTokenInvalid
	}
	return svc.ValidateToken(tokenStr)
}

type contextKeyDB string

// WithDB adiciona o pool de conexão ao contexto.
func WithDB(ctx context.Context, db *pgxpool.Pool) context.Context {
	return context.WithValue(ctx, contextKeyDB("db"), db)
}
