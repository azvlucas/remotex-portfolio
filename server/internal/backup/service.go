package backup

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
)

// R2Config contém a configuração do Cloudflare R2 (compatível S3).
type R2Config struct {
	AccountID       string // Cloudflare Account ID
	AccessKeyID     string
	SecretAccessKey string
	BucketName      string
	PresignExpiry   time.Duration // Validade da URL pré-assinada (padrão 15min)
}

// Service contém a lógica de negócio de backup.
type Service struct {
	repo      *Repository
	r2Config  R2Config
	s3Client  *s3.Client
	presigner *s3.PresignClient
}

// NewService cria um novo serviço de backup.
func NewService(repo *Repository, r2Cfg R2Config) *Service {
	if r2Cfg.PresignExpiry == 0 {
		r2Cfg.PresignExpiry = 15 * time.Minute
	}

	// Endpoint do R2: https://<account_id>.r2.cloudflarestorage.com
	endpoint := fmt.Sprintf("https://%s.r2.cloudflarestorage.com", r2Cfg.AccountID)

	cfg := aws.Config{
		Region: "auto",
		Credentials: credentials.NewStaticCredentialsProvider(
			r2Cfg.AccessKeyID, r2Cfg.SecretAccessKey, "",
		),
		BaseEndpoint: aws.String(endpoint),
	}

	client := s3.NewFromConfig(cfg, func(o *s3.Options) {
		o.UsePathStyle = true
	})

	return &Service{
		repo:      repo,
		r2Config:  r2Cfg,
		s3Client:  client,
		presigner: s3.NewPresignClient(client),
	}
}

// ─── Config ─────────────────────────────────────────────────────────────────

// GetConfig retorna a configuração de backup de uma máquina.
func (s *Service) GetConfig(ctx context.Context, machineID, companyID string) (*BackupConfig, error) {
	return s.repo.GetConfig(ctx, machineID, companyID)
}

// UpsertConfig cria ou atualiza a configuração de backup.
func (s *Service) UpsertConfig(ctx context.Context, machineID, companyID string, folders []string, intervalHours, retentionCount int, enabled bool) (*BackupConfig, error) {
	if intervalHours <= 0 {
		intervalHours = 2
	}
	if retentionCount <= 0 {
		retentionCount = 7
	}
	return s.repo.UpsertConfig(ctx, machineID, companyID, folders, intervalHours, retentionCount, enabled)
}

// ─── Upload (presigned URL) ─────────────────────────────────────────────────

// GenerateUploadURL gera uma URL pré-assinada para upload de um snapshot ao R2.
// Retorna a URL e o storage_key usado.
func (s *Service) GenerateUploadURL(ctx context.Context, machineID, companyID string, sizeBytes int64, fileCount int) (url, storageKey string, err error) {
	// Gera storage key: backups/<machine_id>/<timestamp>.tar.gz
	storageKey = fmt.Sprintf("backups/%s/%d.tar.gz", machineID, time.Now().Unix())

	presignResult, err := s.presigner.PresignPutObject(ctx, &s3.PutObjectInput{
		Bucket:      aws.String(s.r2Config.BucketName),
		Key:         aws.String(storageKey),
		ContentType: aws.String("application/gzip"),
	}, s3.WithPresignExpires(s.r2Config.PresignExpiry))
	if err != nil {
		return "", "", fmt.Errorf("presign error: %w", err)
	}

	// Registra o snapshot no banco.
	_, err = s.repo.CreateSnapshot(ctx, machineID, companyID, storageKey, sizeBytes, fileCount)
	if err != nil {
		return "", "", fmt.Errorf("create snapshot record: %w", err)
	}

	slog.Info("presigned upload URL generated",
		"machine_id", machineID,
		"storage_key", storageKey,
		"size_bytes", sizeBytes,
		"file_count", fileCount)

	return presignResult.URL, storageKey, nil
}

// ─── Snapshots ──────────────────────────────────────────────────────────────

// ListSnapshots retorna os snapshots de uma máquina.
func (s *Service) ListSnapshots(ctx context.Context, machineID, companyID string) ([]Snapshot, error) {
	return s.repo.ListSnapshots(ctx, machineID, companyID)
}

// DeleteSnapshot remove um snapshot do banco e do R2.
func (s *Service) DeleteSnapshot(ctx context.Context, snapshotID, companyID string) error {
	snap, err := s.repo.GetSnapshot(ctx, snapshotID, companyID)
	if err != nil {
		return err
	}

	// Remove do R2.
	_, err = s.s3Client.DeleteObject(ctx, &s3.DeleteObjectInput{
		Bucket: aws.String(s.r2Config.BucketName),
		Key:    aws.String(snap.StorageKey),
	})
	if err != nil {
		slog.Warn("failed to delete from R2, removing DB record anyway",
			"storage_key", snap.StorageKey, "err", err)
	}

	// Remove do banco.
	return s.repo.DeleteSnapshot(ctx, snapshotID, companyID)
}

// EnforceRetention garante que o número de snapshots não excede o limite.
// Remove os snapshots mais antigos até atingir retentionCount.
func (s *Service) EnforceRetention(ctx context.Context, machineID, companyID string) error {
	cfg, err := s.repo.GetConfig(ctx, machineID, companyID)
	if err != nil {
		return fmt.Errorf("get config: %w", err)
	}

	count, err := s.repo.CountSnapshots(ctx, machineID, companyID)
	if err != nil {
		return fmt.Errorf("count snapshots: %w", err)
	}

	for count > cfg.RetentionCount {
		oldest, err := s.repo.GetOldestSnapshot(ctx, machineID, companyID)
		if err != nil {
			return fmt.Errorf("get oldest: %w", err)
		}

		if err := s.DeleteSnapshot(ctx, oldest.ID, companyID); err != nil {
			return fmt.Errorf("delete oldest snapshot: %w", err)
		}

		slog.Info("retention: deleted oldest snapshot",
			"machine_id", machineID,
			"snapshot_id", oldest.ID,
			"storage_key", oldest.StorageKey)
		count--
	}

	return nil
}
