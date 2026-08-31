package machines

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/audit"
	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

type Handler struct {
	svc      *Service
	auditSvc *audit.Service
}

func NewHandler(svc *Service, auditSvc *audit.Service) *Handler {
	return &Handler{svc: svc, auditSvc: auditSvc}
}

func (h *Handler) RegisterHumanRoutes(r chi.Router) {
	r.Get("/", h.List)
	r.Post("/", h.Create)
	r.Put("/{id}", h.Update)
	r.Delete("/{id}", h.Delete)
	r.Get("/{id}/status", h.Status)
	r.Get("/{id}/audit", h.ListAudit)
	r.Post("/{id}/force-update", h.ForceUpdate)
}

func (h *Handler) RegisterCompanyRoutes(r chi.Router) {
	r.Post("/{id}/heartbeat", h.Heartbeat)
	r.Post("/{id}/audit", h.CreateAudit)
}

type createRequest struct {
	Name                 string `json:"name"`
	OSInfo               string `json:"os_info,omitempty"`
	MacAddress           string `json:"mac_address,omitempty"`
	GroupID              string `json:"group_id,omitempty"`
	SubgroupID           string `json:"subgroup_id,omitempty"`
	LegalName            string `json:"legal_name,omitempty"`
	TradeName            string `json:"trade_name,omitempty"`
	DocumentType         string `json:"document_type,omitempty"`
	DocumentNumber       string `json:"document_number,omitempty"`
	ManagersOnlyAccess   bool   `json:"managers_only_access"`
	RequireClientConsent bool   `json:"require_client_consent"`
}

type updateRequest struct {
	Name                 string `json:"name"`
	OSInfo               string `json:"os_info,omitempty"`
	MacAddress           string `json:"mac_address,omitempty"`
	GroupID              string `json:"group_id,omitempty"`
	SubgroupID           string `json:"subgroup_id,omitempty"`
	LegalName            string `json:"legal_name,omitempty"`
	TradeName            string `json:"trade_name,omitempty"`
	DocumentType         string `json:"document_type,omitempty"`
	DocumentNumber       string `json:"document_number,omitempty"`
	ManagersOnlyAccess   bool   `json:"managers_only_access"`
	RequireClientConsent bool   `json:"require_client_consent"`
}

type heartbeatRequest struct {
	MachineKey   string `json:"machine_key"`
	Uptime       int64  `json:"uptime,omitempty"`
	AgentVersion string `json:"agent_version,omitempty"`
}

type createAuditRequest struct {
	SessionID            string `json:"session_id,omitempty"`
	EventType            string `json:"event_type"`
	Status               string `json:"status,omitempty"`
	Summary              string `json:"summary,omitempty"`
	TransferDirection    string `json:"transfer_direction,omitempty"`
	TransferRoot         string `json:"transfer_root,omitempty"`
	TransferItem         string `json:"transfer_item,omitempty"`
	TransferTotalBytes   int64  `json:"transfer_total_bytes,omitempty"`
	TransferTotalEntries int    `json:"transfer_total_entries,omitempty"`
	DurationSec          int    `json:"duration_sec,omitempty"`
	ErrorMessage         string `json:"error_message,omitempty"`
}

type statusResponse struct {
	ID           string `json:"id"`
	IsOnline     bool   `json:"is_online"`
	LastHB       string `json:"last_heartbeat,omitempty"`
	AgentVersion string `json:"agent_version,omitempty"`
}

type machineResponse struct {
	ID                   string  `json:"id"`
	UserID               string  `json:"user_id"`
	CompanyID            string  `json:"company_id"`
	Name                 string  `json:"name"`
	OSInfo               *string `json:"os_info,omitempty"`
	MacAddress           *string `json:"mac_address,omitempty"`
	AgentVersion         *string `json:"agent_version,omitempty"`
	LastHeartbeat        *string `json:"last_heartbeat,omitempty"`
	IsOnline             bool    `json:"is_online"`
	State                string  `json:"state,omitempty"`
	BlockReason          string  `json:"block_reason,omitempty"`
	GroupID              *string `json:"group_id,omitempty"`
	GroupName            *string `json:"group_name,omitempty"`
	SubgroupID           *string `json:"subgroup_id,omitempty"`
	SubgroupName         *string `json:"subgroup_name,omitempty"`
	LegalName            *string `json:"legal_name,omitempty"`
	TradeName            *string `json:"trade_name,omitempty"`
	DocumentType         *string `json:"document_type,omitempty"`
	DocumentNumber       *string `json:"document_number,omitempty"`
	ManagersOnlyAccess   bool    `json:"managers_only_access"`
	RequireClientConsent bool    `json:"require_client_consent"`
	ForceUpdate          bool    `json:"force_update"`
	CreatedAt            string  `json:"created_at"`
	UpdatedAt            string  `json:"updated_at"`
}

type machineCreateResponse struct {
	machineResponse
	MachineKey string `json:"machine_key"`
}

type errorResp struct {
	Error string `json:"error"`
}

func (h *Handler) List(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machines, err := h.svc.List(r.Context(), claims.CompanyID)
	if err != nil {
		slog.Error("list machines error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	humanMachines := make([]machineResponse, 0, len(machines))
	for _, machine := range machines {
		humanMachines = append(humanMachines, newMachineResponse(machine))
	}

	writeJSON(w, http.StatusOK, humanMachines)
}

func (h *Handler) Create(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req createRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	m, err := h.svc.Create(r.Context(), claims.UserID, claims.CompanyID, buildMachineInput(
		req.Name,
		req.OSInfo,
		req.MacAddress,
		req.GroupID,
		req.SubgroupID,
		req.LegalName,
		req.TradeName,
		req.DocumentType,
		req.DocumentNumber,
		req.ManagersOnlyAccess,
		req.RequireClientConsent,
	))
	if err != nil {
		writeMachineError(w, err)
		return
	}

	slog.Info("machine created", "id", m.ID, "user_id", claims.UserID)
	writeJSON(w, http.StatusCreated, newMachineCreateResponse(*m))
}

func (h *Handler) Update(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	var req updateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	m, err := h.svc.Update(r.Context(), id, claims.CompanyID, buildMachineInput(
		req.Name,
		req.OSInfo,
		req.MacAddress,
		req.GroupID,
		req.SubgroupID,
		req.LegalName,
		req.TradeName,
		req.DocumentType,
		req.DocumentNumber,
		req.ManagersOnlyAccess,
		req.RequireClientConsent,
	))
	if err != nil {
		writeMachineError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, newMachineResponse(*m))
}

func (h *Handler) Delete(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")
	if claims.Role != auth.RoleCompanyManager && claims.Role != auth.RoleSuperAdmin {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "manager access required"})
		return
	}

	err := h.svc.Delete(r.Context(), id, claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("delete machine error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("machine deleted", "id", id, "user_id", claims.UserID)
	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) Status(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	m, err := h.svc.GetByID(r.Context(), id, claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("get machine status error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	resp := statusResponse{ID: m.ID, IsOnline: m.IsOnline}
	if m.LastHeartbeat != nil {
		resp.LastHB = m.LastHeartbeat.Format("2006-01-02T15:04:05Z07:00")
	}
	if m.AgentVersion != nil {
		resp.AgentVersion = *m.AgentVersion
	}
	writeJSON(w, http.StatusOK, resp)
}

type heartbeatResponse struct {
	ForceUpdate bool `json:"force_update"`
}

func (h *Handler) Heartbeat(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "id")
	if !claims.IsMachine() {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}
	if claims.MachineID != machineID {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}

	var req heartbeatRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	if req.MachineKey == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "machine_key is required"})
		return
	}

	forceUpdate, err := h.svc.Heartbeat(r.Context(), req.MachineKey, claims.CompanyID, req.AgentVersion)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("heartbeat error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, heartbeatResponse{ForceUpdate: forceUpdate})
}

func (h *Handler) ForceUpdate(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "id")
	err := h.svc.SetForceUpdate(r.Context(), machineID, claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("force-update error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("force update requested", "machine_id", machineID, "by_user", claims.UserID)
	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) ListAudit(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}
	if h.auditSvc == nil {
		writeJSON(w, http.StatusServiceUnavailable, errorResp{Error: "audit unavailable"})
		return
	}

	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	events, err := h.auditSvc.ListMachineEvents(r.Context(), claims.CompanyID, chi.URLParam(r, "id"), limit, offset)
	if err != nil {
		slog.Error("list machine audit error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, events)
}

func (h *Handler) CreateAudit(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "id")
	if !claims.IsMachine() {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}
	if claims.MachineID != machineID {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}
	if h.auditSvc == nil {
		writeJSON(w, http.StatusServiceUnavailable, errorResp{Error: "audit unavailable"})
		return
	}

	var req createAuditRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	event, err := h.auditSvc.RecordEvent(r.Context(), audit.RecordEventInput{
		CompanyID:            claims.CompanyID,
		MachineID:            machineID,
		SessionID:            stringPtr(req.SessionID),
		UserID:               optionalAuditUserID(claims),
		EventType:            req.EventType,
		Status:               stringPtr(req.Status),
		Summary:              req.Summary,
		TransferDirection:    stringPtr(req.TransferDirection),
		TransferRoot:         stringPtr(req.TransferRoot),
		TransferItem:         stringPtr(req.TransferItem),
		TransferTotalBytes:   int64Ptr(req.TransferTotalBytes),
		TransferTotalEntries: intPtr(req.TransferTotalEntries),
		DurationSec:          intPtr(req.DurationSec),
		ErrorMessage:         stringPtr(req.ErrorMessage),
	})
	if errors.Is(err, audit.ErrInvalidEvent) {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid audit event"})
		return
	}
	if errors.Is(err, audit.ErrMachineNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("create machine audit error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusCreated, event)
}

func buildMachineInput(name, osInfo, macAddress, groupID, subgroupID, legalName, tradeName, documentType, documentNumber string, managersOnlyAccess bool, requireClientConsent bool) MachineUpsertInput {
	return MachineUpsertInput{
		Name:                 name,
		OSInfo:               stringPtr(osInfo),
		MacAddress:           stringPtr(macAddress),
		GroupID:              stringPtr(groupID),
		SubgroupID:           stringPtr(subgroupID),
		LegalName:            stringPtr(legalName),
		TradeName:            stringPtr(tradeName),
		DocumentType:         stringPtr(documentType),
		DocumentNumber:       stringPtr(documentNumber),
		ManagersOnlyAccess:   managersOnlyAccess,
		RequireClientConsent: requireClientConsent,
	}
}

func stringPtr(value string) *string {
	if value == "" {
		return nil
	}
	return &value
}

func intPtr(value int) *int {
	if value <= 0 {
		return nil
	}
	return &value
}

func int64Ptr(value int64) *int64 {
	if value <= 0 {
		return nil
	}
	return &value
}

func newMachineResponse(m Machine) machineResponse {
	return machineResponse{
		ID:                   m.ID,
		UserID:               m.UserID,
		CompanyID:            m.CompanyID,
		Name:                 m.Name,
		OSInfo:               m.OSInfo,
		MacAddress:           m.MacAddress,
		AgentVersion:         m.AgentVersion,
		LastHeartbeat:        formatMachineTime(m.LastHeartbeat),
		IsOnline:             m.IsOnline,
		State:                string(m.State),
		BlockReason:          string(m.BlockReason),
		GroupID:              m.GroupID,
		GroupName:            m.GroupName,
		SubgroupID:           m.SubgroupID,
		SubgroupName:         m.SubgroupName,
		LegalName:            m.LegalName,
		TradeName:            m.TradeName,
		DocumentType:         m.DocumentType,
		DocumentNumber:       m.DocumentNumber,
		ManagersOnlyAccess:   m.ManagersOnlyAccess,
		RequireClientConsent: m.RequireClientConsent,
		ForceUpdate:          m.ForceUpdate,
		CreatedAt:            m.CreatedAt.Format(time.RFC3339),
		UpdatedAt:            m.UpdatedAt.Format(time.RFC3339),
	}
}

func newMachineCreateResponse(m Machine) machineCreateResponse {
	return machineCreateResponse{
		machineResponse: newMachineResponse(m),
		MachineKey:      m.MachineKey,
	}
}

func formatMachineTime(value *time.Time) *string {
	if value == nil {
		return nil
	}
	formatted := value.Format(time.RFC3339)
	return &formatted
}

func optionalAuditUserID(claims *auth.Claims) *string {
	if claims == nil || claims.IsMachine() || claims.UserID == "" {
		return nil
	}
	return &claims.UserID
}

func writeMachineError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, ErrInvalidName), errors.Is(err, ErrGroupRequired), errors.Is(err, ErrSubgroupGroupMismatch), errors.Is(err, ErrInvalidDocument):
		writeJSON(w, http.StatusBadRequest, errorResp{Error: err.Error()})
	case errors.Is(err, ErrGroupNotFound), errors.Is(err, ErrSubgroupNotFound), errors.Is(err, ErrNotFound):
		writeJSON(w, http.StatusNotFound, errorResp{Error: err.Error()})
	default:
		slog.Error("machine handler error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
	}
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		_ = json.NewEncoder(w).Encode(v)
	}
}
