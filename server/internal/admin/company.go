package admin

import (
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/audit"
	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

func (h *Handler) CompanyDetail(w http.ResponseWriter, r *http.Request) {
	detail, err := h.svc.GetCompanyDetail(r.Context(), chi.URLParam(r, "id"))
	if err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			http.Redirect(w, r, "/admin?error="+url.QueryEscape("empresa nÃ£o encontrada"), http.StatusSeeOther)
			return
		}
		slog.Error("admin get company detail failed", "err", err)
		http.Redirect(w, r, "/admin?error="+url.QueryEscape("falha ao carregar empresa"), http.StatusSeeOther)
		return
	}

	renderTemplate(w, http.StatusOK, companyDetailTemplate, companyPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Detail:    *detail,
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) MachinesPage(w http.ResponseWriter, r *http.Request) {
	query := strings.TrimSpace(r.URL.Query().Get("q"))
	machines := []MachineSummary{}

	if query != "" {
		var err error
		machines, err = h.svc.SearchMachines(r.Context(), query, 100)
		if err != nil {
			slog.Error("admin search machines failed", "err", err, "query", query)
			if wantsJSON(r) {
				writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "falha ao pesquisar mÃ¡quinas"})
				return
			}
			renderTemplate(w, http.StatusInternalServerError, machinesTemplate, machinesPageData{
				Error:     "falha ao pesquisar mÃ¡quinas",
				Query:     query,
				CSRFToken: csrfFromCtx(r),
			})
			return
		}
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, machines)
		return
	}

	renderTemplate(w, http.StatusOK, machinesTemplate, machinesPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Query:     query,
		Machines:  machines,
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) CompanyLogs(w http.ResponseWriter, r *http.Request) {
	companyID := chi.URLParam(r, "id")

	detail, err := h.svc.GetCompanyDetail(r.Context(), companyID)
	if err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			http.Redirect(w, r, "/admin?error="+url.QueryEscape("empresa nÃ£o encontrada"), http.StatusSeeOther)
			return
		}
		slog.Error("admin company logs failed", "err", err)
		http.Redirect(w, r, "/admin?error="+url.QueryEscape("falha ao carregar empresa"), http.StatusSeeOther)
		return
	}

	filter := audit.CompanyEventsFilter{
		EventType: r.URL.Query().Get("event_type"),
		MachineID: r.URL.Query().Get("machine_id"),
		DateFrom:  r.URL.Query().Get("date_from"),
		DateTo:    r.URL.Query().Get("date_to"),
		Search:    r.URL.Query().Get("search"),
	}

	pageSize := 50
	page := 1
	if p := r.URL.Query().Get("page"); p != "" {
		v := 0
		if _, scanErr := fmt.Sscanf(p, "%d", &v); scanErr == nil && v > 0 {
			page = v
		}
	}
	offset := (page - 1) * pageSize

	queryErr := r.URL.Query().Get("error")
	events, total, err := h.audit.ListCompanyEvents(r.Context(), companyID, filter, pageSize, offset)
	if err != nil {
		slog.Error("admin list company events failed", "err", err, "company_id", companyID)
		events = []audit.Event{}
		queryErr = "Falha ao carregar eventos. Tente novamente."
	}

	totalPages := (total + pageSize - 1) / pageSize
	if totalPages < 1 {
		totalPages = 1
	}

	prevPage := page - 1
	if prevPage < 1 {
		prevPage = 1
	}
	nextPage := page + 1
	if nextPage > totalPages {
		nextPage = totalPages
	}

	renderTemplate(w, http.StatusOK, companyLogsTemplate, companyLogsPageData{
		Error:      queryErr,
		Company:    detail.Company,
		Events:     events,
		Filter:     filter,
		Total:      total,
		Page:       page,
		PageSize:   pageSize,
		TotalPages: totalPages,
		PrevPage:   prevPage,
		NextPage:   nextPage,
		CSRFToken:  csrfFromCtx(r),
	})
}

func (h *Handler) DetachMachine(w http.ResponseWriter, r *http.Request) {
	machineID := chi.URLParam(r, "id")
	query := strings.TrimSpace(readParam(r, "q"))
	redirectTarget := "/admin/machines"
	if query != "" {
		redirectTarget += "?q=" + url.QueryEscape(query)
	}

	if err := h.svc.DetachMachine(r.Context(), machineID); err != nil {
		if errors.Is(err, ErrMachineNotFound) {
			h.redirectOrJSONError(w, r, redirectTarget, http.StatusNotFound, "mÃ¡quina nÃ£o encontrada")
			return
		}
		slog.Error("admin detach machine failed", "err", err, "machine_id", machineID)
		h.redirectOrJSONError(w, r, redirectTarget, http.StatusInternalServerError, "falha ao desvincular mÃ¡quina")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
		return
	}

	separator := "?"
	if strings.Contains(redirectTarget, "?") {
		separator = "&"
	}
	http.Redirect(w, r, redirectTarget+separator+"message="+url.QueryEscape("mÃ¡quina desvinculada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) UpdateBlockDays(w http.ResponseWriter, r *http.Request) {
	companyID := chi.URLParam(r, "id")
	raw := strings.TrimSpace(readParam(r, "block_after_days"))

	var days *int
	if raw != "" {
		v := 0
		if _, err := fmt.Sscanf(raw, "%d", &v); err != nil || v < 0 {
			h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusBadRequest, "valor invÃ¡lido para dias de bloqueio")
			return
		}
		days = &v
	}

	if err := h.svc.UpdateCompanyBlockDays(r.Context(), companyID, days); err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			h.redirectOrJSONError(w, r, "/admin", http.StatusNotFound, "empresa nÃ£o encontrada")
			return
		}
		slog.Error("admin update block days failed", "err", err, "company_id", companyID)
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusInternalServerError, "falha ao atualizar dias de bloqueio")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "block_after_days": days})
		return
	}
	http.Redirect(w, r, "/admin/companies/"+companyID+"?message="+url.QueryEscape("dias de bloqueio atualizados"), http.StatusSeeOther)
}

func parseOptionalUnlimitedLimit(raw string) (*int, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return nil, nil
	}

	v, err := strconv.Atoi(raw)
	if err != nil || v < 0 {
		return nil, errors.New("invalid limit")
	}
	if v == 0 {
		return nil, nil
	}
	return &v, nil
}

func (h *Handler) UpdateConcurrentLimit(w http.ResponseWriter, r *http.Request) {
	companyID := chi.URLParam(r, "id")
	raw := strings.TrimSpace(readParam(r, "concurrent_session_limit"))

	limit, err := parseOptionalUnlimitedLimit(raw)
	if err != nil {
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusBadRequest, "valor invÃ¡lido para limite de sessÃµes")
		return
	}

	if err := h.svc.UpdateConcurrentSessionLimit(r.Context(), companyID, limit); err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			h.redirectOrJSONError(w, r, "/admin", http.StatusNotFound, "empresa nÃ£o encontrada")
			return
		}
		slog.Error("admin update concurrent limit failed", "err", err, "company_id", companyID)
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusInternalServerError, "falha ao atualizar limite de sessÃµes")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "concurrent_session_limit": limit})
		return
	}
	http.Redirect(w, r, "/admin/companies/"+companyID+"?message="+url.QueryEscape("limite de sessÃµes atualizado"), http.StatusSeeOther)
}

func (h *Handler) UpdateMachineLimit(w http.ResponseWriter, r *http.Request) {
	companyID := chi.URLParam(r, "id")
	raw := strings.TrimSpace(readParam(r, "machine_limit"))

	limit, err := parseOptionalUnlimitedLimit(raw)
	if err != nil {
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusBadRequest, "valor invalido para limite de maquinas")
		return
	}

	if err := h.svc.UpdateMachineLimit(r.Context(), companyID, limit); err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			h.redirectOrJSONError(w, r, "/admin", http.StatusNotFound, "empresa nÃ£o encontrada")
			return
		}
		slog.Error("admin update machine limit failed", "err", err, "company_id", companyID)
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusInternalServerError, "falha ao atualizar limite de maquinas")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "machine_limit": limit})
		return
	}
	http.Redirect(w, r, "/admin/companies/"+companyID+"?message="+url.QueryEscape("limite de maquinas atualizado"), http.StatusSeeOther)
}

func (h *Handler) CreateCompany(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimSpace(readParam(r, "name"))
	if name == "" {
		h.redirectOrJSONError(w, r, "/admin", http.StatusBadRequest, "nome da empresa Ã© obrigatÃ³rio")
		return
	}

	company, err := h.svc.CreateCompany(r.Context(), name)
	if err != nil {
		if errors.Is(err, ErrDuplicateName) {
			h.redirectOrJSONError(w, r, "/admin", http.StatusConflict, "empresa jÃ¡ cadastrada")
			return
		}
		slog.Error("admin create company failed", "err", err)
		h.redirectOrJSONError(w, r, "/admin", http.StatusInternalServerError, "falha ao criar empresa")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusCreated, company)
		return
	}
	http.Redirect(w, r, "/admin?message="+url.QueryEscape("empresa criada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) ActivateCompany(w http.ResponseWriter, r *http.Request) {
	h.setCompanyActive(w, r, true)
}

func (h *Handler) DeactivateCompany(w http.ResponseWriter, r *http.Request) {
	h.setCompanyActive(w, r, false)
}

func (h *Handler) CreateCompanyUser(w http.ResponseWriter, r *http.Request) {
	companyID := chi.URLParam(r, "id")
	name := strings.TrimSpace(readParam(r, "name"))
	email := strings.TrimSpace(strings.ToLower(readParam(r, "email")))
	password := readParam(r, "password")
	roleStr := strings.TrimSpace(readParam(r, "role"))
	if name == "" || email == "" || password == "" {
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusBadRequest, "nome, e-mail e senha sÃ£o obrigatÃ³rios")
		return
	}
	if len(password) < 8 {
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusBadRequest, "a senha deve ter pelo menos 8 caracteres")
		return
	}

	role := auth.RoleCompanyUser
	if roleStr == string(auth.RoleCompanyManager) {
		role = auth.RoleCompanyManager
	}

	user, err := h.svc.CreateCompanyUserWithRole(r.Context(), companyID, name, email, password, role)
	if err != nil {
		switch {
		case errors.Is(err, ErrDuplicateEmail):
			h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusConflict, "e-mail jÃ¡ cadastrado")
		case errors.Is(err, ErrCompanyNotFound):
			h.redirectOrJSONError(w, r, "/admin", http.StatusNotFound, "empresa nÃ£o encontrada")
		default:
			slog.Error("admin create company user failed", "err", err)
			h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusInternalServerError, "falha ao criar usuÃ¡rio")
		}
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusCreated, user)
		return
	}
	http.Redirect(w, r, "/admin/companies/"+companyID+"?message="+url.QueryEscape("usuÃ¡rio criado com sucesso"), http.StatusSeeOther)
}

func (h *Handler) UpdateUserRole(w http.ResponseWriter, r *http.Request) {
	userID := chi.URLParam(r, "id")
	companyID := readParam(r, "company_id")
	roleStr := strings.TrimSpace(readParam(r, "role"))

	role := auth.Role(roleStr)
	if role != auth.RoleCompanyUser && role != auth.RoleCompanyManager {
		target := "/admin"
		if companyID != "" {
			target = "/admin/companies/" + companyID
		}
		h.redirectOrJSONError(w, r, target, http.StatusBadRequest, "cargo invÃ¡lido")
		return
	}

	if err := h.svc.UpdateUserRole(r.Context(), userID, role); err != nil {
		target := "/admin"
		if companyID != "" {
			target = "/admin/companies/" + companyID
		}
		if errors.Is(err, ErrUserNotFound) {
			h.redirectOrJSONError(w, r, target, http.StatusNotFound, "usuÃ¡rio nÃ£o encontrado")
			return
		}
		slog.Error("admin update user role failed", "err", err, "user_id", userID)
		h.redirectOrJSONError(w, r, target, http.StatusInternalServerError, "falha ao atualizar cargo")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "role": roleStr})
		return
	}
	target := "/admin"
	if companyID != "" {
		target = "/admin/companies/" + companyID
	}
	http.Redirect(w, r, target+"?message="+url.QueryEscape("cargo atualizado"), http.StatusSeeOther)
}

func (h *Handler) ActivateUser(w http.ResponseWriter, r *http.Request) {
	h.setUserActive(w, r, true)
}

func (h *Handler) DeactivateUser(w http.ResponseWriter, r *http.Request) {
	h.setUserActive(w, r, false)
}

func (h *Handler) setCompanyActive(w http.ResponseWriter, r *http.Request, active bool) {
	companyID := chi.URLParam(r, "id")
	if err := h.svc.SetCompanyActive(r.Context(), companyID, active); err != nil {
		if errors.Is(err, ErrCompanyNotFound) {
			h.redirectOrJSONError(w, r, "/admin", http.StatusNotFound, "empresa nÃ£o encontrada")
			return
		}
		slog.Error("admin toggle company failed", "err", err, "company_id", companyID, "active", active)
		h.redirectOrJSONError(w, r, "/admin/companies/"+companyID, http.StatusInternalServerError, "falha ao atualizar empresa")
		return
	}

	status := "empresa ativada"
	if !active {
		status = "empresa bloqueada"
	}
	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "active": active})
		return
	}
	http.Redirect(w, r, "/admin/companies/"+companyID+"?message="+url.QueryEscape(status), http.StatusSeeOther)
}

func (h *Handler) setUserActive(w http.ResponseWriter, r *http.Request, active bool) {
	userID := chi.URLParam(r, "id")
	companyID := readParam(r, "company_id")
	if err := h.svc.SetUserActive(r.Context(), userID, active); err != nil {
		target := "/admin"
		if companyID != "" {
			target = "/admin/companies/" + companyID
		}
		if errors.Is(err, ErrUserNotFound) {
			h.redirectOrJSONError(w, r, target, http.StatusNotFound, "usuÃ¡rio nÃ£o encontrado")
			return
		}
		slog.Error("admin toggle user failed", "err", err, "user_id", userID, "active", active)
		h.redirectOrJSONError(w, r, target, http.StatusInternalServerError, "falha ao atualizar usuÃ¡rio")
		return
	}

	status := "usuÃ¡rio ativado"
	if !active {
		status = "usuÃ¡rio bloqueado"
	}
	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "active": active})
		return
	}
	target := "/admin"
	if companyID != "" {
		target = "/admin/companies/" + companyID
	}
	http.Redirect(w, r, target+"?message="+url.QueryEscape(status), http.StatusSeeOther)
}
