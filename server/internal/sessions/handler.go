package sessions

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/relay"
)

// Handler expoe os endpoints de sessoes.
type Handler struct {
	svc             sessionService
	transportIssuer *relay.TicketIssuer
}

type sessionService interface {
	List(ctx context.Context, companyID string, limit, offset int) ([]Session, error)
	Get(ctx context.Context, sessionID, companyID string) (*Session, error)
	Start(ctx context.Context, userID, companyID string, isManager bool, req StartRequest) (*Session, error)
	End(ctx context.Context, sessionID, companyID string) (*Session, error)
	ResolveConsent(ctx context.Context, sessionID, companyID, machineID, decision string) (*Session, error)
	Heartbeat(ctx context.Context, sessionID, companyID, userID, machineID string, isMachine bool) error
}

// NewHandler cria um novo handler.
func NewHandler(svc sessionService, issuers ...*relay.TicketIssuer) *Handler {
	var issuer *relay.TicketIssuer
	if len(issuers) > 0 {
		issuer = issuers[0]
	}
	return &Handler{svc: svc, transportIssuer: issuer}
}

// Routes registra as rotas de sessoes.
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/", h.List)
	r.Get("/{id}", h.Get)
	r.Post("/", h.Start)
	r.Put("/{id}", h.End)
	r.Put("/{id}/heartbeat", h.Heartbeat)
	r.Post("/{id}/consent", h.ResolveConsent)
	return r
}

type startRequest struct {
	MachineID  string `json:"machine_id"`
	IPViewer   string `json:"ip_viewer,omitempty"`
	IPHost     string `json:"ip_host,omitempty"`
	AccessMode string `json:"access_mode,omitempty"`
}

type consentRequest struct {
	Decision string `json:"decision"`
}

type errorResp struct {
	Error string `json:"error"`
}

const machineBlockedByLimitMessage = "Esta maquina esta bloqueada: Limite de maquinas excedido."

type sessionResponse struct {
	Session
	Transport *relay.TransportBundle `json:"transport,omitempty"`
}

func (h *Handler) List(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}
	if claims.IsMachine() {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "human user required"})
		return
	}

	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))

	sessions, err := h.svc.List(r.Context(), claims.CompanyID, limit, offset)
	if err != nil {
		slog.Error("list sessions error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, sessions)
}

func (h *Handler) Get(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	sess, err := h.svc.Get(r.Context(), chi.URLParam(r, "id"), claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "session not found"})
		return
	}
	if err != nil {
		slog.Error("get session error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}
	if claims.IsMachine() && sess.MachineID != claims.MachineID {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}

	writeJSON(w, http.StatusOK, h.withTransport(claims, sess))
}

func (h *Handler) Start(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}
	if claims.IsMachine() {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "human user required"})
		return
	}

	var req startRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}
	if req.MachineID == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "machine_id is required"})
		return
	}

	accessMode, err := normalizeAccessMode(req.AccessMode)
	if err == ErrInvalidAccessMode {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid access_mode"})
		return
	}
	if err != nil {
		slog.Error("normalize access mode error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	sess, err := h.svc.Start(
		r.Context(),
		claims.UserID,
		claims.CompanyID,
		claims.Role == auth.RoleCompanyManager || claims.Role == auth.RoleSuperAdmin,
		StartRequest{
			MachineID:  req.MachineID,
			IPViewer:   req.IPViewer,
			IPHost:     req.IPHost,
			AccessMode: accessMode,
		},
	)
	if err == ErrConcurrentSessionLimit {
		writeJSON(w, http.StatusConflict, errorResp{Error: "Limite de sessões simultâneas atingido para esta empresa"})
		return
	}
	if errors.Is(err, ErrMachineBlockedByLimit) {
		writeJSON(w, http.StatusForbidden, errorResp{Error: machineBlockedByLimitMessage})
		return
	}
	if err == ErrManagerAccessRequired {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "manager access required"})
		return
	}
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err == ErrInvalidAccessMode {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid access_mode"})
		return
	}
	if err != nil {
		slog.Error("start session error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("session started", "id", sess.ID, "machine", req.MachineID, "status", sess.Status)
	writeJSON(w, http.StatusCreated, h.withTransport(claims, sess))
}

func (h *Handler) End(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}
	if claims.IsMachine() {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "human user required"})
		return
	}

	sess, err := h.svc.End(r.Context(), chi.URLParam(r, "id"), claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "session not found"})
		return
	}
	if err != nil {
		slog.Error("end session error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("session ended", "id", sess.ID, "status", sess.Status, "duration_sec", sess.DurationSec)
	writeJSON(w, http.StatusOK, h.withTransport(claims, sess))
}

func (h *Handler) Heartbeat(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	err := h.svc.Heartbeat(
		r.Context(),
		chi.URLParam(r, "id"),
		claims.CompanyID,
		claims.UserID,
		claims.MachineID,
		claims.IsMachine(),
	)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "session not found"})
		return
	}
	if err == ErrHeartbeatAccessDenied {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "session access denied"})
		return
	}
	if err != nil {
		slog.Error("session heartbeat error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) ResolveConsent(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	if !claims.IsMachine() || strings.TrimSpace(claims.MachineID) == "" {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}

	var req consentRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	machineID := claims.MachineID
	sessionID := chi.URLParam(r, "id")
	slog.Info("resolve consent request",
		"session_id", sessionID,
		"company_id", claims.CompanyID,
		"machine_id", machineID,
		"decision", req.Decision,
		"is_machine", claims.IsMachine(),
	)

	sess, err := h.svc.ResolveConsent(r.Context(), sessionID, claims.CompanyID, machineID, req.Decision)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "session not found"})
		return
	}
	if err == ErrMachineAccessDenied {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}
	if err == ErrInvalidConsentDecision {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid consent decision"})
		return
	}
	if err == ErrSessionConsentNotPending {
		writeJSON(w, http.StatusConflict, errorResp{Error: "session consent not pending"})
		return
	}
	if err != nil {
		slog.Error("resolve session consent error",
			"err", err,
			"session_id", sessionID,
			"company_id", claims.CompanyID,
			"machine_id", machineID,
			"decision", req.Decision,
		)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error: " + err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, h.withTransport(claims, sess))
}

func (h *Handler) withTransport(claims *auth.Claims, sess *Session) sessionResponse {
	resp := sessionResponse{Session: *sess}
	if h.transportIssuer == nil || claims == nil || sess == nil || sess.ID == "" {
		return resp
	}

	role := relay.RoleViewer
	userID := claims.UserID
	machineID := ""
	if claims.IsMachine() {
		role = relay.RoleHost
		userID = ""
		machineID = claims.MachineID
	}

	transport, err := h.transportIssuer.IssueBundle(sess.ID, role, sess.CompanyID, userID, firstNonEmpty(machineID, sess.MachineID))
	if err != nil {
		slog.Warn("failed to issue relay ticket",
			"session_id", sess.ID,
			"role", role,
			"err", err,
		)
		return resp
	}
	resp.Transport = transport
	return resp
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		_ = json.NewEncoder(w).Encode(v)
	}
}
