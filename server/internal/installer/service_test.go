package installer

import (
	"os"
	"path/filepath"
	"testing"
)

func TestCopyReleasePayloadCopiesAllRuntimeFiles(t *testing.T) {
	releaseDir := t.TempDir()
	distDir := t.TempDir()

	files := map[string][]byte{
		"zlib1.dll":               []byte("zlib"),
		"ed25519_public.bin":      []byte("pub"),
		"avcodec-62.dll":          []byte("avcodec"),
		"swscale-9.dll":           []byte("swscale"),
		"helper.exe":              []byte("helper"),
		"release.json":            []byte(`{"version":"1.0.0"}`),
		"remotex_host.exe":        []byte("binary"),
		"README.md":               []byte("docs"),
		"remotex_host_0.1.17.zip": []byte("archive"),
	}
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(releaseDir, name), data, 0o644); err != nil {
			t.Fatalf("WriteFile(%s): %v", name, err)
		}
	}

	if err := copyReleasePayload(releaseDir, distDir); err != nil {
		t.Fatalf("copyReleasePayload() error = %v", err)
	}

	wantPresent := []string{"zlib1.dll", "ed25519_public.bin", "avcodec-62.dll", "swscale-9.dll", "helper.exe"}
	for _, name := range wantPresent {
		if _, err := os.Stat(filepath.Join(distDir, name)); err != nil {
			t.Fatalf("expected %s in dist dir: %v", name, err)
		}
	}

	wantAbsent := []string{"release.json", "remotex_host.exe", "README.md", "remotex_host_0.1.17.zip"}
	for _, name := range wantAbsent {
		if _, err := os.Stat(filepath.Join(distDir, name)); !os.IsNotExist(err) {
			t.Fatalf("expected %s to be absent from dist dir, got err=%v", name, err)
		}
	}
}
