package recordings

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

func newRecordingsTestRouter(authSvc *auth.Service, handler *Handler) http.Handler {
	router := chi.NewRouter()
	router.Group(func(r chi.Router) {
		r.Use(auth.JWTAuthMiddleware(authSvc, nil))
		r.Mount("/recordings", handler.Routes())
	})
	return router
}

func mustGenerateRecordingToken(t *testing.T, authSvc *auth.Service, subject auth.TokenSubject) string {
	t.Helper()

	pair, err := authSvc.GenerateTokenPair(subject)
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}

	return pair.AccessToken
}

func TestWriteJSON_NoHTMLEscape(t *testing.T) {
	// Presigned URLs contain & separating query params.
	// writeJSON must NOT escape & as \u0026.
	url := "https://bucket.r2.cloudflarestorage.com/key?X-Amz-Algorithm=AWS4&X-Amz-Credential=abc&X-Amz-Signature=def"

	w := httptest.NewRecorder()
	writeJSON(w, 200, uploadResponse{
		ID:  "rec-123",
		URL: url,
	})

	body := w.Body.String()

	if strings.Contains(body, `\u0026`) {
		t.Fatalf("writeJSON escaped & as \\u0026:\n%s", body)
	}
	if !strings.Contains(body, `&X-Amz-Credential`) {
		t.Fatalf("writeJSON did not preserve literal & in URL:\n%s", body)
	}
}

func TestWriteJSON_NoHTMLEscapeDownloadResponse(t *testing.T) {
	url := "https://bucket.r2.cloudflarestorage.com/key?X-Amz-Algorithm=AWS4&response-content-disposition=inline%3B%20filename%3D%22recording.mp4%22&X-Amz-Signature=def"

	w := httptest.NewRecorder()
	writeJSON(w, 200, downloadResponse{URL: url})

	body := w.Body.String()

	if strings.Contains(body, `\u0026`) {
		t.Fatalf("writeJSON escaped & as \\u0026:\n%s", body)
	}
	if !strings.Contains(body, `response-content-disposition=inline%3B%20filename%3D%22recording.mp4%22`) {
		t.Fatalf("writeJSON did not preserve download response query string:\n%s", body)
	}
}

func TestConfirmUploadHandlerReturnsNoContentForSameCompany(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &fakeRecordingRepo{
		expectedUpdateStatusID:        "rec-1",
		expectedUpdateStatusCompanyID: "company-1",
	}
	handler := NewHandler(&Service{repo: repo})
	router := newRecordingsTestRouter(authSvc, handler)

	token := mustGenerateRecordingToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	})

	req := httptest.NewRequest(http.MethodPut, "/recordings/confirm/rec-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNoContent, w.Body.String())
	}
	if repo.updateStatusCalls != 1 {
		t.Fatalf("updateStatusCalls = %d, want 1", repo.updateStatusCalls)
	}
	if repo.lastUpdateStatusCompanyID != "company-1" {
		t.Fatalf("company id = %q, want %q", repo.lastUpdateStatusCompanyID, "company-1")
	}
	if repo.lastUpdateStatus != "uploaded" {
		t.Fatalf("status = %q, want %q", repo.lastUpdateStatus, "uploaded")
	}
}

func TestConfirmUploadHandlerReturnsNotFoundForWrongCompany(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &fakeRecordingRepo{
		expectedUpdateStatusID:        "rec-1",
		expectedUpdateStatusCompanyID: "company-2",
	}
	handler := NewHandler(&Service{repo: repo})
	router := newRecordingsTestRouter(authSvc, handler)

	token := mustGenerateRecordingToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	})

	req := httptest.NewRequest(http.MethodPut, "/recordings/confirm/rec-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNotFound, w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"recording not found"`) {
		t.Fatalf("body = %s, want recording not found", w.Body.String())
	}
	if repo.updateStatusCalls != 1 {
		t.Fatalf("updateStatusCalls = %d, want 1", repo.updateStatusCalls)
	}
}

func TestConfirmUploadHandlerReturnsNotFoundForUnknownRecording(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &fakeRecordingRepo{
		expectedUpdateStatusID:        "rec-1",
		expectedUpdateStatusCompanyID: "company-1",
	}
	handler := NewHandler(&Service{repo: repo})
	router := newRecordingsTestRouter(authSvc, handler)

	token := mustGenerateRecordingToken(t, authSvc, auth.TokenSubject{
		UserID:    "user-1",
		Email:     "user@example.com",
		Name:      "Operator",
		Role:      auth.RoleCompanyUser,
		CompanyID: "company-1",
	})

	req := httptest.NewRequest(http.MethodPut, "/recordings/confirm/missing-rec", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusNotFound, w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"recording not found"`) {
		t.Fatalf("body = %s, want recording not found", w.Body.String())
	}
}

func TestConfirmUploadHandlerRejectsMissingCompanyClaim(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	repo := &fakeRecordingRepo{}
	handler := NewHandler(&Service{repo: repo})
	router := newRecordingsTestRouter(authSvc, handler)

	token := mustGenerateRecordingToken(t, authSvc, auth.TokenSubject{
		UserID: "user-1",
		Email:  "user@example.com",
		Name:   "Operator",
		Role:   auth.RoleCompanyUser,
	})

	req := httptest.NewRequest(http.MethodPut, "/recordings/confirm/rec-1", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d, body = %s", w.Code, http.StatusUnauthorized, w.Body.String())
	}
	if repo.updateStatusCalls != 0 {
		t.Fatalf("updateStatusCalls = %d, want 0", repo.updateStatusCalls)
	}
}
