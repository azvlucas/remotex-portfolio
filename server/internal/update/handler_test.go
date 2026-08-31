package update

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/azvlucas/remotex-portfolio/server/internal/releases"
)

func TestCheck_NoUpdateWhenVersionsMatch(t *testing.T) {
	handler := newTestHandler(t, []byte("embedded-binary"))

	req := httptest.NewRequest("GET", "/check?current_version=1.0.0", nil)
	w := httptest.NewRecorder()
	handler.Check(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be false when versions match")
	}
	if resp.LatestVersion != "1.0.0" {
		t.Fatalf("LatestVersion = %q, want %q", resp.LatestVersion, "1.0.0")
	}
}

func TestCheck_UpdateAvailableWhenClientIsOlder(t *testing.T) {
	handler := newTestHandler(t, []byte("embedded-binary"))

	// Server has 1.0.0, client has 0.9.0 → update available.
	req := httptest.NewRequest("GET", "/check?current_version=0.9.0", nil)
	w := httptest.NewRecorder()
	handler.Check(w, req)

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if !resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be true when client is older")
	}
	if resp.DownloadURL == "" {
		t.Fatal("DownloadURL should not be empty")
	}
	if resp.SignatureHex == "" {
		t.Fatal("SignatureHex should not be empty")
	}
	if resp.FileSize == 0 {
		t.Fatal("FileSize should not be zero")
	}
	if resp.SHA256Hex == "" {
		t.Fatal("SHA256Hex should not be empty")
	}
}

func TestCheck_NoUpdateWhenBinaryMissing(t *testing.T) {
	handler, binaryPath := newTestHandlerWithBinaryPath(t, []byte("embedded-binary"))
	if err := os.Remove(binaryPath); err != nil {
		t.Fatalf("Remove() error = %v", err)
	}

	req := httptest.NewRequest("GET", "/check?current_version=0.9.0", nil)
	w := httptest.NewRecorder()
	handler.Check(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be false when the host binary is missing")
	}
}

func TestUpdaterCheck_NoUpdateWhenBinaryMissing(t *testing.T) {
	handler, binaryPath := newTestUpdaterHandlerWithBinaryPath(t, []byte("embedded-updater-binary"))
	if err := os.Remove(binaryPath); err != nil {
		t.Fatalf("Remove() error = %v", err)
	}

	req := httptest.NewRequest("GET", "/updater/check?current_version=0.9.0", nil)
	w := httptest.NewRecorder()
	handler.UpdaterCheck(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be false when the updater binary is missing")
	}
}

func TestCheck_NoUpdateWhenClientIsNewer(t *testing.T) {
	handler := newTestHandler(t, []byte("embedded-binary"))

	// Server has 1.0.0, client has 1.0.1 → no downgrade.
	req := httptest.NewRequest("GET", "/check?current_version=1.0.1", nil)
	w := httptest.NewRecorder()
	handler.Check(w, req)

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be false when client is newer (no downgrade)")
	}
}

func TestIsNewerVersion(t *testing.T) {
	tests := []struct {
		current, latest string
		want            bool
	}{
		{"0.7.1", "0.7.2", true},
		{"0.7.2", "0.7.1", false}, // downgrade
		{"0.7.2", "0.7.2", false}, // equal
		{"1.0.0", "1.0.0", false},
		{"0.9.0", "1.0.0", true},
		{"1.0.0", "0.9.0", false},
		{"0.7", "0.7.1", true},
		{"0.7.1", "0.7", false},
		{"1", "2", true},
		{"2", "1", false},
	}
	for _, tt := range tests {
		got := isNewerVersion(tt.current, tt.latest)
		if got != tt.want {
			t.Errorf("isNewerVersion(%q, %q) = %v, want %v", tt.current, tt.latest, got, tt.want)
		}
	}
}

func TestDownloadHostBinaryServesCurrentRelease(t *testing.T) {
	handler := newTestHandler(t, []byte("download-binary"))

	req := httptest.NewRequest("GET", "/download/remotex_host.exe", nil)
	w := httptest.NewRecorder()
	handler.DownloadHostBinary(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	if got := w.Header().Get("Content-Disposition"); got == "" {
		t.Fatal("Content-Disposition should be set")
	}
	if got := w.Body.String(); got != "download-binary" {
		t.Fatalf("body = %q, want %q", got, "download-binary")
	}
}

func TestDownloadHostBinaryNotFoundWithoutRelease(t *testing.T) {
	svc := releases.NewService(releases.Config{
		RootPath:         filepath.Join(t.TempDir(), "releases"),
		FallbackRootPath: filepath.Join(t.TempDir(), "missing"),
		PublicKeyPath:    filepath.Join(t.TempDir(), "missing", "ed25519_public.bin"),
	})
	handler := NewHandler(Config{DownloadURL: "/update/download/remotex_host.exe"}, svc, nil, nil)

	req := httptest.NewRequest("GET", "/download/remotex_host.exe", nil)
	w := httptest.NewRecorder()
	handler.DownloadHostBinary(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
	}
}

func TestCheck_UsesActivatedPublishedRelease(t *testing.T) {
	root := t.TempDir()
	fallbackRoot := filepath.Join(root, "embedded")
	if err := os.MkdirAll(fallbackRoot, 0o755); err != nil {
		t.Fatalf("MkdirAll() fallback error = %v", err)
	}

	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("GenerateKey() error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(fallbackRoot, "ed25519_public.bin"), publicKey, 0o644); err != nil {
		t.Fatalf("WriteFile() public key error = %v", err)
	}
	writeReleaseFixture(t, fallbackRoot, privateKey, "1.0.0", []byte("embedded-binary"))

	cfg := releases.Config{
		RootPath:         filepath.Join(root, "releases"),
		FallbackRootPath: fallbackRoot,
		PublicKeyPath:    filepath.Join(fallbackRoot, "ed25519_public.bin"),
	}
	svc := releases.NewService(cfg)

	publishedDir := filepath.Join(cfg.RootPath, "uploads", "1.1.0")
	if err := os.MkdirAll(publishedDir, 0o755); err != nil {
		t.Fatalf("MkdirAll() published error = %v", err)
	}
	writeReleaseFixture(t, publishedDir, privateKey, "1.1.0", []byte("published-binary"))

	if _, err := svc.Activate("1.1.0"); err != nil {
		t.Fatalf("Activate() error = %v", err)
	}

	handler := NewHandler(Config{DownloadURL: "/update/download/remotex_host.exe"}, svc, nil, nil)
	req := httptest.NewRequest("GET", "/check?current_version=1.0.0", nil)
	w := httptest.NewRecorder()
	handler.Check(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp checkResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if !resp.UpdateAvailable {
		t.Fatal("UpdateAvailable should be true when an activated published release exists")
	}
	if resp.LatestVersion != "1.1.0" {
		t.Fatalf("LatestVersion = %q, want %q", resp.LatestVersion, "1.1.0")
	}
}

func TestReleaseSHA256ForResponseFallsBackToManifest(t *testing.T) {
	binary := []byte("manifest-hash-binary")
	sum := sha256.Sum256(binary)
	want := hex.EncodeToString(sum[:])

	release := releases.ResolvedRelease{
		Manifest: releases.Manifest{
			Version:   "1.2.3",
			SHA256Hex: want,
		},
		BinaryPath: filepath.Join(t.TempDir(), "missing.exe"),
	}

	if got := releaseSHA256ForResponse(release); got != want {
		t.Fatalf("releaseSHA256ForResponse() = %q, want %q", got, want)
	}
}

func newTestHandler(t *testing.T, binary []byte) *Handler {
	handler, _ := newTestHandlerWithBinaryPath(t, binary)
	return handler
}

func newTestHandlerWithBinaryPath(t *testing.T, binary []byte) (*Handler, string) {
	t.Helper()

	root, fallbackRoot, svc := newReleaseServiceFixture(t, "remotex_host.exe", "1.0.0", binary)
	_ = root
	return NewHandler(Config{DownloadURL: "/update/download/remotex_host.exe"}, svc, nil, nil), filepath.Join(fallbackRoot, "remotex_host.exe")
}

func newTestUpdaterHandlerWithBinaryPath(t *testing.T, binary []byte) (*Handler, string) {
	t.Helper()

	_, fallbackRoot, svc := newReleaseServiceFixture(t, "remotex_updater.exe", "1.0.0", binary)
	return NewHandler(Config{UpdaterDownloadURL: "/update/updater/download/remotex_updater.exe"}, nil, nil, svc), filepath.Join(fallbackRoot, "remotex_updater.exe")
}

func newReleaseServiceFixture(t *testing.T, binaryName, version string, binary []byte) (string, string, *releases.Service) {
	t.Helper()

	root := t.TempDir()
	fallbackRoot := filepath.Join(root, "embedded")
	if err := os.MkdirAll(fallbackRoot, 0o755); err != nil {
		t.Fatalf("MkdirAll() error = %v", err)
	}
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("GenerateKey() error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(fallbackRoot, "ed25519_public.bin"), publicKey, 0o644); err != nil {
		t.Fatalf("WriteFile() public key error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(fallbackRoot, binaryName), binary, 0o644); err != nil {
		t.Fatalf("WriteFile() binary error = %v", err)
	}
	manifestData, err := json.Marshal(releases.Manifest{
		Version:      version,
		Binary:       binaryName,
		SignatureHex: hex.EncodeToString(ed25519.Sign(privateKey, binary)),
		FileSize:     int64(len(binary)),
	})
	if err != nil {
		t.Fatalf("Marshal() manifest error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(fallbackRoot, "release.json"), manifestData, 0o644); err != nil {
		t.Fatalf("WriteFile() manifest error = %v", err)
	}

	cfg := releases.Config{
		RootPath:         filepath.Join(root, "releases"),
		FallbackRootPath: fallbackRoot,
		PublicKeyPath:    filepath.Join(fallbackRoot, "ed25519_public.bin"),
	}
	return root, fallbackRoot, releases.NewService(cfg)
}

func writeReleaseFixture(t *testing.T, dir string, privateKey ed25519.PrivateKey, version string, binary []byte) {
	t.Helper()

	if err := os.WriteFile(filepath.Join(dir, "remotex_host.exe"), binary, 0o644); err != nil {
		t.Fatalf("WriteFile() binary error = %v", err)
	}
	manifestData, err := json.Marshal(releases.Manifest{
		Version:      version,
		Binary:       "remotex_host.exe",
		SignatureHex: hex.EncodeToString(ed25519.Sign(privateKey, binary)),
		FileSize:     int64(len(binary)),
	})
	if err != nil {
		t.Fatalf("Marshal() manifest error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "release.json"), manifestData, 0o644); err != nil {
		t.Fatalf("WriteFile() manifest error = %v", err)
	}
}
