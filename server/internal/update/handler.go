package update

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
)

type Config struct {
	DownloadURL        string
	ViewerDownloadURL  string
	UpdaterDownloadURL string
}

type Handler struct {
	cfg             Config
	releases        *releases.Service
	viewerReleases  *releases.Service
	updaterReleases *releases.Service
}

type checkResponse struct {
	UpdateAvailable bool   `json:"update_available"`
	LatestVersion   string `json:"latest_version"`
	DownloadURL     string `json:"download_url,omitempty"`
	SignatureHex    string `json:"signature_hex,omitempty"`
	FileSize        int64  `json:"file_size,omitempty"`
	SHA256Hex       string `json:"sha256_hex,omitempty"`
}

func ConfigFromEnv() Config {
	return Config{
		DownloadURL:        envOr("UPDATE_DOWNLOAD_URL", "/update/download/remotex_host.exe"),
		ViewerDownloadURL:  envOr("VIEWER_UPDATE_DOWNLOAD_URL", "/update/viewer/download/remotex_viewer.zip"),
		UpdaterDownloadURL: envOr("UPDATER_UPDATE_DOWNLOAD_URL", "/update/updater/download/remotex_updater.exe"),
	}
}

func NewHandler(cfg Config, releaseSvc *releases.Service, viewerReleaseSvc *releases.Service, updaterReleaseSvc *releases.Service) *Handler {
	return &Handler{cfg: cfg, releases: releaseSvc, viewerReleases: viewerReleaseSvc, updaterReleases: updaterReleaseSvc}
}

func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/check", h.Check)
	r.Get("/download/remotex_host.exe", h.DownloadHostBinary)
	r.Get("/viewer/check", h.ViewerCheck)
	r.Get("/viewer/download/remotex_viewer.zip", h.DownloadViewerPackage)
	r.Get("/updater/check", h.UpdaterCheck)
	r.Get("/updater/download/remotex_updater.exe", h.DownloadUpdaterBinary)
	return r
}

func (h *Handler) Check(w http.ResponseWriter, r *http.Request) {
	currentVersion := r.URL.Query().Get("current_version")
	resp := checkResponse{}

	release, err := h.releases.ResolveCurrent()
	if err == nil {
		resp.LatestVersion = release.Version
		if isNewerVersion(currentVersion, release.Version) && h.cfg.DownloadURL != "" {
			if _, statErr := os.Stat(release.BinaryPath); statErr != nil {
				slog.Warn("update check: binary not found, cannot offer update",
					"version", release.Version,
					"binary_path", release.BinaryPath,
					"err", statErr,
				)
			} else {
				resp.UpdateAvailable = true
				resp.DownloadURL = h.cfg.DownloadURL
				resp.SignatureHex = release.SignatureHex
				resp.FileSize = release.FileSize
				resp.SHA256Hex = releaseSHA256ForResponse(release)
				slog.Info("update available",
					"current_version", currentVersion,
					"latest_version", release.Version,
					"source", release.Source,
					"download_url", h.cfg.DownloadURL,
				)
			}
		}
	} else {
		slog.Warn("update check failed to resolve current release", "err", err, "current_version", currentVersion)
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(resp)
}

func (h *Handler) DownloadHostBinary(w http.ResponseWriter, r *http.Request) {
	release, err := h.releases.ResolveCurrent()
	if err != nil {
		http.NotFound(w, r)
		return
	}

	info, err := os.Stat(release.BinaryPath)
	if err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}

	slog.Info("serving host update binary",
		"version", release.Version,
		"source", release.Source,
		"path", release.BinaryPath,
		"size", info.Size(),
	)

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(info.Size(), 10))
	w.Header().Set("Content-Disposition", `attachment; filename="`+filepath.Base(release.BinaryPath)+`"`)
	http.ServeFile(w, r, release.BinaryPath)
}

func (h *Handler) ViewerCheck(w http.ResponseWriter, r *http.Request) {
	currentVersion := r.URL.Query().Get("current_version")
	resp := checkResponse{}

	if h.viewerReleases == nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(resp)
		return
	}

	release, err := h.viewerReleases.ResolveCurrent()
	if err == nil {
		resp.LatestVersion = release.Version
		if isNewerVersion(currentVersion, release.Version) && h.cfg.ViewerDownloadURL != "" {
			// For the viewer, the download serves package.zip, so return its hash and size.
			pkgInfo, pkgErr := os.Stat(release.PackagePath())
			if pkgErr != nil {
				slog.Warn("viewer package.zip not found, cannot offer update",
					"version", release.Version, "err", pkgErr)
			} else {
				resp.UpdateAvailable = true
				resp.DownloadURL = h.cfg.ViewerDownloadURL
				resp.SignatureHex = release.SignatureHex
				resp.FileSize = pkgInfo.Size()
				if hash, hashErr := release.ComputePackageSHA256(); hashErr == nil {
					resp.SHA256Hex = hash
				}
				slog.Info("viewer update available",
					"current_version", currentVersion,
					"latest_version", release.Version,
					"source", release.Source,
					"download_url", h.cfg.ViewerDownloadURL,
				)
			}
		}
	} else {
		slog.Warn("viewer update check failed to resolve current release", "err", err, "current_version", currentVersion)
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(resp)
}

func (h *Handler) DownloadViewerPackage(w http.ResponseWriter, r *http.Request) {
	if h.viewerReleases == nil {
		http.NotFound(w, r)
		return
	}

	release, err := h.viewerReleases.ResolveCurrent()
	if err != nil {
		http.NotFound(w, r)
		return
	}

	// Serve the saved package.zip (contains exe + DLLs + Qt plugins).
	pkgPath := release.PackagePath()
	info, err := os.Stat(pkgPath)
	if err != nil || info.IsDir() {
		slog.Error("viewer package.zip not found, release must be re-uploaded",
			"version", release.Version,
			"path", pkgPath,
		)
		http.NotFound(w, r)
		return
	}

	slog.Info("serving viewer update package",
		"version", release.Version,
		"source", release.Source,
		"path", pkgPath,
		"size", info.Size(),
	)

	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Length", strconv.FormatInt(info.Size(), 10))
	w.Header().Set("Content-Disposition", `attachment; filename="remotex_viewer.zip"`)
	http.ServeFile(w, r, pkgPath)
}

func (h *Handler) UpdaterCheck(w http.ResponseWriter, r *http.Request) {
	currentVersion := r.URL.Query().Get("current_version")
	resp := checkResponse{}

	if h.updaterReleases == nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(resp)
		return
	}

	release, err := h.updaterReleases.ResolveCurrent()
	if err == nil {
		resp.LatestVersion = release.Version
		if isNewerVersion(currentVersion, release.Version) && h.cfg.UpdaterDownloadURL != "" {
			if _, statErr := os.Stat(release.BinaryPath); statErr != nil {
				slog.Warn("updater check: binary not found, cannot offer update",
					"version", release.Version,
					"binary_path", release.BinaryPath,
					"err", statErr,
				)
			} else {
				resp.UpdateAvailable = true
				resp.DownloadURL = h.cfg.UpdaterDownloadURL
				resp.SignatureHex = release.SignatureHex
				resp.FileSize = release.FileSize
				resp.SHA256Hex = releaseSHA256ForResponse(release)
				slog.Info("updater update available",
					"current_version", currentVersion,
					"latest_version", release.Version,
					"source", release.Source,
					"download_url", h.cfg.UpdaterDownloadURL,
				)
			}
		}
	} else {
		slog.Warn("updater update check failed to resolve current release", "err", err, "current_version", currentVersion)
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(resp)
}

func (h *Handler) DownloadUpdaterBinary(w http.ResponseWriter, r *http.Request) {
	if h.updaterReleases == nil {
		http.NotFound(w, r)
		return
	}

	release, err := h.updaterReleases.ResolveCurrent()
	if err != nil {
		http.NotFound(w, r)
		return
	}

	info, err := os.Stat(release.BinaryPath)
	if err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}

	slog.Info("serving updater binary",
		"version", release.Version,
		"source", release.Source,
		"path", release.BinaryPath,
		"size", info.Size(),
	)

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(info.Size(), 10))
	w.Header().Set("Content-Disposition", `attachment; filename="`+filepath.Base(release.BinaryPath)+`"`)
	http.ServeFile(w, r, release.BinaryPath)
}

func releaseSHA256ForResponse(release releases.ResolvedRelease) string {
	hash, err := release.ComputeSHA256()
	if err == nil {
		return hash
	}

	if release.SHA256Hex != "" {
		slog.Warn("ComputeSHA256 failed, falling back to manifest",
			"err", err,
			"version", release.Version,
			"binary_path", release.BinaryPath,
		)
		return release.SHA256Hex
	}

	slog.Warn("ComputeSHA256 failed and manifest sha256_hex is empty",
		"err", err,
		"version", release.Version,
		"binary_path", release.BinaryPath,
	)
	return ""
}

// isNewerVersion returns true if latest is strictly newer than current.
// Versions are compared as dot-separated integers (e.g. "0.7.2").
// Returns false if versions are equal or current is already newer.
func isNewerVersion(current, latest string) bool {
	parseParts := func(v string) []int {
		var parts []int
		for _, s := range strings.Split(v, ".") {
			n, err := strconv.Atoi(s)
			if err != nil {
				return nil
			}
			parts = append(parts, n)
		}
		return parts
	}

	cur := parseParts(current)
	lat := parseParts(latest)
	if len(cur) == 0 || len(lat) == 0 {
		// Unparseable — fall back to string inequality.
		return current != latest
	}

	maxLen := len(cur)
	if len(lat) > maxLen {
		maxLen = len(lat)
	}
	for i := 0; i < maxLen; i++ {
		c, l := 0, 0
		if i < len(cur) {
			c = cur[i]
		}
		if i < len(lat) {
			l = lat[i]
		}
		if l > c {
			return true
		}
		if l < c {
			return false
		}
	}
	return false // equal
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
