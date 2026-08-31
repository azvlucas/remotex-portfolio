package backup

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

// Handler expõe os endpoints de gestão de backup.
type Handler struct {
	svc *Service
}

// NewHandler cria um novo handler.
func NewHandler(svc *Service) *Handler {
	return &Handler{svc: svc}
}

// Routes registra as rotas de backup (todas protegidas por JWT).
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/config/{machine_id}", h.GetConfig)
	r.Put("/config/{machine_id}", h.UpdateConfig)
	r.Post("/upload/{machine_id}", h.Upload)
	r.Get("/snapshots/{machine_id}", h.ListSnapshots)
	r.Delete("/snapshots/{id}", h.DeleteSnapshot)
	return r
}

// ─── DTOs ───────────────────────────────────────────────────────────────────

type configRequest struct {
	Folders        []string `json:"folders"`
	IntervalHours  int      `json:"interval_hours"`
	RetentionCount int      `json:"retention_count"`
	Enabled        *bool    `json:"enabled,omitempty"`
}

type uploadRequest struct {
	SizeBytes int64 `json:"size_bytes"`
	FileCount int   `json:"file_count"`
}

type uploadResponse struct {
	URL        string `json:"url"`
	StorageKey string `json:"storage_key"`
}

type errorResp struct {
	Error string `json:"error"`
}

// ─── GET /backup/config/:machine_id ─────────────────────────────────────────

func (h *Handler) GetConfig(w http.ResponseWriter, r *http.Request) {
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

	cfg, err := h.svc.GetConfig(r.Context(), machineID, claims.CompanyID)
	if errors.Is(err, pgx.ErrNoRows) {
		// Retorna configuração padrão se não existe.
		writeJSON(w, http.StatusOK, BackupConfig{
			MachineID:      machineID,
			Folders:        []string{},
			IntervalHours:  2,
			RetentionCount: 7,
			Enabled:        false,
		})
		return
	}
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
			return
		}
		slog.Error("get backup config error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, cfg)
}

// ─── PUT /backup/config/:machine_id ─────────────────────────────────────────

func (h *Handler) UpdateConfig(w http.ResponseWriter, r *http.Request) {
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

	var req configRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	enabled := true
	if req.Enabled != nil {
		enabled = *req.Enabled
	}

	cfg, err := h.svc.UpsertConfig(r.Context(), machineID, claims.CompanyID, req.Folders, req.IntervalHours, req.RetentionCount, enabled)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("update backup config error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("backup config updated", "machine_id", machineID)
	writeJSON(w, http.StatusOK, cfg)
}

// ─── POST /backup/upload/:machine_id ────────────────────────────────────────

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

	if req.SizeBytes <= 0 {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "size_bytes is required"})
		return
	}

	url, storageKey, err := h.svc.GenerateUploadURL(r.Context(), machineID, claims.CompanyID, req.SizeBytes, req.FileCount)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("generate upload URL error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	// Aplica retenção após registrar o novo snapshot.
	if err := h.svc.EnforceRetention(r.Context(), machineID, claims.CompanyID); err != nil {
		slog.Warn("retention enforcement error", "machine_id", machineID, "err", err)
	}

	writeJSON(w, http.StatusOK, uploadResponse{
		URL:        url,
		StorageKey: storageKey,
	})
}

// ─── GET /backup/snapshots/:machine_id ──────────────────────────────────────

func (h *Handler) ListSnapshots(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	machineID := chi.URLParam(r, "machine_id")

	snapshots, err := h.svc.ListSnapshots(r.Context(), machineID, claims.CompanyID)
	if errors.Is(err, ErrNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "machine not found"})
		return
	}
	if err != nil {
		slog.Error("list snapshots error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, snapshots)
}

// ─── DELETE /backup/snapshots/:id ───────────────────────────────────────────

func (h *Handler) DeleteSnapshot(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" || claims.IsMachine() {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	id := chi.URLParam(r, "id")

	err := h.svc.DeleteSnapshot(r.Context(), id, claims.CompanyID)
	if err == ErrNotFound {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "snapshot not found"})
		return
	}
	if err != nil {
		slog.Error("delete snapshot error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	slog.Info("snapshot deleted", "id", id)
	w.WriteHeader(http.StatusNoContent)
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
