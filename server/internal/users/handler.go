package users

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
	r.Get("/", h.List)
	r.Post("/", h.Create)
	r.Put("/{id}/activate", h.Activate)
	r.Put("/{id}/deactivate", h.Deactivate)
	r.Put("/{id}/reset-password", h.ResetPassword)
	return r
}

type createRequest struct {
	Name     string `json:"name"`
	Email    string `json:"email"`
	Password string `json:"password"`
}

type resetPasswordRequest struct {
	Password string `json:"password"`
}

type errorResp struct {
	Error string `json:"error"`
}

func (h *Handler) List(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	users, err := h.svc.ListCompanyUsers(r.Context(), claims.CompanyID)
	if err != nil {
		slog.Error("list company users error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, users)
}

func (h *Handler) Create(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	var req createRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}

	if req.Name == "" || req.Email == "" || req.Password == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "name, email and password are required"})
		return
	}
	if len(req.Password) < 8 {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "password must be at least 8 characters"})
		return
	}

	user, err := h.svc.CreateCompanyUser(r.Context(), claims.CompanyID, req.Name, req.Email, req.Password)
	if err != nil {
		if errors.Is(err, ErrDuplicateEmail) {
			writeJSON(w, http.StatusConflict, errorResp{Error: "email already registered"})
			return
		}
		slog.Error("create company user error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusCreated, user)
}

func (h *Handler) Activate(w http.ResponseWriter, r *http.Request) {
	h.setActive(w, r, true)
}

func (h *Handler) Deactivate(w http.ResponseWriter, r *http.Request) {
	h.setActive(w, r, false)
}

func (h *Handler) setActive(w http.ResponseWriter, r *http.Request, active bool) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	userID := chi.URLParam(r, "id")

	err := h.svc.SetUserActive(r.Context(), userID, claims.CompanyID, active)
	if errors.Is(err, ErrUserNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "user not found"})
		return
	}
	if err != nil {
		slog.Error("toggle user active error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) ResetPassword(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	userID := chi.URLParam(r, "id")

	var req resetPasswordRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid request body"})
		return
	}
	if len(req.Password) < 8 {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "password must be at least 8 characters"})
		return
	}

	err := h.svc.ResetUserPassword(r.Context(), userID, claims.CompanyID, req.Password)
	if errors.Is(err, ErrUserNotFound) {
		writeJSON(w, http.StatusNotFound, errorResp{Error: "user not found"})
		return
	}
	if err != nil {
		slog.Error("reset user password error", "err", err)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		json.NewEncoder(w).Encode(v)
	}
}
