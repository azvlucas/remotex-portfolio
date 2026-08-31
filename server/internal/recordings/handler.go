package recordings

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

// Handler expõe os endpoints de gravações de sessão.
type Handler struct {
	svc *Service
}

// NewHandler cria um novo handler.
func NewHandler(svc *Service) *Handler {
	return &Handler{svc: svc}
}

// Routes registra as rotas de recordings (todas protegidas por JWT).
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Post("/upload/{machine_id}", h.Upload)
	r.Put("/confirm/{id}", h.ConfirmUpload)
	r.Get("/", h.List)
	r.Get("/{id}", h.GetByID)
	r.Get("/{id}/download", h.Download)
	r.Get("/session/{session_id}", h.GetBySession)
	r.Get("/machine/{machine_id}", h.ListByMachine)
	r.Delete("/{id}", h.Delete)
	return r
}

// ─── DTOs ───────────────────────────────────────────────────────────────────

type uploadRequest struct {
	FileSize    int64   `json:"file_size"`
	DurationSec int     `json:"duration_sec"`
	SessionID   *string `json:"session_id,omitempty"`
}

type uploadResponse struct {
	ID  string `json:"id"`
	URL string `json:"url"`
}

type downloadResponse struct {
	URL string `json:"url"`
}

type errorResp struct {
	Error string `json:"error"`
}

// ─── POST /recordings/upload/{machine_id} ────────────────────────────────────

func (h *Handler) Upload(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "machine_id")
	if claims.IsMachine() && claims.MachineID != machineID {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "machine access denied"})
		return
	}

	var req uploadRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	if req.FileSize <= 0 {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "file_size is required"})
		return
	}

	rec, url, err := h.svc.GenerateUploadURL(r.Context(), req.SessionID, claims.CompanyID, machineID, req.FileSize, req.DurationSec)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("generate recording upload URL error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, uploadResponse{
		ID:  rec.ID,
		URL: url,
	})
}

// ─── PUT /recordings/confirm/{id} ───────────────────────────────────────────

func (h *Handler) ConfirmUpload(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	err := h.svc.ConfirmUpload(r.Context(), id, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "recording not found"})
		return
	}
	if err != nil {
		slog.Error("confirm recording upload error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

// ─── GET /recordings ────────────────────────────────────────────────────────

func (h *Handler) List(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	limit, offset := parsePagination(r)

	list, err := h.svc.ListByCompany(r.Context(), claims.CompanyID, limit, offset)
	if err != nil {
		slog.Error("list recordings error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, list)
}

// ─── GET /recordings/{id} ───────────────────────────────────────────────────

func (h *Handler) GetByID(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	rec, err := h.svc.GetByID(r.Context(), id, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "recording not found"})
		return
	}
	if err != nil {
		slog.Error("get recording error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, rec)
}

// ─── GET /recordings/{id}/download ──────────────────────────────────────────

func (h *Handler) Download(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	url, err := h.svc.GenerateDownloadURL(r.Context(), id, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "recording not found"})
		return
	}
	if err != nil {
		slog.Error("generate recording download URL error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, downloadResponse{URL: url})
}

// ─── GET /recordings/session/{session_id} ───────────────────────────────────

func (h *Handler) GetBySession(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	sessionID := chi.URLParam(r, "session_id")

	rec, err := h.svc.GetBySessionID(r.Context(), sessionID, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "recording not found"})
		return
	}
	if err != nil {
		slog.Error("get recording by session error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, rec)
}

// ─── GET /recordings/machine/{machine_id} ───────────────────────────────────

func (h *Handler) ListByMachine(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "machine_id")
	limit, offset := parsePagination(r)

	list, err := h.svc.ListByMachine(r.Context(), machineID, claims.CompanyID, limit, offset)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("list recordings by machine error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, list)
}

// ─── DELETE /recordings/{id} ────────────────────────────────────────────────

func (h *Handler) Delete(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	if claims.Role != auth.RoleCompanyManager && claims.Role != auth.RoleSuperAdmin {
		writeJSON(w, http.StatusForbidden, errorResp{Error: "manager access required"})
		return
	}

	id := chi.URLParam(r, "id")

	err := h.svc.Delete(r.Context(), id, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "recording not found"})
		return
	}
	if err != nil {
		slog.Error("delete recording error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

// ─── Helpers ────────────────────────────────────────────────────────────────

func parsePagination(r *http.Request) (limit, offset int) {
	limit = 50
	if v := r.URL.Query().Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 && n <= 200 {
			limit = n
		}
	}
	if v := r.URL.Query().Get("offset"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			offset = n
		}
	}
	return
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		enc := json.NewEncoder(w)
		enc.SetEscapeHTML(false)
		enc.Encode(v)
	}
}
