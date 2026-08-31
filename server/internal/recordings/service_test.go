package recordings

import (
	"context"
	"errors"
	"testing"

	"github.com/azvlucas/remotex-portfolio/server/internal/backup"
)

type fakeRecordingRepo struct {
	expectedUpdateStatusID        string
	expectedUpdateStatusCompanyID string
	updateStatusErr               error
	updateStatusCalls             int
	lastUpdateStatusID            string
	lastUpdateStatusCompanyID     string
	lastUpdateStatus              string
}

func (r *fakeRecordingRepo) Create(context.Context, *string, string, string, string, int64, int) (*Recording, error) {
	panic("unexpected call to Create")
}

func (r *fakeRecordingRepo) UpdateStatus(_ context.Context, id, companyID, status string) error {
	r.updateStatusCalls++
	r.lastUpdateStatusID = id
	r.lastUpdateStatusCompanyID = companyID
	r.lastUpdateStatus = status

	if r.updateStatusErr != nil {
		return r.updateStatusErr
	}
	if r.expectedUpdateStatusID != "" && id != r.expectedUpdateStatusID {
		return ErrNotFound
	}
	if r.expectedUpdateStatusCompanyID != "" && companyID != r.expectedUpdateStatusCompanyID {
		return ErrNotFound
	}
	return nil
}

func (r *fakeRecordingRepo) GetByID(context.Context, string, string) (*Recording, error) {
	panic("unexpected call to GetByID")
}

func (r *fakeRecordingRepo) GetBySessionID(context.Context, string, string) (*Recording, error) {
	panic("unexpected call to GetBySessionID")
}

func (r *fakeRecordingRepo) ListByCompany(context.Context, string, int, int) ([]Recording, error) {
	panic("unexpected call to ListByCompany")
}

func (r *fakeRecordingRepo) ListByMachine(context.Context, string, string, int, int) ([]Recording, error) {
	panic("unexpected call to ListByMachine")
}

func (r *fakeRecordingRepo) Delete(context.Context, string, string) (*Recording, error) {
	panic("unexpected call to Delete")
}

func TestEnsureR2Configured(t *testing.T) {
	t.Run("missing config", func(t *testing.T) {
		svc := &Service{}
		if err := svc.ensureR2Configured(); err == nil {
			t.Fatal("ensureR2Configured() returned nil, want error")
		}
	})

	t.Run("configured", func(t *testing.T) {
		svc := &Service{
			r2Config: backup.R2Config{
				AccountID:       "account",
				AccessKeyID:     "key",
				SecretAccessKey: "secret",
				BucketName:      "bucket",
			},
		}
		if err := svc.ensureR2Configured(); err != nil {
			t.Fatalf("ensureR2Configured() error = %v, want nil", err)
		}
	})
}

func TestConfirmUploadThreadsCompanyScopeToRepository(t *testing.T) {
	repo := &fakeRecordingRepo{
		expectedUpdateStatusID:        "rec-1",
		expectedUpdateStatusCompanyID: "company-1",
	}
	svc := &Service{repo: repo}

	if err := svc.ConfirmUpload(context.Background(), "rec-1", "company-1"); err != nil {
		t.Fatalf("ConfirmUpload() error = %v, want nil", err)
	}
	if repo.updateStatusCalls != 1 {
		t.Fatalf("updateStatusCalls = %d, want 1", repo.updateStatusCalls)
	}
	if repo.lastUpdateStatusID != "rec-1" {
		t.Fatalf("recording id = %q, want %q", repo.lastUpdateStatusID, "rec-1")
	}
	if repo.lastUpdateStatusCompanyID != "company-1" {
		t.Fatalf("company id = %q, want %q", repo.lastUpdateStatusCompanyID, "company-1")
	}
	if repo.lastUpdateStatus != "uploaded" {
		t.Fatalf("status = %q, want %q", repo.lastUpdateStatus, "uploaded")
	}
}

func TestConfirmUploadReturnsNotFoundForWrongCompany(t *testing.T) {
	repo := &fakeRecordingRepo{
		expectedUpdateStatusID:        "rec-1",
		expectedUpdateStatusCompanyID: "company-1",
	}
	svc := &Service{repo: repo}

	err := svc.ConfirmUpload(context.Background(), "rec-1", "company-2")
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("ConfirmUpload() error = %v, want %v", err, ErrNotFound)
	}
	if repo.updateStatusCalls != 1 {
		t.Fatalf("updateStatusCalls = %d, want 1", repo.updateStatusCalls)
	}
	if repo.lastUpdateStatusCompanyID != "company-2" {
		t.Fatalf("company id = %q, want %q", repo.lastUpdateStatusCompanyID, "company-2")
	}
}
