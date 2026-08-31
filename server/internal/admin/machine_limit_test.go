package admin

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

type execAdminDB struct {
	t            *testing.T
	expectedSQL  string
	expectedArgs []any
	tag          pgconn.CommandTag
	execErr      error
}

func (f *execAdminDB) Query(context.Context, string, ...any) (pgx.Rows, error) {
	f.t.Helper()
	f.t.Fatal("unexpected Query call")
	return nil, nil
}

func (f *execAdminDB) QueryRow(context.Context, string, ...any) pgx.Row {
	f.t.Helper()
	f.t.Fatal("unexpected QueryRow call")
	return fakeRow{err: errors.New("unexpected QueryRow call")}
}

func (f *execAdminDB) Exec(_ context.Context, sql string, args ...any) (pgconn.CommandTag, error) {
	f.t.Helper()
	if strings.TrimSpace(sql) != strings.TrimSpace(f.expectedSQL) {
		f.t.Fatalf("unexpected exec query:\n%s", sql)
	}
	if !reflect.DeepEqual(args, f.expectedArgs) {
		f.t.Fatalf("unexpected exec args: got %v want %v", args, f.expectedArgs)
	}
	return f.tag, f.execErr
}

func intPtr(v int) *int {
	return &v
}

func newAdminFormRequest(t *testing.T, authSvc *auth.Service, target string, form url.Values) *http.Request {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, target, strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	req.AddCookie(&http.Cookie{Name: csrfCookieName, Value: "csrf-machine-limit", Path: "/admin"})
	return req
}

func TestUpdateMachineLimitPersistsValue(t *testing.T) {
	t.Parallel()

	limit := 6
	svc := NewService(&execAdminDB{
		t: t,
		expectedSQL: `UPDATE companies
		    SET machine_limit = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		expectedArgs: []any{"company-1", &limit},
		tag:          pgconn.NewCommandTag("UPDATE 1"),
	}, nil, nil)

	if err := svc.UpdateMachineLimit(context.Background(), "company-1", &limit); err != nil {
		t.Fatalf("UpdateMachineLimit returned error: %v", err)
	}
}

func TestUpdateMachineLimitUnknownCompanyFailsClosed(t *testing.T) {
	t.Parallel()

	var limit *int
	svc := NewService(&execAdminDB{
		t: t,
		expectedSQL: `UPDATE companies
		    SET machine_limit = $2,
		        updated_at = NOW()
		  WHERE id = $1`,
		expectedArgs: []any{"missing-company", limit},
		tag:          pgconn.NewCommandTag("UPDATE 0"),
	}, nil, nil)

	err := svc.UpdateMachineLimit(context.Background(), "missing-company", limit)
	if !errors.Is(err, ErrCompanyNotFound) {
		t.Fatalf("expected ErrCompanyNotFound, got %v", err)
	}
}

func TestCompanyDetailRendersMachineLimitCard(t *testing.T) {
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := &Handler{
		svc: &fakeAdminService{
			getCompanyDetailFn: func(ctx context.Context, companyID string) (*CompanyDetail, error) {
				if companyID != "company-1" {
					t.Fatalf("companyID = %q, want %q", companyID, "company-1")
				}
				return &CompanyDetail{
					Company: CompanySummary{
						ID:                     companyID,
						Name:                   "Alpha",
						IsActive:               true,
						UserCount:              2,
						MachineCount:           4,
						OnlineMachineCount:     1,
						ActiveSessionCount:     3,
						ConcurrentSessionLimit: intPtr(7),
						MachineLimit:           nil,
					},
					Users: []CompanyUser{},
				}, nil
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	req := httptest.NewRequest(http.MethodGet, "/companies/company-1", nil)
	markSecureAdminRequest(req)
	req.AddCookie(adminAccessCookie(t, authSvc))
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}
	body := w.Body.String()
	for _, needle := range []string{
		"Limite de maquinas",
		"/admin/companies/company-1/machine-limit",
		"name=\"machine_limit\"",
		"placeholder=\"vazio ou 0 = sem limite\"",
		"Sem limite",
		"Limite de sessoes simultaneas",
		"Maximo: <strong>7</strong> sessoes",
	} {
		if !strings.Contains(body, needle) {
			t.Fatalf("response body missing %q: %s", needle, body)
		}
	}
}

func TestUpdateMachineLimitNormalizesUnlimitedValues(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		raw       string
		wantLimit *int
	}{
		{name: "blank_means_unlimited", raw: "", wantLimit: nil},
		{name: "zero_means_unlimited", raw: "0", wantLimit: nil},
		{name: "positive_persists", raw: "9", wantLimit: intPtr(9)},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
			called := false
			handler := &Handler{
				svc: &fakeAdminService{
					updateMachineLimitFn: func(ctx context.Context, companyID string, limit *int) error {
						called = true
						if companyID != "company-1" {
							t.Fatalf("companyID = %q, want %q", companyID, "company-1")
						}
						if !reflect.DeepEqual(limit, tc.wantLimit) {
							t.Fatalf("limit = %#v, want %#v", limit, tc.wantLimit)
						}
						return nil
					},
				},
				authSvc: authSvc,
				users:   &fakeAdminUserStore{},
			}

			form := url.Values{}
			form.Set("csrf_token", "csrf-machine-limit")
			form.Set("machine_limit", tc.raw)
			req := newAdminFormRequest(t, authSvc, "/companies/company-1/machine-limit", form)
			w := httptest.NewRecorder()

			handler.Routes().ServeHTTP(w, req)

			if !called {
				t.Fatal("expected UpdateMachineLimit to be called")
			}
			if w.Code != http.StatusSeeOther {
				t.Fatalf("status = %d, want %d", w.Code, http.StatusSeeOther)
			}
			if location := w.Header().Get("Location"); !strings.Contains(location, "/admin/companies/company-1?message=") {
				t.Fatalf("unexpected redirect location: %s", location)
			}
		})
	}
}

func TestUpdateMachineLimitRejectsMalformedValues(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		raw  string
	}{
		{name: "negative", raw: "-1"},
		{name: "nonnumeric", raw: "abc"},
		{name: "trailing_garbage", raw: "7oops"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
			called := false
			handler := &Handler{
				svc: &fakeAdminService{
					updateMachineLimitFn: func(context.Context, string, *int) error {
						called = true
						return nil
					},
				},
				authSvc: authSvc,
				users:   &fakeAdminUserStore{},
			}

			form := url.Values{}
			form.Set("csrf_token", "csrf-machine-limit")
			form.Set("machine_limit", tc.raw)
			req := newAdminFormRequest(t, authSvc, "/companies/company-1/machine-limit", form)
			req.Header.Set("Accept", "application/json")
			w := httptest.NewRecorder()

			handler.Routes().ServeHTTP(w, req)

			if called {
				t.Fatal("unexpected UpdateMachineLimit call")
			}
			if w.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want %d", w.Code, http.StatusBadRequest)
			}
			if body := w.Body.String(); !strings.Contains(body, `"error"`) {
				t.Fatalf("response body missing error payload: %s", body)
			}
		})
	}
}

func TestUpdateMachineLimitUnknownCompanyUsesExistingNotFoundSurface(t *testing.T) {
	t.Parallel()

	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	handler := &Handler{
		svc: &fakeAdminService{
			updateMachineLimitFn: func(context.Context, string, *int) error {
				return ErrCompanyNotFound
			},
		},
		authSvc: authSvc,
		users:   &fakeAdminUserStore{},
	}

	form := url.Values{}
	form.Set("csrf_token", "csrf-machine-limit")
	form.Set("machine_limit", "5")
	req := newAdminFormRequest(t, authSvc, "/companies/missing-company/machine-limit", form)
	req.Header.Set("Accept", "application/json")
	w := httptest.NewRecorder()

	handler.Routes().ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusNotFound)
	}
	if body := w.Body.String(); !strings.Contains(body, `"error"`) {
		t.Fatalf("response body missing error payload: %s", body)
	}
}

func TestMachineLimitSchemaParityAcrossBootstrapPaths(t *testing.T) {
	t.Parallel()

	_, currentFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}

	bootstrapPath := filepath.Join(filepath.Dir(currentFile), "..", "bootstrap", "bootstrap.go")
	initSQLPath := filepath.Join(filepath.Dir(currentFile), "..", "..", "..", "deploy", "postgres", "init.sql")

	bootstrapContent, err := os.ReadFile(bootstrapPath)
	if err != nil {
		t.Fatalf("read bootstrap.go: %v", err)
	}
	initSQLContent, err := os.ReadFile(initSQLPath)
	if err != nil {
		t.Fatalf("read init.sql: %v", err)
	}

	bootstrapText := string(bootstrapContent)
	initSQLText := string(initSQLContent)

	for _, needle := range []string{
		"machine_limit INTEGER DEFAULT NULL,",
		"ALTER TABLE companies ADD COLUMN IF NOT EXISTS machine_limit INTEGER DEFAULT NULL",
	} {
		if !strings.Contains(bootstrapText, needle) {
			t.Fatalf("bootstrap.go is missing %q", needle)
		}
	}
	if !strings.Contains(initSQLText, "machine_limit INTEGER,") {
		t.Fatalf("init.sql is missing machine_limit column contract")
	}
}
