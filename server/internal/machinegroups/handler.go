package machinegroups

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

type Handler struct {
	svc *Service
}

func NewHandler(svc *Service) *Handler {
	return &Handler{svc: svc}
}

func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/", h.ListGroups)
	r.Post("/", h.CreateGroup)
	r.Put("/{id}", h.UpdateGroup)
	r.Delete("/{id}", h.DeleteGroup)
	r.Get("/{id}/subgroups", h.ListSubgroups)
	r.Post("/{id}/subgroups", h.CreateSubgroup)
	return r
}

func (h *Handler) SubgroupRoutes() chi.Router {
	r := chi.NewRouter()
	r.Put("/{id}", h.UpdateSubgroup)
	r.Delete("/{id}", h.DeleteSubgroup)
	return r
}

type nameRequest struct {
	Name string `json:"name"`
}

type errorResp struct {
	Error string `json:"error"`
}

func (h *Handler) ListGroups(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	groups, err := h.svc.ListGroups(r.Context(), claims.CompanyID)
	if err != nil {
		slog.Error("list groups error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, groups)
}

func (h *Handler) CreateGroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req nameRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	group, err := h.svc.CreateGroup(r.Context(), claims.CompanyID, req.Name)
	if err != nil {
		writeGroupError(w, err)
		return
	}

	writeJSON(w, http.StatusCreated, group)
}

func (h *Handler) UpdateGroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req nameRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	group, err := h.svc.UpdateGroup(r.Context(), chi.URLParam(r, "id"), claims.CompanyID, req.Name)
	if err != nil {
		writeGroupError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, group)
}

func (h *Handler) DeleteGroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	if err := h.svc.DeleteGroup(r.Context(), chi.URLParam(r, "id"), claims.CompanyID); err != nil {
		writeGroupError(w, err)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) ListSubgroups(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	subgroups, err := h.svc.ListSubgroups(r.Context(), claims.CompanyID, chi.URLParam(r, "id"))
	if err != nil {
		writeGroupError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, subgroups)
}

func (h *Handler) CreateSubgroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req nameRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	subgroup, err := h.svc.CreateSubgroup(r.Context(), claims.CompanyID, chi.URLParam(r, "id"), req.Name)
	if err != nil {
		writeGroupError(w, err)
		return
	}

	writeJSON(w, http.StatusCreated, subgroup)
}

func (h *Handler) UpdateSubgroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req nameRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	subgroup, err := h.svc.UpdateSubgroup(r.Context(), chi.URLParam(r, "id"), claims.CompanyID, req.Name)
	if err != nil {
		writeGroupError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, subgroup)
}

func (h *Handler) DeleteSubgroup(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	if err := h.svc.DeleteSubgroup(r.Context(), chi.URLParam(r, "id"), claims.CompanyID); err != nil {
		writeGroupError(w, err)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func writeGroupError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, ErrInvalidName):
		writeJSON(w, http.StatusBadRequest, errorResp{Error: err.Error()})
	case errors.Is(err, ErrGroupNotFound), errors.Is(err, ErrSubgroupNotFound):
		writeJSON(w, http.StatusNotFound, errorResp{Error: err.Error()})
	case errors.Is(err, ErrDuplicateName):
		writeJSON(w, http.StatusConflict, errorResp{Error: err.Error()})
	case errors.Is(err, ErrGroupInUse), errors.Is(err, ErrSubgroupInUse):
		writeJSON(w, http.StatusConflict, errorResp{Error: err.Error()})
	default:
		slog.Error("machine groups handler error", "err", err)
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
