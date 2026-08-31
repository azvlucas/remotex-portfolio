package releases

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf16"
)

var (
	ErrInvalidPackage         = errors.New("invalid release package")
	ErrReleaseNotFound        = errors.New("release not found")
	ErrReleaseVersionConflict = errors.New("release version conflict")
	ErrReleaseIsActive        = errors.New("cannot delete active release")
)

type Manifest struct {
	Version      string `json:"version"`
	Binary       string `json:"binary,omitempty"`
	SignatureHex string `json:"signature_hex"`
	FileSize     int64  `json:"file_size,omitempty"`
	SHA256Hex    string `json:"sha256_hex,omitempty"`
}

type Config struct {
	RootPath         string
	FallbackRootPath string
	PublicKeyPath    string
}

type Service struct {
	cfg Config
}

type ResolvedRelease struct {
	Manifest
	ManifestPath string
	BinaryPath   string
	Directory    string
	Source       string
}

type PublishedRelease struct {
	Version      string    `json:"version"`
	Binary       string    `json:"binary"`
	SignatureHex string    `json:"signature_hex"`
	FileSize     int64     `json:"file_size"`
	UploadedAt   time.Time `json:"uploaded_at"`
	Active       bool      `json:"active"`
	Source       string    `json:"source"`
}

func ConfigFromEnv() Config {
	fallbackRoot := envOr("RELEASES_FALLBACK_ROOT", "/data/host")
	return Config{
		RootPath:         envOr("RELEASES_ROOT_PATH", "/data/releases"),
		FallbackRootPath: fallbackRoot,
		PublicKeyPath:    envOr("RELEASES_PUBLIC_KEY_PATH", filepath.Join(fallbackRoot, "ed25519_public.bin")),
	}
}

func NewService(cfg Config) *Service {
	return &Service{cfg: cfg}
}

func (s *Service) ResolveCurrent() (ResolvedRelease, error) {
	activeDir := s.activeDir()
	if info, err := os.Stat(activeDir); err == nil && info.IsDir() {
		release, err := s.loadReleaseFromDir(activeDir, "published")
		if err == nil {
			if verifyErr := s.verifyRelease(release); verifyErr == nil {
				return release, nil
			}
		}
	}

	release, err := s.loadReleaseFromDir(s.cfg.FallbackRootPath, "embedded")
	if err != nil {
		return ResolvedRelease{}, err
	}
	if err := s.verifyRelease(release); err != nil {
		return ResolvedRelease{}, err
	}
	return release, nil
}

func (s *Service) ListPublished() ([]PublishedRelease, error) {
	activeVersion := ""
	if active, err := s.loadReleaseFromDir(s.activeDir(), "published"); err == nil {
		activeVersion = active.Version
	}

	entries, err := os.ReadDir(s.uploadsDir())
	if errors.Is(err, os.ErrNotExist) {
		return []PublishedRelease{}, nil
	}
	if err != nil {
		return nil, err
	}

	items := make([]PublishedRelease, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}

		release, err := s.loadReleaseFromDir(filepath.Join(s.uploadsDir(), entry.Name()), "published")
		if err != nil {
			continue
		}

		info, err := os.Stat(release.ManifestPath)
		if err != nil {
			continue
		}

		items = append(items, PublishedRelease{
			Version:      release.Version,
			Binary:       release.Binary,
			SignatureHex: release.SignatureHex,
			FileSize:     release.FileSize,
			UploadedAt:   info.ModTime().UTC(),
			Active:       release.Version == activeVersion,
			Source:       "published",
		})
	}

	sort.Slice(items, func(i, j int) bool {
		if items[i].UploadedAt.Equal(items[j].UploadedAt) {
			return items[i].Version > items[j].Version
		}
		return items[i].UploadedAt.After(items[j].UploadedAt)
	})

	return items, nil
}

func (s *Service) PublishPackage(fileName string, src io.Reader) (PublishedRelease, error) {
	if strings.ToLower(filepath.Ext(strings.TrimSpace(fileName))) != ".zip" {
		return PublishedRelease{}, fmt.Errorf("%w: expected .zip package", ErrInvalidPackage)
	}

	if err := os.MkdirAll(s.uploadsDir(), 0o755); err != nil {
		return PublishedRelease{}, err
	}

	tmpDir, err := os.MkdirTemp("", "remotex-release-upload-*")
	if err != nil {
		return PublishedRelease{}, err
	}
	defer os.RemoveAll(tmpDir)

	zipPath := filepath.Join(tmpDir, "release.zip")
	dst, err := os.Create(zipPath)
	if err != nil {
		return PublishedRelease{}, err
	}
	if _, err := io.Copy(dst, src); err != nil {
		dst.Close()
		return PublishedRelease{}, err
	}
	if err := dst.Close(); err != nil {
		return PublishedRelease{}, err
	}

	extractDir := filepath.Join(tmpDir, "contents")
	if err := os.MkdirAll(extractDir, 0o755); err != nil {
		return PublishedRelease{}, err
	}
	if err := extractZip(zipPath, extractDir); err != nil {
		return PublishedRelease{}, err
	}

	release, err := s.loadReleaseFromDir(extractDir, "published")
	if err != nil {
		return PublishedRelease{}, fmt.Errorf("%w: %v", ErrInvalidPackage, err)
	}
	if err := s.verifyRelease(release); err != nil {
		return PublishedRelease{}, fmt.Errorf("%w: %v", ErrInvalidPackage, err)
	}
	if err := s.ensurePublicKeyCopy(extractDir); err != nil {
		return PublishedRelease{}, err
	}

	destDir := filepath.Join(s.uploadsDir(), release.Version)
	if info, err := os.Stat(destDir); err == nil && info.IsDir() {
		same, cmpErr := sameReleaseBinary(filepath.Join(destDir, release.Binary), release.BinaryPath)
		if cmpErr != nil {
			return PublishedRelease{}, cmpErr
		}
		if !same {
			return PublishedRelease{}, fmt.Errorf("%w: version %s already exists", ErrReleaseVersionConflict, release.Version)
		}
		return s.readPublishedRelease(destDir, false)
	}

	if err := copyDir(extractDir, destDir); err != nil {
		return PublishedRelease{}, err
	}

	// Save original zip as package.zip so it can be served for viewer downloads.
	if err := copyFile(zipPath, filepath.Join(destDir, "package.zip")); err != nil {
		return PublishedRelease{}, err
	}

	return s.readPublishedRelease(destDir, false)
}

func (s *Service) Activate(version string) (PublishedRelease, error) {
	version = strings.TrimSpace(version)
	if version == "" {
		return PublishedRelease{}, ErrReleaseNotFound
	}

	sourceDir := filepath.Join(s.uploadsDir(), version)
	release, err := s.loadReleaseFromDir(sourceDir, "published")
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return PublishedRelease{}, ErrReleaseNotFound
		}
		return PublishedRelease{}, err
	}
	if err := s.verifyRelease(release); err != nil {
		return PublishedRelease{}, err
	}

	stageDir := filepath.Join(s.cfg.RootPath, ".active-stage")
	if err := os.RemoveAll(stageDir); err != nil {
		return PublishedRelease{}, err
	}
	if err := copyDir(sourceDir, stageDir); err != nil {
		return PublishedRelease{}, err
	}
	if err := s.ensurePublicKeyCopy(stageDir); err != nil {
		return PublishedRelease{}, err
	}

	activeDir := s.activeDir()
	if err := os.RemoveAll(activeDir); err != nil {
		return PublishedRelease{}, err
	}
	if err := os.Rename(stageDir, activeDir); err != nil {
		return PublishedRelease{}, err
	}

	return s.readPublishedRelease(activeDir, true)
}

func LoadManifest(path string) (Manifest, error) {
	if strings.TrimSpace(path) == "" {
		return Manifest{}, fmt.Errorf("release manifest path is empty")
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return Manifest{}, err
	}

	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return Manifest{}, fmt.Errorf("invalid release manifest: %w", err)
	}

	manifest.Version = strings.TrimSpace(manifest.Version)
	manifest.Binary = strings.TrimSpace(manifest.Binary)
	manifest.SignatureHex = strings.TrimSpace(strings.ToLower(manifest.SignatureHex))
	if manifest.Binary == "" {
		manifest.Binary = "remotex_host.exe"
	}
	if manifest.Version == "" {
		return Manifest{}, fmt.Errorf("release manifest version is empty")
	}
	if manifest.SignatureHex == "" {
		return Manifest{}, fmt.Errorf("release manifest signature_hex is empty")
	}

	return manifest, nil
}

func (s *Service) Delete(version string) error {
	version = strings.TrimSpace(version)
	if version == "" {
		return ErrReleaseNotFound
	}

	versionDir := filepath.Join(s.uploadsDir(), version)
	if _, err := os.Stat(versionDir); errors.Is(err, os.ErrNotExist) {
		return ErrReleaseNotFound
	}

	if active, err := s.loadReleaseFromDir(s.activeDir(), "published"); err == nil {
		if active.Version == version {
			return ErrReleaseIsActive
		}
	}

	return os.RemoveAll(versionDir)
}

func (s *Service) readPublishedRelease(dir string, active bool) (PublishedRelease, error) {
	release, err := s.loadReleaseFromDir(dir, "published")
	if err != nil {
		return PublishedRelease{}, err
	}
	info, err := os.Stat(release.ManifestPath)
	if err != nil {
		return PublishedRelease{}, err
	}

	return PublishedRelease{
		Version:      release.Version,
		Binary:       release.Binary,
		SignatureHex: release.SignatureHex,
		FileSize:     release.FileSize,
		UploadedAt:   info.ModTime().UTC(),
		Active:       active,
		Source:       "published",
	}, nil
}

func (s *Service) loadReleaseFromDir(dir, source string) (ResolvedRelease, error) {
	manifestPath := filepath.Join(dir, "release.json")
	manifest, err := LoadManifest(manifestPath)
	if err != nil {
		return ResolvedRelease{}, err
	}

	binaryPath := filepath.Join(dir, manifest.Binary)
	info, err := os.Stat(binaryPath)
	if err != nil {
		return ResolvedRelease{}, err
	}
	if info.IsDir() {
		return ResolvedRelease{}, fmt.Errorf("release binary path is a directory")
	}

	if manifest.FileSize <= 0 {
		manifest.FileSize = info.Size()
	}

	return ResolvedRelease{
		Manifest:     manifest,
		ManifestPath: manifestPath,
		BinaryPath:   binaryPath,
		Directory:    dir,
		Source:       source,
	}, nil
}

func (s *Service) verifyRelease(release ResolvedRelease) error {
	info, err := os.Stat(release.BinaryPath)
	if err != nil {
		return err
	}
	if release.FileSize > 0 && info.Size() != release.FileSize {
		return fmt.Errorf("release manifest file_size=%d does not match binary size=%d", release.FileSize, info.Size())
	}

	pubKey, err := os.ReadFile(s.cfg.PublicKeyPath)
	if err != nil {
		return fmt.Errorf("failed to read public key: %w", err)
	}
	if len(pubKey) != ed25519.PublicKeySize {
		return fmt.Errorf("invalid public key length %d", len(pubKey))
	}

	signature, err := hex.DecodeString(release.SignatureHex)
	if err != nil {
		return fmt.Errorf("invalid release signature hex: %w", err)
	}
	if len(signature) != ed25519.SignatureSize {
		return fmt.Errorf("invalid release signature length %d", len(signature))
	}

	data, err := os.ReadFile(release.BinaryPath)
	if err != nil {
		return err
	}
	if !ed25519.Verify(ed25519.PublicKey(pubKey), data, signature) {
		return fmt.Errorf("release signature verification failed")
	}
	if err := verifyBinaryVersionMarker(release, data); err != nil {
		return err
	}

	return nil
}

func verifyBinaryVersionMarker(release ResolvedRelease, data []byte) error {
	if !strings.EqualFold(release.Binary, "remotex_viewer.exe") {
		return nil
	}

	exactMarkers := []string{
		"RemoteX - Versao " + release.Version,
		"RemoteX - Version " + release.Version,
	}
	for _, marker := range exactMarkers {
		if containsVersionMarker(data, marker) {
			return nil
		}
	}

	formatMarkers := []string{
		"RemoteX - Versao %1",
		"RemoteX - Version %1",
	}
	for _, marker := range formatMarkers {
		if containsVersionMarker(data, marker) && containsVersionMarker(data, release.Version) {
			return nil
		}
	}

	return fmt.Errorf("viewer binary version marker does not match manifest version %q", release.Version)
}

func containsVersionMarker(data []byte, marker string) bool {
	if marker == "" {
		return false
	}
	if bytes.Contains(data, []byte(marker)) {
		return true
	}

	utf16Marker := utf16LEBytes(marker)
	return len(utf16Marker) > 0 && bytes.Contains(data, utf16Marker)
}

func utf16LEBytes(s string) []byte {
	if s == "" {
		return nil
	}
	runes := utf16.Encode([]rune(s))
	out := make([]byte, 0, len(runes)*2)
	for _, r := range runes {
		out = append(out, byte(r), byte(r>>8))
	}
	return out
}

func (s *Service) ensurePublicKeyCopy(dir string) error {
	if s.cfg.PublicKeyPath == "" {
		return nil
	}

	srcInfo, err := os.Stat(s.cfg.PublicKeyPath)
	if err != nil {
		return err
	}
	if srcInfo.IsDir() {
		return fmt.Errorf("public key path is a directory")
	}

	return copyFile(s.cfg.PublicKeyPath, filepath.Join(dir, "ed25519_public.bin"))
}

func (s *Service) uploadsDir() string {
	return filepath.Join(s.cfg.RootPath, "uploads")
}

func (s *Service) activeDir() string {
	return filepath.Join(s.cfg.RootPath, "active")
}

func (r *ResolvedRelease) ComputeSHA256() (string, error) {
	data, err := os.ReadFile(r.BinaryPath)
	if err != nil {
		return "", err
	}
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:]), nil
}

// PackagePath returns the path to the saved upload package (zip) if it exists.
func (r *ResolvedRelease) PackagePath() string {
	return filepath.Join(r.Directory, "package.zip")
}

// ComputePackageSHA256 returns the SHA-256 hex of the saved package.zip.
func (r *ResolvedRelease) ComputePackageSHA256() (string, error) {
	data, err := os.ReadFile(r.PackagePath())
	if err != nil {
		return "", err
	}
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:]), nil
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func extractZip(srcZip, destDir string) error {
	reader, err := zip.OpenReader(srcZip)
	if err != nil {
		return err
	}
	defer reader.Close()

	for _, file := range reader.File {
		if file.FileInfo().IsDir() {
			continue
		}

		name := filepath.ToSlash(strings.TrimSpace(file.Name))
		if name == "" || strings.Contains(name, "/") {
			return fmt.Errorf("%w: package must contain only root files", ErrInvalidPackage)
		}

		destPath := filepath.Join(destDir, name)
		if !strings.HasPrefix(destPath, filepath.Clean(destDir)+string(os.PathSeparator)) {
			return fmt.Errorf("%w: invalid zip entry path", ErrInvalidPackage)
		}

		rc, err := file.Open()
		if err != nil {
			return err
		}

		dst, err := os.OpenFile(destPath, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
		if err != nil {
			rc.Close()
			return err
		}
		if _, err := io.Copy(dst, rc); err != nil {
			dst.Close()
			rc.Close()
			return err
		}
		if err := dst.Close(); err != nil {
			rc.Close()
			return err
		}
		if err := rc.Close(); err != nil {
			return err
		}
	}

	return nil
}

func sameReleaseBinary(existingPath, incomingPath string) (bool, error) {
	existingData, err := os.ReadFile(existingPath)
	if err != nil {
		return false, err
	}
	incomingData, err := os.ReadFile(incomingPath)
	if err != nil {
		return false, err
	}

	existingHash := sha256.Sum256(existingData)
	incomingHash := sha256.Sum256(incomingData)
	return existingHash == incomingHash, nil
}

func copyDir(srcDir, destDir string) error {
	if err := os.RemoveAll(destDir); err != nil {
		return err
	}
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}

	entries, err := os.ReadDir(srcDir)
	if err != nil {
		return err
	}

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		if err := copyFile(filepath.Join(srcDir, entry.Name()), filepath.Join(destDir, entry.Name())); err != nil {
			return err
		}
	}

	return nil
}

func copyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0o644)
}
