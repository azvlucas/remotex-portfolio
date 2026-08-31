package admin

import (
	"errors"
	"log/slog"
	"net/http"
	"net/url"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
)

func (h *Handler) ReleasesPage(w http.ResponseWriter, r *http.Request) {
	published, err := h.releases.ListPublished()
	if err != nil {
		slog.Error("admin list releases failed", "err", err)
		renderTemplate(w, http.StatusInternalServerError, releasesTemplate, releasesPageData{
			Error:     "falha ao carregar releases",
			CSRFToken: csrfFromCtx(r),
		})
		return
	}

	var current *releases.ResolvedRelease
	if release, err := h.releases.ResolveCurrent(); err == nil {
		current = &release
	}

	renderTemplate(w, http.StatusOK, releasesTemplate, releasesPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Current:   current,
		Releases:  published,
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) UploadRelease(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, releaseUploadMaxBytes)
	if err := r.ParseMultipartForm(releaseUploadMaxBytes); err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases", http.StatusBadRequest, "falha ao ler upload da release")
		return
	}

	file, header, err := r.FormFile("package")
	if err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases", http.StatusBadRequest, "arquivo .zip da release Ã© obrigatÃ³rio")
		return
	}
	defer file.Close()

	release, err := h.releases.PublishPackage(header.Filename, file)
	if err != nil {
		status := http.StatusBadRequest
		message := err.Error()
		switch {
		case errors.Is(err, releases.ErrReleaseVersionConflict):
			status = http.StatusConflict
		case errors.Is(err, releases.ErrInvalidPackage):
			status = http.StatusBadRequest
		default:
			status = http.StatusInternalServerError
			message = "falha ao publicar release"
			slog.Error("admin publish release failed", "err", err)
		}
		h.redirectOrJSONError(w, r, "/admin/releases", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusCreated, release)
		return
	}
	http.Redirect(w, r, "/admin/releases?message="+url.QueryEscape("release "+release.Version+" publicada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) ActivateRelease(w http.ResponseWriter, r *http.Request) {
	version := chi.URLParam(r, "version")
	release, err := h.releases.Activate(version)
	if err != nil {
		status := http.StatusInternalServerError
		switch {
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
		default:
			slog.Error("admin activate release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases", status, "falha ao ativar release")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, release)
		return
	}
	http.Redirect(w, r, "/admin/releases?message="+url.QueryEscape("release "+release.Version+" ativada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) DeleteRelease(w http.ResponseWriter, r *http.Request) {
	version := chi.URLParam(r, "version")
	err := h.releases.Delete(version)
	if err != nil {
		status := http.StatusInternalServerError
		message := "falha ao excluir release"
		switch {
		case errors.Is(err, releases.ErrReleaseIsActive):
			status = http.StatusBadRequest
			message = "nÃ£o Ã© possÃ­vel excluir a release ativa"
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
			message = "release nÃ£o encontrada"
		default:
			slog.Error("admin delete release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "version": version})
		return
	}
	http.Redirect(w, r, "/admin/releases?message="+url.QueryEscape("release "+version+" excluÃ­da com sucesso"), http.StatusSeeOther)
}

// --- Viewer Release handlers ---

func (h *Handler) ViewerReleasesPage(w http.ResponseWriter, r *http.Request) {
	published, err := h.viewerReleases.ListPublished()
	if err != nil {
		slog.Error("admin list viewer releases failed", "err", err)
		renderTemplate(w, http.StatusInternalServerError, viewerReleasesTemplate, releasesPageData{
			Error:     "falha ao carregar releases do viewer",
			CSRFToken: csrfFromCtx(r),
		})
		return
	}

	var current *releases.ResolvedRelease
	if release, err := h.viewerReleases.ResolveCurrent(); err == nil {
		current = &release
	}

	renderTemplate(w, http.StatusOK, viewerReleasesTemplate, releasesPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Current:   current,
		Releases:  published,
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) UploadViewerRelease(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, releaseUploadMaxBytes)
	if err := r.ParseMultipartForm(releaseUploadMaxBytes); err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases/viewer", http.StatusBadRequest, "falha ao ler upload da release")
		return
	}

	file, header, err := r.FormFile("package")
	if err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases/viewer", http.StatusBadRequest, "arquivo .zip da release Ã© obrigatÃ³rio")
		return
	}
	defer file.Close()

	release, err := h.viewerReleases.PublishPackage(header.Filename, file)
	if err != nil {
		status := http.StatusBadRequest
		message := err.Error()
		switch {
		case errors.Is(err, releases.ErrReleaseVersionConflict):
			status = http.StatusConflict
		case errors.Is(err, releases.ErrInvalidPackage):
			status = http.StatusBadRequest
		default:
			status = http.StatusInternalServerError
			message = "falha ao publicar release do viewer"
			slog.Error("admin publish viewer release failed", "err", err)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/viewer", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusCreated, release)
		return
	}
	http.Redirect(w, r, "/admin/releases/viewer?message="+url.QueryEscape("release "+release.Version+" publicada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) ActivateViewerRelease(w http.ResponseWriter, r *http.Request) {
	version := chi.URLParam(r, "version")
	release, err := h.viewerReleases.Activate(version)
	if err != nil {
		status := http.StatusInternalServerError
		switch {
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
		default:
			slog.Error("admin activate viewer release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/viewer", status, "falha ao ativar release do viewer")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, release)
		return
	}
	http.Redirect(w, r, "/admin/releases/viewer?message="+url.QueryEscape("release "+release.Version+" ativada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) DeleteViewerRelease(w http.ResponseWriter, r *http.Request) {
	version := chi.URLParam(r, "version")
	err := h.viewerReleases.Delete(version)
	if err != nil {
		status := http.StatusInternalServerError
		message := "falha ao excluir release do viewer"
		switch {
		case errors.Is(err, releases.ErrReleaseIsActive):
			status = http.StatusBadRequest
			message = "nÃ£o Ã© possÃ­vel excluir a release ativa"
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
			message = "release nÃ£o encontrada"
		default:
			slog.Error("admin delete viewer release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/viewer", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "version": version})
		return
	}
	http.Redirect(w, r, "/admin/releases/viewer?message="+url.QueryEscape("release "+version+" excluÃ­da com sucesso"), http.StatusSeeOther)
}

// --- Updater Releases handlers ---

func (h *Handler) UpdaterReleasesPage(w http.ResponseWriter, r *http.Request) {
	if h.updaterReleases == nil {
		renderTemplate(w, http.StatusOK, updaterReleasesTemplate, releasesPageData{
			Error:     "updater releases not configured",
			CSRFToken: csrfFromCtx(r),
		})
		return
	}

	published, err := h.updaterReleases.ListPublished()
	if err != nil {
		slog.Error("admin list updater releases failed", "err", err)
		renderTemplate(w, http.StatusInternalServerError, updaterReleasesTemplate, releasesPageData{
			Error:     "falha ao carregar releases do updater",
			CSRFToken: csrfFromCtx(r),
		})
		return
	}

	var current *releases.ResolvedRelease
	if release, err := h.updaterReleases.ResolveCurrent(); err == nil {
		current = &release
	}

	renderTemplate(w, http.StatusOK, updaterReleasesTemplate, releasesPageData{
		Error:     r.URL.Query().Get("error"),
		Message:   r.URL.Query().Get("message"),
		Current:   current,
		Releases:  published,
		CSRFToken: csrfFromCtx(r),
	})
}

func (h *Handler) UploadUpdaterRelease(w http.ResponseWriter, r *http.Request) {
	if h.updaterReleases == nil {
		h.redirectOrJSONError(w, r, "/admin/releases/updater", http.StatusInternalServerError, "updater releases not configured")
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, releaseUploadMaxBytes)
	if err := r.ParseMultipartForm(releaseUploadMaxBytes); err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases/updater", http.StatusBadRequest, "falha ao ler upload da release")
		return
	}

	file, header, err := r.FormFile("package")
	if err != nil {
		h.redirectOrJSONError(w, r, "/admin/releases/updater", http.StatusBadRequest, "arquivo .zip da release Ã© obrigatÃ³rio")
		return
	}
	defer file.Close()

	release, err := h.updaterReleases.PublishPackage(header.Filename, file)
	if err != nil {
		status := http.StatusBadRequest
		message := err.Error()
		switch {
		case errors.Is(err, releases.ErrReleaseVersionConflict):
			status = http.StatusConflict
		case errors.Is(err, releases.ErrInvalidPackage):
			status = http.StatusBadRequest
		default:
			status = http.StatusInternalServerError
			message = "falha ao publicar release do updater"
			slog.Error("admin publish updater release failed", "err", err)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/updater", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusCreated, release)
		return
	}
	http.Redirect(w, r, "/admin/releases/updater?message="+url.QueryEscape("release "+release.Version+" publicada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) ActivateUpdaterRelease(w http.ResponseWriter, r *http.Request) {
	if h.updaterReleases == nil {
		h.redirectOrJSONError(w, r, "/admin/releases/updater", http.StatusInternalServerError, "updater releases not configured")
		return
	}

	version := chi.URLParam(r, "version")
	release, err := h.updaterReleases.Activate(version)
	if err != nil {
		status := http.StatusInternalServerError
		switch {
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
		default:
			slog.Error("admin activate updater release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/updater", status, "falha ao ativar release do updater")
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, release)
		return
	}
	http.Redirect(w, r, "/admin/releases/updater?message="+url.QueryEscape("release "+release.Version+" ativada com sucesso"), http.StatusSeeOther)
}

func (h *Handler) DeleteUpdaterRelease(w http.ResponseWriter, r *http.Request) {
	if h.updaterReleases == nil {
		h.redirectOrJSONError(w, r, "/admin/releases/updater", http.StatusInternalServerError, "updater releases not configured")
		return
	}

	version := chi.URLParam(r, "version")
	err := h.updaterReleases.Delete(version)
	if err != nil {
		status := http.StatusInternalServerError
		message := "falha ao excluir release do updater"
		switch {
		case errors.Is(err, releases.ErrReleaseIsActive):
			status = http.StatusBadRequest
			message = "nÃ£o Ã© possÃ­vel excluir a release ativa"
		case errors.Is(err, releases.ErrReleaseNotFound):
			status = http.StatusNotFound
			message = "release nÃ£o encontrada"
		default:
			slog.Error("admin delete updater release failed", "err", err, "version", version)
		}
		h.redirectOrJSONError(w, r, "/admin/releases/updater", status, message)
		return
	}

	if wantsJSON(r) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "version": version})
		return
	}
	http.Redirect(w, r, "/admin/releases/updater?message="+url.QueryEscape("release "+version+" excluÃ­da com sucesso"), http.StatusSeeOther)
}

// --- Company Logs handler ---
