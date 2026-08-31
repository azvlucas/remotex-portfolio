package recordings

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"

	"github.com/azvlucas/remotex-portfolio/server/internal/backup"
)

type recordingRepository interface {
	Create(ctx context.Context, sessionID *string, companyID, machineID, storageKey string, fileSize int64, durationSec int) (*Recording, error)
	UpdateStatus(ctx context.Context, id, companyID, status string) error
	GetByID(ctx context.Context, id, companyID string) (*Recording, error)
	GetBySessionID(ctx context.Context, sessionID, companyID string) (*Recording, error)
	ListByCompany(ctx context.Context, companyID string, limit, offset int) ([]Recording, error)
	ListByMachine(ctx context.Context, machineID, companyID string, limit, offset int) ([]Recording, error)
	Delete(ctx context.Context, id, companyID string) (*Recording, error)
}

// Service contém a lógica de negócio de gravações de sessão.
type Service struct {
	repo      recordingRepository
	r2Config  backup.R2Config
	s3Client  *s3.Client
	presigner *s3.PresignClient
}

// NewService cria um novo serviço de gravações. Reutiliza R2Config do pacote backup (mesmo bucket).
func NewService(repo *Repository, r2Cfg backup.R2Config) *Service {
	if r2Cfg.PresignExpiry == 0 {
		r2Cfg.PresignExpiry = 15 * time.Minute
	}

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

func (s *Service) ensureR2Configured() error {
	var missing []string
	if s.r2Config.AccountID == "" {
		missing = append(missing, "R2_ACCOUNT_ID")
	}
	if s.r2Config.AccessKeyID == "" {
		missing = append(missing, "R2_ACCESS_KEY_ID")
	}
	if s.r2Config.SecretAccessKey == "" {
		missing = append(missing, "R2_SECRET_ACCESS_KEY")
	}
	if s.r2Config.BucketName == "" {
		missing = append(missing, "R2_BUCKET_NAME")
	}
	if len(missing) > 0 {
		return fmt.Errorf("recordings R2 not configured: missing %s", strings.Join(missing, ", "))
	}
	return nil
}

// GenerateUploadURL gera uma URL pré-assinada para upload de gravação ao R2.
// Retorna o recording criado (status=pending) e a presigned URL.
func (s *Service) GenerateUploadURL(ctx context.Context, sessionID *string, companyID, machineID string, fileSize int64, durationSec int) (*Recording, string, error) {
	if err := s.ensureR2Configured(); err != nil {
		return nil, "", err
	}

	// Storage key: recordings/{company_id}/{machine_id}/{timestamp}.mp4
	storageKey := fmt.Sprintf("recordings/%s/%s/%d.mp4", companyID, machineID, time.Now().Unix())

	presignResult, err := s.presigner.PresignPutObject(ctx, &s3.PutObjectInput{
		Bucket:      aws.String(s.r2Config.BucketName),
		Key:         aws.String(storageKey),
		ContentType: aws.String("video/mp4"),
	}, s3.WithPresignExpires(s.r2Config.PresignExpiry))
	if err != nil {
		return nil, "", fmt.Errorf("presign error: %w", err)
	}

	rec, err := s.repo.Create(ctx, sessionID, companyID, machineID, storageKey, fileSize, durationSec)
	if err != nil {
		return nil, "", fmt.Errorf("create recording record: %w", err)
	}

	slog.Info("recording presigned upload URL generated",
		"recording_id", rec.ID,
		"machine_id", machineID,
		"storage_key", storageKey,
		"file_size", fileSize,
		"duration_sec", durationSec)

	return rec, presignResult.URL, nil
}

// ConfirmUpload marca a gravação como uploaded.
func (s *Service) ConfirmUpload(ctx context.Context, id, companyID string) error {
	return s.repo.UpdateStatus(ctx, id, companyID, "uploaded")
}

// GetByID retorna uma gravação por ID.
func (s *Service) GetByID(ctx context.Context, id, companyID string) (*Recording, error) {
	return s.repo.GetByID(ctx, id, companyID)
}

// GetBySessionID retorna a gravação de uma sessão específica.
func (s *Service) GetBySessionID(ctx context.Context, sessionID, companyID string) (*Recording, error) {
	return s.repo.GetBySessionID(ctx, sessionID, companyID)
}

// ListByCompany retorna gravações de uma empresa.
func (s *Service) ListByCompany(ctx context.Context, companyID string, limit, offset int) ([]Recording, error) {
	return s.repo.ListByCompany(ctx, companyID, limit, offset)
}

// ListByMachine retorna gravações de uma máquina.
func (s *Service) ListByMachine(ctx context.Context, machineID, companyID string, limit, offset int) ([]Recording, error) {
	return s.repo.ListByMachine(ctx, machineID, companyID, limit, offset)
}

// GenerateDownloadURL gera uma URL pré-assinada para download de uma gravação.
func (s *Service) GenerateDownloadURL(ctx context.Context, id, companyID string) (string, error) {
	if err := s.ensureR2Configured(); err != nil {
		return "", err
	}

	rec, err := s.repo.GetByID(ctx, id, companyID)
	if err != nil {
		return "", err
	}

	downloadName := fmt.Sprintf("recording-%s.mp4", rec.ID)
	presignResult, err := s.presigner.PresignGetObject(ctx, &s3.GetObjectInput{
		Bucket:                     aws.String(s.r2Config.BucketName),
		Key:                        aws.String(rec.StorageKey),
		ResponseContentDisposition: aws.String(fmt.Sprintf("inline; filename=\"%s\"", downloadName)),
		ResponseContentType:        aws.String("video/mp4"),
	}, s3.WithPresignExpires(s.r2Config.PresignExpiry))
	if err != nil {
		return "", fmt.Errorf("presign download error: %w", err)
	}

	return presignResult.URL, nil
}

// Delete remove uma gravação do R2 e do banco.
func (s *Service) Delete(ctx context.Context, id, companyID string) error {
	rec, err := s.repo.Delete(ctx, id, companyID)
	if err != nil {
		return err
	}

	// Remove do R2 (best-effort).
	_, delErr := s.s3Client.DeleteObject(ctx, &s3.DeleteObjectInput{
		Bucket: aws.String(s.r2Config.BucketName),
		Key:    aws.String(rec.StorageKey),
	})
	if delErr != nil {
		slog.Warn("failed to delete recording from R2",
			"recording_id", id,
			"storage_key", rec.StorageKey,
			"err", delErr)
	}

	slog.Info("recording deleted", "recording_id", id, "storage_key", rec.StorageKey)
	return nil
}
