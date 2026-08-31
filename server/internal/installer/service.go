package installer

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
)

type Config struct {
	InstallerNSIPath  string
	LicensePath       string
	InstallerIconPath string
}

type Service struct {
	cfg      Config
	releases *releases.Service
}

type BootstrapConfig struct {
	APIHost         string `json:"api_host"`
	APIPort         int    `json:"api_port"`
	APIUseTLS       bool   `json:"api_use_tls"`
	SignalingHost   string `json:"signaling_host"`
	SignalingPort   int    `json:"signaling_port"`
	SignalingUseTLS bool   `json:"signaling_use_tls"`
	RelayHost       string `json:"relay_host"`
	RelayPort       int    `json:"relay_port"`
	EnrollmentToken string `json:"enrollment_token"`
}

func ConfigFromEnv() Config {
	return Config{
		InstallerNSIPath:  envOr("INSTALLER_NSI_PATH", "/data/installer/installer.nsi"),
		LicensePath:       envOr("INSTALLER_LICENSE_PATH", "/data/installer/license.txt"),
		InstallerIconPath: envOr("INSTALLER_ICON_PATH", "/data/installer/remotex.ico"),
	}
}

func NewService(cfg Config, releaseSvc *releases.Service) *Service {
	return &Service{cfg: cfg, releases: releaseSvc}
}

func (s *Service) BuildInstaller(bootstrap BootstrapConfig) (string, func(), error) {
	release, err := s.releases.ResolveCurrent()
	if err != nil {
		return "", nil, fmt.Errorf("failed to resolve active release: %w", err)
	}

	if _, err := os.Stat(release.BinaryPath); err != nil {
		return "", nil, fmt.Errorf("host binary not found at %s: %w", release.BinaryPath, err)
	}
	if _, err := os.Stat(s.cfg.InstallerNSIPath); err != nil {
		return "", nil, fmt.Errorf("installer.nsi not found at %s: %w", s.cfg.InstallerNSIPath, err)
	}

	tmpDir, err := os.MkdirTemp("", "remotex-installer-*")
	if err != nil {
		return "", nil, fmt.Errorf("failed to create temp dir: %w", err)
	}

	cleanup := func() {
		if err := os.RemoveAll(tmpDir); err != nil {
			slog.Warn("failed to cleanup installer temp dir", "dir", tmpDir, "err", err)
		}
	}

	distDir := filepath.Join(tmpDir, "dist")
	if err := os.MkdirAll(distDir, 0o755); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to create dist dir: %w", err)
	}

	hostDst := filepath.Join(distDir, filepath.Base(release.BinaryPath))
	if err := copyFile(release.BinaryPath, hostDst); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to copy host binary: %w", err)
	}

	if err := copyReleasePayload(release.Directory, distDir); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to stage release payload: %w", err)
	}

	bootstrapData, err := json.MarshalIndent(bootstrap, "", "  ")
	if err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to marshal bootstrap: %w", err)
	}
	if err := os.WriteFile(filepath.Join(distDir, "bootstrap.json"), bootstrapData, 0o600); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to write bootstrap.json: %w", err)
	}

	nsiDst := filepath.Join(tmpDir, "installer.nsi")
	if err := copyFile(s.cfg.InstallerNSIPath, nsiDst); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("failed to copy installer.nsi: %w", err)
	}

	if s.cfg.LicensePath != "" {
		if _, err := os.Stat(s.cfg.LicensePath); err == nil {
			if err := copyFile(s.cfg.LicensePath, filepath.Join(tmpDir, "license.txt")); err != nil {
				slog.Warn("failed to copy license.txt, continuing without it", "err", err)
			}
		}
	}

	iconMacro := ""
	if s.cfg.InstallerIconPath != "" {
		switch _, err := os.Stat(s.cfg.InstallerIconPath); {
		case err == nil:
			if copyErr := copyFile(s.cfg.InstallerIconPath, filepath.Join(tmpDir, "remotex.ico")); copyErr != nil {
				slog.Warn("failed to copy installer icon, continuing with default icon", "err", copyErr)
			} else {
				iconMacro = "-DREMOTEX_ICON_FILE=remotex.ico"
			}
		case errors.Is(err, os.ErrNotExist):
			slog.Warn("installer icon not found, continuing with default icon", "path", s.cfg.InstallerIconPath)
		default:
			slog.Warn("failed to stat installer icon, continuing with default icon", "path", s.cfg.InstallerIconPath, "err", err)
		}
	}

	outputPath := filepath.Join(tmpDir, "RemoteX_Host_Setup.exe")
	args := []string{
		"-DCRED_MODE=embedded",
		fmt.Sprintf("-DOUTPUT_FILE=%s", outputPath),
		fmt.Sprintf("-DPRODUCT_VERSION=%s", release.Version),
	}
	if iconMacro != "" {
		args = append(args, iconMacro)
	}
	args = append(args, nsiDst)

	slog.Info("running makensis", "args", args)
	cmd := exec.Command("makensis", args...)
	cmd.Dir = tmpDir
	output, err := cmd.CombinedOutput()
	if err != nil {
		slog.Error("makensis failed", "err", err, "output", string(output))
		cleanup()
		return "", nil, fmt.Errorf("makensis failed: %w\nOutput: %s", err, string(output))
	}

	if _, err := os.Stat(outputPath); err != nil {
		cleanup()
		return "", nil, fmt.Errorf("installer output not found after build: %w", err)
	}

	slog.Info("installer built successfully", "path", outputPath, "release_version", release.Version)
	return outputPath, cleanup, nil
}

func copyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0o644)
}

func copyReleasePayload(releaseDir, distDir string) error {
	entries, err := os.ReadDir(releaseDir)
	if err != nil {
		return err
	}

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		if shouldSkipReleasePayloadFile(name) {
			continue
		}

		src := filepath.Join(releaseDir, name)
		dst := filepath.Join(distDir, name)
		if err := copyFile(src, dst); err != nil {
			return err
		}
		slog.Info("release payload file packed successfully", "file", name)
	}

	return nil
}

func shouldSkipReleasePayloadFile(name string) bool {
	if name == "release.json" || name == "remotex_host.exe" {
		return true
	}

	switch filepath.Ext(name) {
	case ".zip", ".md":
		return true
	default:
		return false
	}
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
