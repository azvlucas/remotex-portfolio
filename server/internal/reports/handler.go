package reports

import (
	"encoding/json"
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

// Handler expoe os endpoints de relatorios.
type Handler struct {
	svc *Service
}

// NewHandler cria um novo handler.
func NewHandler(svc *Service) *Handler {
	return &Handler{svc: svc}
}

// Routes registra as rotas de relatorios.
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/dashboard", h.Dashboard)
	return r
}

type errorResp struct {
	Error string `json:"error"`
}

// Dashboard retorna estatisticas agregadas de sessoes da empresa.
func (h *Handler) Dashboard(w http.ResponseWriter, r *http.Request) {
	claims, ok := auth.UserFromContext(r.Context())
	if !ok || claims.CompanyID == "" {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "unauthorized"})
		return
	}

	dateFrom := r.URL.Query().Get("date_from")
	dateTo := r.URL.Query().Get("date_to")
	tz := r.URL.Query().Get("tz")

	data, err := h.svc.GetDashboard(r.Context(), claims.CompanyID, dateFrom, dateTo, tz)
	if err != nil {
		slog.Error("reports dashboard error", "err", err, "company_id", claims.CompanyID)
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "internal error"})
		return
	}

	writeJSON(w, http.StatusOK, data)
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		_ = json.NewEncoder(w).Encode(v)
	}
}
