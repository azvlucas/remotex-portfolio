package releases

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResolveCurrentFallsBackToEmbeddedRelease(t *testing.T) {
	cfg, _ := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	release, err := svc.ResolveCurrent()
	if err != nil {
		t.Fatalf("ResolveCurrent() error = %v", err)
	}
	if release.Version != "1.0.0" {
		t.Fatalf("Version = %q, want %q", release.Version, "1.0.0")
	}
	if release.Source != "embedded" {
		t.Fatalf("Source = %q, want %q", release.Source, "embedded")
	}
}

func TestPublishPackageAndActivateRelease(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	pkg := buildReleasePackage(t, priv, "1.1.0", []byte("published-release"), map[string][]byte{
		"zlib1.dll": []byte("zlib"),
	})

	published, err := svc.PublishPackage("remotex_host_1.1.0.zip", bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("PublishPackage() error = %v", err)
	}
	if published.Version != "1.1.0" {
		t.Fatalf("Version = %q, want %q", published.Version, "1.1.0")
	}
	if published.Active {
		t.Fatal("published release should not be active before activation")
	}

	list, err := svc.ListPublished()
	if err != nil {
		t.Fatalf("ListPublished() error = %v", err)
	}
	if len(list) != 1 {
		t.Fatalf("len(ListPublished()) = %d, want 1", len(list))
	}

	active, err := svc.Activate("1.1.0")
	if err != nil {
		t.Fatalf("Activate() error = %v", err)
	}
	if !active.Active {
		t.Fatal("activated release should be marked active")
	}

	resolved, err := svc.ResolveCurrent()
	if err != nil {
		t.Fatalf("ResolveCurrent() after activation error = %v", err)
	}
	if resolved.Version != "1.1.0" {
		t.Fatalf("resolved version = %q, want %q", resolved.Version, "1.1.0")
	}
	if resolved.Source != "published" {
		t.Fatalf("resolved source = %q, want %q", resolved.Source, "published")
	}
	if _, err := os.Stat(filepath.Join(resolved.Directory, "ed25519_public.bin")); err != nil {
		t.Fatalf("expected ed25519_public.bin in active release: %v", err)
	}
	if _, err := os.Stat(filepath.Join(resolved.Directory, "zlib1.dll")); err != nil {
		t.Fatalf("expected optional package file in active release: %v", err)
	}
}

func TestPublishPackageRejectsInvalidSignature(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	pkg := buildReleasePackage(t, priv, "1.2.0", []byte("release-data"), nil)
	pkg = tamperPackageBinary(t, pkg, []byte("tampered-release-data"))

	_, err := svc.PublishPackage("remotex_host_1.2.0.zip", bytes.NewReader(pkg))
	if err == nil {
		t.Fatal("PublishPackage() error = nil, want signature failure")
	}
	if !errors.Is(err, ErrInvalidPackage) {
		t.Fatalf("error = %v, want ErrInvalidPackage", err)
	}
}

func TestPublishPackageAcceptsViewerBinaryWithMatchingVersionMarker(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	binary := []byte("stub RemoteX - Versao 1.1.0 payload")
	pkg := buildNamedReleasePackage(t, priv, "1.1.0", "remotex_viewer.exe", binary, map[string][]byte{
		"Qt6Core.dll": []byte("qt"),
	})

	published, err := svc.PublishPackage("remotex_viewer_1.1.0.zip", bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("PublishPackage() error = %v", err)
	}
	if published.Version != "1.1.0" {
		t.Fatalf("Version = %q, want %q", published.Version, "1.1.0")
	}
}

func TestPublishPackageAcceptsViewerBinaryWithQtFormatMarkerAndMatchingVersion(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	binary := []byte("stub RemoteX - Versao %1 payload 1.1.0 data")
	pkg := buildNamedReleasePackage(t, priv, "1.1.0", "remotex_viewer.exe", binary, map[string][]byte{
		"Qt6Core.dll": []byte("qt"),
	})

	published, err := svc.PublishPackage("remotex_viewer_1.1.0.zip", bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("PublishPackage() error = %v", err)
	}
	if published.Version != "1.1.0" {
		t.Fatalf("Version = %q, want %q", published.Version, "1.1.0")
	}
}

func TestPublishPackageRejectsViewerBinaryVersionMismatch(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	binary := []byte("stub RemoteX - Versao 0.9.9 payload")
	pkg := buildNamedReleasePackage(t, priv, "1.0.48", "remotex_viewer.exe", binary, nil)

	_, err := svc.PublishPackage("remotex_viewer_1.0.48.zip", bytes.NewReader(pkg))
	if err == nil {
		t.Fatal("PublishPackage() error = nil, want viewer version-marker failure")
	}
	if !errors.Is(err, ErrInvalidPackage) {
		t.Fatalf("error = %v, want ErrInvalidPackage", err)
	}
	if !strings.Contains(err.Error(), "version marker") {
		t.Fatalf("error = %v, want version marker detail", err)
	}
}

func TestResolveCurrentFallsBackWhenActiveReleaseIsInvalid(t *testing.T) {
	cfg, priv := writeReleaseFixture(t, "1.0.0", []byte("embedded-release"), nil)
	svc := NewService(cfg)

	pkg := buildReleasePackage(t, priv, "1.1.0", []byte("published-release"), nil)
	if _, err := svc.PublishPackage("remotex_host_1.1.0.zip", bytes.NewReader(pkg)); err != nil {
		t.Fatalf("PublishPackage() error = %v", err)
	}
	if _, err := svc.Activate("1.1.0"); err != nil {
		t.Fatalf("Activate() error = %v", err)
	}

	activeBinary := filepath.Join(cfg.RootPath, "active", "remotex_host.exe")
	if err := os.WriteFile(activeBinary, []byte("tampered-published-release"), 0o644); err != nil {
		t.Fatalf("WriteFile tampered active binary: %v", err)
	}

	resolved, err := svc.ResolveCurrent()
	if err != nil {
		t.Fatalf("ResolveCurrent() error = %v", err)
	}
	if resolved.Version != "1.0.0" {
		t.Fatalf("resolved version = %q, want %q", resolved.Version, "1.0.0")
	}
	if resolved.Source != "embedded" {
		t.Fatalf("resolved source = %q, want %q", resolved.Source, "embedded")
	}
}

func writeReleaseFixture(t *testing.T, version string, binary []byte, extras map[string][]byte) (Config, ed25519.PrivateKey) {
	t.Helper()

	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("GenerateKey: %v", err)
	}

	root := t.TempDir()
	fallbackRoot := filepath.Join(root, "embedded")
	if err := os.MkdirAll(fallbackRoot, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.WriteFile(filepath.Join(fallbackRoot, "ed25519_public.bin"), publicKey, 0o644); err != nil {
		t.Fatalf("WriteFile public key: %v", err)
	}

	writeReleaseFiles(t, fallbackRoot, privateKey, version, binary, extras)

	cfg := Config{
		RootPath:         filepath.Join(root, "releases"),
		FallbackRootPath: fallbackRoot,
		PublicKeyPath:    filepath.Join(fallbackRoot, "ed25519_public.bin"),
	}
	return cfg, privateKey
}

func writeReleaseFiles(t *testing.T, dir string, privateKey ed25519.PrivateKey, version string, binary []byte, extras map[string][]byte) {
	t.Helper()

	if err := os.WriteFile(filepath.Join(dir, "remotex_host.exe"), binary, 0o644); err != nil {
		t.Fatalf("WriteFile binary: %v", err)
	}
	for name, data := range extras {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
			t.Fatalf("WriteFile extra %s: %v", name, err)
		}
	}

	manifest := Manifest{
		Version:      version,
		Binary:       "remotex_host.exe",
		SignatureHex: hex.EncodeToString(ed25519.Sign(privateKey, binary)),
		FileSize:     int64(len(binary)),
	}
	data, err := json.Marshal(manifest)
	if err != nil {
		t.Fatalf("Marshal manifest: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "release.json"), data, 0o644); err != nil {
		t.Fatalf("WriteFile manifest: %v", err)
	}
}

func buildReleasePackage(t *testing.T, privateKey ed25519.PrivateKey, version string, binary []byte, extras map[string][]byte) []byte {
	t.Helper()
	return buildNamedReleasePackage(t, privateKey, version, "remotex_host.exe", binary, extras)
}

func buildNamedReleasePackage(t *testing.T, privateKey ed25519.PrivateKey, version, binaryName string, binary []byte, extras map[string][]byte) []byte {
	t.Helper()

	buffer := &bytes.Buffer{}
	writer := zip.NewWriter(buffer)

	files := map[string][]byte{
		binaryName: binary,
	}
	for name, data := range extras {
		files[name] = data
	}

	manifest := Manifest{
		Version:      version,
		Binary:       binaryName,
		SignatureHex: hex.EncodeToString(ed25519.Sign(privateKey, binary)),
		FileSize:     int64(len(binary)),
	}
	manifestData, err := json.Marshal(manifest)
	if err != nil {
		t.Fatalf("Marshal manifest: %v", err)
	}
	files["release.json"] = manifestData

	for name, data := range files {
		entry, err := writer.Create(name)
		if err != nil {
			t.Fatalf("Create zip entry %s: %v", name, err)
		}
		if _, err := entry.Write(data); err != nil {
			t.Fatalf("Write zip entry %s: %v", name, err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("Close zip writer: %v", err)
	}

	return buffer.Bytes()
}

func tamperPackageBinary(t *testing.T, pkg []byte, replacement []byte) []byte {
	t.Helper()

	reader, err := zip.NewReader(bytes.NewReader(pkg), int64(len(pkg)))
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}

	buffer := &bytes.Buffer{}
	writer := zip.NewWriter(buffer)

	for _, file := range reader.File {
		rc, err := file.Open()
		if err != nil {
			t.Fatalf("Open zip file: %v", err)
		}
		data, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			t.Fatalf("ReadAll: %v", err)
		}
		if file.Name == "remotex_host.exe" {
			data = replacement
		}
		entry, err := writer.Create(file.Name)
		if err != nil {
			t.Fatalf("Create zip entry: %v", err)
		}
		if _, err := entry.Write(data); err != nil {
			t.Fatalf("Write zip entry: %v", err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("Close zip writer: %v", err)
	}

	return buffer.Bytes()
}
