package installer

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/machinelimit"
	"github.com/azvlucas/remotex-portfolio/server/internal/machines"
)

// Handler expõe o endpoint de geração de instaladores.
type Handler struct {
	svc      *Service
	authSvc  *auth.Service
	machines *machines.Service
}

// NewHandler cria um handler de instalador.
func NewHandler(svc *Service, authSvc *auth.Service, machineSvc *machines.Service) *Handler {
	return &Handler{svc: svc, authSvc: authSvc, machines: machineSvc}
}

// ProtectedRoutes registra as rotas protegidas do instalador.
func (h *Handler) ProtectedRoutes() chi.Router {
	r := chi.NewRouter()
	r.Post("/installer", h.GenerateInstaller)
	return r
}

// PublicRoutes registra as rotas públicas de bootstrap.
func (h *Handler) PublicRoutes() chi.Router {
	r := chi.NewRouter()
	r.Post("/enroll", h.Enroll)
	return r
}

type installerRequest struct {
	APIHost         string `json:"api_host"`
	APIPort         int    `json:"api_port"`
	APIUseTLS       *bool  `json:"api_use_tls"`
	SignalingHost   string `json:"signaling_host"`
	SignalingPort   int    `json:"signaling_port"`
	SignalingUseTLS *bool  `json:"signaling_use_tls"`
	RelayHost       string `json:"relay_host"`
	RelayPort       int    `json:"relay_port"`
}

type enrollRequest struct {
	EnrollmentToken string `json:"enrollment_token"`
	MachineName     string `json:"machine_name"`
	OSInfo          string `json:"os_info,omitempty"`
}

type enrollResponse struct {
	MachineID    string                   `json:"machine_id"`
	MachineKey   string                   `json:"machine_key"`
	UserID       string                   `json:"user_id"`
	Role         auth.Role                `json:"role"`
	CompanyID    string                   `json:"company_id,omitempty"`
	State        machinelimit.State       `json:"state"`
	BlockReason  machinelimit.BlockReason `json:"block_reason,omitempty"`
	AccessToken  string                   `json:"access_token"`
	RefreshToken string                   `json:"refresh_token"`
	ExpiresIn    int64                    `json:"expires_in"`
}

// GenerateInstaller gera o instalador NSIS com credenciais do usuário autenticado.
// POST /host/installer
func (h *Handler) GenerateInstaller(w http.ResponseWriter, r *http.Request) {
	// Extrair user claims do contexto (JWT middleware já validou).
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	slog.Info("generating installer", "user_id", claims.UserID, "email", claims.Email)

	var req installerRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil && !errors.Is(err, io.EOF) {
		http.Error(w, `{"error":"invalid request body"}`, http.StatusBadRequest)
		return
	}

	bootstrapToken, err := h.authSvc.GenerateEnrollmentToken(auth.TokenSubject{
		UserID:      claims.UserID,
		Email:       claims.Email,
		Name:        claims.Name,
		Role:        claims.Role,
		CompanyID:   claims.CompanyID,
		SubjectType: auth.SubjectTypeUser,
	}, 72*time.Hour)
	if err != nil {
		slog.Error("failed to generate enrollment token for installer", "err", err)
		http.Error(w, `{"error":"failed to generate enrollment token"}`, http.StatusInternalServerError)
		return
	}

	// Buildar o instalador.
	installerPath, cleanup, err := h.svc.BuildInstaller(
		normalizeBootstrapRequest(req, bootstrapToken),
	)
	if err != nil {
		slog.Error("failed to build installer", "err", err, "user_id", claims.UserID)
		http.Error(w, `{"error":"failed to build installer: `+err.Error()+`"}`, http.StatusInternalServerError)
		return
	}
	defer cleanup()

	// Ler o instalador gerado.
	data, err := os.ReadFile(installerPath)
	if err != nil {
		slog.Error("failed to read installer", "err", err, "path", installerPath)
		http.Error(w, `{"error":"failed to read generated installer"}`, http.StatusInternalServerError)
		return
	}

	slog.Info("installer generated successfully",
		"user_id", claims.UserID,
		"size_bytes", len(data),
	)

	// Enviar como download.
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="RemoteX_Host_Setup.exe"`)
	w.Header().Set("Content-Length", fmt.Sprintf("%d", len(data)))
	w.WriteHeader(http.StatusOK)
	w.Write(data)
}

func (h *Handler) Enroll(w http.ResponseWriter, r *http.Request) {
	if h.machines == nil {
		http.Error(w, `{"error":"host enrollment unavailable"}`, http.StatusServiceUnavailable)
		return
	}

	var req enrollRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"invalid request body"}`, http.StatusBadRequest)
		return
	}
	if strings.TrimSpace(req.EnrollmentToken) == "" {
		http.Error(w, `{"error":"enrollment_token is required"}`, http.StatusBadRequest)
		return
	}

	claims, err := h.authSvc.ValidateEnrollmentToken(strings.TrimSpace(req.EnrollmentToken))
	if err != nil {
		status := http.StatusUnauthorized
		if err == auth.ErrTokenExpired {
			status = http.StatusUnauthorized
		}
		http.Error(w, `{"error":"invalid or expired enrollment token"}`, status)
		return
	}
	if claims.CompanyID == "" {
		http.Error(w, `{"error":"company scope missing from enrollment token"}`, http.StatusForbidden)
		return
	}

	machineName := strings.TrimSpace(req.MachineName)
	if machineName == "" {
		machineName = "RemoteX Host"
	}

	machine, err := h.machines.Create(r.Context(), claims.UserID, claims.CompanyID, machines.MachineUpsertInput{
		Name:   machineName,
		OSInfo: stringPtr(strings.TrimSpace(req.OSInfo)),
	})
	if err != nil {
		slog.Error("host enrollment create machine failed", "err", err, "company_id", claims.CompanyID)
		http.Error(w, `{"error":"failed to enroll host"}`, http.StatusInternalServerError)
		return
	}
	if machine.State == "" || (machine.State == machinelimit.StateBlocked && machine.BlockReason == "") {
		slog.Error("host enrollment missing machine state metadata",
			"machine_id", machine.ID,
			"company_id", machine.CompanyID,
			"state", machine.State,
			"block_reason", machine.BlockReason,
		)
		http.Error(w, `{"error":"failed to enroll host"}`, http.StatusInternalServerError)
		return
	}

	tokens, err := h.authSvc.GenerateTokenPair(auth.TokenSubject{
		UserID:      machine.ID,
		Name:        machine.Name,
		Role:        auth.RoleMachineAgent,
		CompanyID:   machine.CompanyID,
		SubjectType: auth.SubjectTypeMachine,
		MachineID:   machine.ID,
	})
	if err != nil {
		slog.Error("host enrollment token generation failed", "err", err, "machine_id", machine.ID)
		http.Error(w, `{"error":"failed to create machine token"}`, http.StatusInternalServerError)
		return
	}

	resp := enrollResponse{
		MachineID:    machine.ID,
		MachineKey:   machine.MachineKey,
		UserID:       machine.ID,
		Role:         auth.RoleMachineAgent,
		CompanyID:    machine.CompanyID,
		State:        machine.State,
		BlockReason:  machine.BlockReason,
		AccessToken:  tokens.AccessToken,
		RefreshToken: tokens.RefreshToken,
		ExpiresIn:    tokens.ExpiresIn,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	_ = json.NewEncoder(w).Encode(resp)
}

func normalizeBootstrapRequest(req installerRequest, enrollmentToken string) BootstrapConfig {
	apiUseTLS := true
	if req.APIUseTLS != nil {
		apiUseTLS = *req.APIUseTLS
	}
	signalingUseTLS := true
	if req.SignalingUseTLS != nil {
		signalingUseTLS = *req.SignalingUseTLS
	}

	cfg := BootstrapConfig{
		APIHost:         strings.TrimSpace(req.APIHost),
		APIPort:         req.APIPort,
		APIUseTLS:       apiUseTLS,
		SignalingHost:   strings.TrimSpace(req.SignalingHost),
		SignalingPort:   req.SignalingPort,
		SignalingUseTLS: signalingUseTLS,
		RelayHost:       strings.TrimSpace(req.RelayHost),
		RelayPort:       req.RelayPort,
		EnrollmentToken: enrollmentToken,
	}

	if cfg.APIHost == "" {
		cfg.APIHost = "demo.remotex.example"
	}
	if cfg.APIPort == 0 {
		cfg.APIPort = 443
	}
	if cfg.SignalingHost == "" {
		cfg.SignalingHost = "demo.remotex.example"
	}
	if cfg.SignalingPort == 0 {
		cfg.SignalingPort = 443
	}
	if cfg.RelayHost == "" {
		cfg.RelayHost = "demo.remotex.example"
	}
	if cfg.RelayPort == 0 {
		cfg.RelayPort = 9100
	}
	return cfg
}

func stringPtr(value string) *string {
	if value == "" {
		return nil
	}
	return &value
}
