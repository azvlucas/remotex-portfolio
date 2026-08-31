package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

func TestRunMintsRawTokenWithExpectedClaims(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	exitCode := run([]string{
		"-company-id", "company-1",
		"-user-id", "manager-1",
		"-email", "Manager@Example.com",
		"-name", "Manager One",
	}, &stdout, &stderr, envLookup(map[string]string{"JWT_SECRET": "test-secret"}))
	if exitCode != 0 {
		t.Fatalf("exitCode = %d, stderr = %s", exitCode, stderr.String())
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}

	token := strings.TrimSpace(stdout.String())
	if token == "" {
		t.Fatal("expected token output")
	}

	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))
	claims, err := authSvc.ValidateEnrollmentToken(token)
	if err != nil {
		t.Fatalf("ValidateEnrollmentToken() error = %v", err)
	}
	if claims.Issuer != "remotex-enroll" {
		t.Fatalf("claims.Issuer = %q, want remotex-enroll", claims.Issuer)
	}
	if claims.Scope != auth.ScopeHostEnroll {
		t.Fatalf("claims.Scope = %q, want %q", claims.Scope, auth.ScopeHostEnroll)
	}
	if claims.CompanyID != "company-1" {
		t.Fatalf("claims.CompanyID = %q, want company-1", claims.CompanyID)
	}
	if claims.UserID != "manager-1" {
		t.Fatalf("claims.UserID = %q, want manager-1", claims.UserID)
	}
	if claims.Email != "manager@example.com" {
		t.Fatalf("claims.Email = %q, want manager@example.com", claims.Email)
	}
	if claims.Name != "Manager One" {
		t.Fatalf("claims.Name = %q, want Manager One", claims.Name)
	}
	if claims.Role != auth.RoleCompanyManager {
		t.Fatalf("claims.Role = %q, want %q", claims.Role, auth.RoleCompanyManager)
	}
	if claims.SubjectType != auth.SubjectTypeUser {
		t.Fatalf("claims.SubjectType = %q, want %q", claims.SubjectType, auth.SubjectTypeUser)
	}
}

func TestRunMintsJSONOutputWithExpectedMetadata(t *testing.T) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	exitCode := run([]string{
		"-company-id", "company-2",
		"-user-id", "user-2",
		"-role", "company_user",
		"-ttl", "2h30m",
		"-output", "json",
	}, &stdout, &stderr, envLookup(map[string]string{"JWT_SECRET": "json-secret"}))
	if exitCode != 0 {
		t.Fatalf("exitCode = %d, stderr = %s", exitCode, stderr.String())
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want empty", stderr.String())
	}

	var payload jsonOutput
	if err := json.Unmarshal(stdout.Bytes(), &payload); err != nil {
		t.Fatalf("json.Unmarshal() error = %v", err)
	}
	if payload.Token == "" {
		t.Fatal("expected token in json output")
	}
	if payload.Issuer != "remotex-enroll" {
		t.Fatalf("payload.Issuer = %q, want remotex-enroll", payload.Issuer)
	}
	if payload.Scope != auth.ScopeHostEnroll {
		t.Fatalf("payload.Scope = %q, want %q", payload.Scope, auth.ScopeHostEnroll)
	}
	if payload.CompanyID != "company-2" {
		t.Fatalf("payload.CompanyID = %q, want company-2", payload.CompanyID)
	}
	if payload.UserID != "user-2" {
		t.Fatalf("payload.UserID = %q, want user-2", payload.UserID)
	}
	if payload.Role != auth.RoleCompanyUser {
		t.Fatalf("payload.Role = %q, want %q", payload.Role, auth.RoleCompanyUser)
	}
	if payload.SubjectType != auth.SubjectTypeUser {
		t.Fatalf("payload.SubjectType = %q, want %q", payload.SubjectType, auth.SubjectTypeUser)
	}
	if payload.TTLSeconds != int64((150 * time.Minute).Seconds()) {
		t.Fatalf("payload.TTLSeconds = %d, want %d", payload.TTLSeconds, int64((150 * time.Minute).Seconds()))
	}

	authSvc := auth.NewService(auth.DefaultConfig("json-secret"))
	claims, err := authSvc.ValidateEnrollmentToken(payload.Token)
	if err != nil {
		t.Fatalf("ValidateEnrollmentToken() error = %v", err)
	}
	if claims.CompanyID != payload.CompanyID || claims.UserID != payload.UserID || claims.Role != payload.Role {
		t.Fatalf("claims mismatch = %+v payload = %+v", claims, payload)
	}
}

func TestRunRejectsInvalidInputsAndMissingSecret(t *testing.T) {
	tests := []struct {
		name          string
		args          []string
		env           map[string]string
		wantErrSubstr string
	}{
		{
			name:          "missing secret",
			args:          []string{"-company-id", "company-1", "-user-id", "user-1"},
			env:           map[string]string{},
			wantErrSubstr: "JWT_SECRET is required",
		},
		{
			name:          "blank company id",
			args:          []string{"-company-id", "   ", "-user-id", "user-1"},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "company-id is required",
		},
		{
			name:          "blank user id",
			args:          []string{"-company-id", "company-1", "-user-id", "   "},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "user-id is required",
		},
		{
			name:          "invalid ttl",
			args:          []string{"-company-id", "company-1", "-user-id", "user-1", "-ttl", "later"},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "ttl must be a positive duration",
		},
		{
			name:          "invalid output",
			args:          []string{"-company-id", "company-1", "-user-id", "user-1", "-output", "yaml"},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "invalid output",
		},
		{
			name:          "unsupported subject type",
			args:          []string{"-company-id", "company-1", "-user-id", "user-1", "-subject-type", "machine"},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "unsupported subject_type",
		},
		{
			name:          "invalid role",
			args:          []string{"-company-id", "company-1", "-user-id", "user-1", "-role", "super_admin"},
			env:           map[string]string{"JWT_SECRET": "test-secret"},
			wantErrSubstr: "invalid role",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			var stdout bytes.Buffer
			var stderr bytes.Buffer

			exitCode := run(tc.args, &stdout, &stderr, envLookup(tc.env))
			if exitCode == 0 {
				t.Fatalf("expected non-zero exit code, stdout = %q", stdout.String())
			}
			if stdout.Len() != 0 {
				t.Fatalf("stdout = %q, want empty on error", stdout.String())
			}
			if !strings.Contains(stderr.String(), tc.wantErrSubstr) {
				t.Fatalf("stderr = %q, want substring %q", stderr.String(), tc.wantErrSubstr)
			}
		})
	}
}

func envLookup(values map[string]string) func(string) (string, bool) {
	return func(key string) (string, bool) {
		value, ok := values[key]
		return value, ok
	}
}
