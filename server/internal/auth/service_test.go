package auth

import (
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

func TestDefaultConfig(t *testing.T) {
	cfg := DefaultConfig("test-secret")
	if cfg.JWTSecret == "" {
		t.Error("JWTSecret should not be empty")
	}
	if cfg.AccessTokenTTL == 0 {
		t.Error("AccessTokenTTL should be set")
	}
	if cfg.RefreshTokenTTL == 0 {
		t.Error("RefreshTokenTTL should be set")
	}
}

func TestService_GenerateTokenPair(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret-key-for-jwt"))

	pair, err := svc.GenerateTokenPair(TokenSubject{
		UserID:    "user-123",
		Email:     "test@example.com",
		Name:      "Test User",
		Role:      RoleCompanyUser,
		CompanyID: "company-1",
	})
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}
	if pair.AccessToken == "" {
		t.Error("AccessToken should not be empty")
	}
	if pair.RefreshToken == "" {
		t.Error("RefreshToken should not be empty")
	}
	if pair.ExpiresIn <= 0 {
		t.Error("ExpiresIn should be positive")
	}
}

func TestService_ValidateToken(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret-key-for-jwt"))

	pair, err := svc.GenerateTokenPair(TokenSubject{
		UserID:    "user-123",
		Email:     "test@example.com",
		Name:      "Test User",
		Role:      RoleCompanyUser,
		CompanyID: "company-1",
	})
	if err != nil {
		t.Fatalf("GenerateTokenPair failed: %v", err)
	}

	claims, err := svc.ValidateToken(pair.AccessToken)
	if err != nil {
		t.Fatalf("ValidateToken failed: %v", err)
	}
	if claims.UserID != "user-123" {
		t.Errorf("UserID = %q, want %q", claims.UserID, "user-123")
	}
	if claims.Email != "test@example.com" {
		t.Errorf("Email = %q, want %q", claims.Email, "test@example.com")
	}
	if claims.Role != RoleCompanyUser {
		t.Errorf("Role = %q, want %q", claims.Role, RoleCompanyUser)
	}
	if claims.CompanyID != "company-1" {
		t.Errorf("CompanyID = %q, want %q", claims.CompanyID, "company-1")
	}
}

func TestService_ValidateToken_Invalid(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))

	_, err := svc.ValidateToken("invalid.token.here")
	if err == nil {
		t.Error("ValidateToken should fail for invalid token")
	}
}

func TestService_ValidateToken_WrongSecret(t *testing.T) {
	svc1 := NewService(DefaultConfig("secret-1"))
	svc2 := NewService(DefaultConfig("secret-2"))

	pair, _ := svc1.GenerateTokenPair(TokenSubject{UserID: "user-1", Email: "a@b.com", Role: RoleCompanyUser, CompanyID: "company-1"})
	_, err := svc2.ValidateToken(pair.AccessToken)
	if err == nil {
		t.Error("ValidateToken should fail with different secret")
	}
}

func TestValidateAdminSessionToken(t *testing.T) {
	t.Run("accepts valid admin session token", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		token, err := svc.GenerateAdminSessionToken(TokenSubject{
			UserID: "admin-123",
			Email:  "admin@example.com",
			Name:   "Admin",
			Role:   RoleSuperAdmin,
		})
		if err != nil {
			t.Fatalf("GenerateAdminSessionToken failed: %v", err)
		}

		claims, err := svc.ValidateAdminSessionToken(token)
		if err != nil {
			t.Fatalf("ValidateAdminSessionToken failed: %v", err)
		}
		if claims.UserID != "admin-123" {
			t.Fatalf("UserID = %q, want %q", claims.UserID, "admin-123")
		}
		if claims.Issuer != adminSessionTokenIssuer {
			t.Fatalf("Issuer = %q, want %q", claims.Issuer, adminSessionTokenIssuer)
		}
		if claims.Scope != ScopeAdminSession {
			t.Fatalf("Scope = %q, want %q", claims.Scope, ScopeAdminSession)
		}
	})

	t.Run("rejects generic access token", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		pair, err := svc.GenerateTokenPair(TokenSubject{UserID: "admin-123", Email: "admin@example.com", Role: RoleSuperAdmin})
		if err != nil {
			t.Fatalf("GenerateTokenPair failed: %v", err)
		}
		if _, err := svc.ValidateAdminSessionToken(pair.AccessToken); err != ErrTokenInvalid {
			t.Fatalf("ValidateAdminSessionToken error = %v, want %v", err, ErrTokenInvalid)
		}
	})

	t.Run("rejects missing scope claim", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		now := time.Now()
		claims := Claims{
			RegisteredClaims: jwt.RegisteredClaims{
				Subject:   "admin-123",
				IssuedAt:  jwt.NewNumericDate(now),
				ExpiresAt: jwt.NewNumericDate(now.Add(svc.cfg.AccessTokenTTL)),
				Issuer:    adminSessionTokenIssuer,
			},
			UserID:      "admin-123",
			Role:        RoleSuperAdmin,
			SubjectType: SubjectTypeUser,
		}
		token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
		tokenStr, err := token.SignedString([]byte(svc.cfg.JWTSecret))
		if err != nil {
			t.Fatalf("SignedString failed: %v", err)
		}
		if _, err := svc.ValidateAdminSessionToken(tokenStr); err != ErrTokenInvalid {
			t.Fatalf("ValidateAdminSessionToken error = %v, want %v", err, ErrTokenInvalid)
		}
	})

	t.Run("rejects expired admin session token", func(t *testing.T) {
		cfg := DefaultConfig("test-secret")
		cfg.AccessTokenTTL = 25 * time.Millisecond
		svc := NewService(cfg)
		token, err := svc.GenerateAdminSessionToken(TokenSubject{UserID: "admin-123", Email: "admin@example.com", Role: RoleSuperAdmin})
		if err != nil {
			t.Fatalf("GenerateAdminSessionToken failed: %v", err)
		}

		time.Sleep(60 * time.Millisecond)
		if _, err := svc.ValidateAdminSessionToken(token); err != ErrTokenExpired {
			t.Fatalf("ValidateAdminSessionToken error = %v, want %v", err, ErrTokenExpired)
		}
	})
}

func TestValidateAdminChallengeToken(t *testing.T) {
	t.Run("accepts valid setup challenge token", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		token, err := svc.GenerateAdminChallengeToken(TokenSubject{
			UserID: "admin-123",
			Email:  "admin@example.com",
			Name:   "Admin",
			Role:   RoleSuperAdmin,
		}, ScopeAdminChallengeSetup, "JBSWY3DPEHPK3PXP", "otpauth://totp/RemoteX%20Admin:admin@example.com?secret=JBSWY3DPEHPK3PXP&issuer=RemoteX%20Admin", time.Minute)
		if err != nil {
			t.Fatalf("GenerateAdminChallengeToken failed: %v", err)
		}

		claims, err := svc.ValidateAdminChallengeToken(token)
		if err != nil {
			t.Fatalf("ValidateAdminChallengeToken failed: %v", err)
		}
		if claims.Scope != ScopeAdminChallengeSetup {
			t.Fatalf("Scope = %q, want %q", claims.Scope, ScopeAdminChallengeSetup)
		}
		if claims.PendingTOTPSecret != "JBSWY3DPEHPK3PXP" {
			t.Fatalf("PendingTOTPSecret = %q, want expected secret", claims.PendingTOTPSecret)
		}
		if claims.PendingTOTPURL == "" {
			t.Fatal("PendingTOTPURL should not be empty")
		}
	})

	t.Run("accepts valid mfa challenge token", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		token, err := svc.GenerateAdminChallengeToken(TokenSubject{
			UserID: "admin-123",
			Email:  "admin@example.com",
			Name:   "Admin",
			Role:   RoleSuperAdmin,
		}, ScopeAdminChallengeMFA, "", "", time.Minute)
		if err != nil {
			t.Fatalf("GenerateAdminChallengeToken failed: %v", err)
		}

		claims, err := svc.ValidateAdminChallengeToken(token)
		if err != nil {
			t.Fatalf("ValidateAdminChallengeToken failed: %v", err)
		}
		if claims.Scope != ScopeAdminChallengeMFA {
			t.Fatalf("Scope = %q, want %q", claims.Scope, ScopeAdminChallengeMFA)
		}
		if claims.PendingTOTPSecret != "" || claims.PendingTOTPURL != "" {
			t.Fatalf("unexpected setup payload on MFA challenge: %+v", claims)
		}
	})

	t.Run("rejects admin session token", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		token, err := svc.GenerateAdminSessionToken(TokenSubject{UserID: "admin-123", Email: "admin@example.com", Role: RoleSuperAdmin})
		if err != nil {
			t.Fatalf("GenerateAdminSessionToken failed: %v", err)
		}
		if _, err := svc.ValidateAdminChallengeToken(token); err != ErrTokenInvalid {
			t.Fatalf("ValidateAdminChallengeToken error = %v, want %v", err, ErrTokenInvalid)
		}
	})

	t.Run("rejects missing setup payload", func(t *testing.T) {
		svc := NewService(DefaultConfig("test-secret"))
		now := time.Now()
		claims := Claims{
			RegisteredClaims: jwt.RegisteredClaims{
				Subject:   "admin-123",
				IssuedAt:  jwt.NewNumericDate(now),
				ExpiresAt: jwt.NewNumericDate(now.Add(time.Minute)),
				Issuer:    adminChallengeTokenIssuer,
			},
			UserID:      "admin-123",
			Role:        RoleSuperAdmin,
			SubjectType: SubjectTypeUser,
			Scope:       ScopeAdminChallengeSetup,
		}
		token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
		tokenStr, err := token.SignedString([]byte(svc.cfg.JWTSecret))
		if err != nil {
			t.Fatalf("SignedString failed: %v", err)
		}
		if _, err := svc.ValidateAdminChallengeToken(tokenStr); err != ErrTokenInvalid {
			t.Fatalf("ValidateAdminChallengeToken error = %v, want %v", err, ErrTokenInvalid)
		}
	})

	t.Run("rejects expired challenge token", func(t *testing.T) {
		cfg := DefaultConfig("test-secret")
		cfg.AccessTokenTTL = 25 * time.Millisecond
		svc := NewService(cfg)
		token, err := svc.GenerateAdminChallengeToken(TokenSubject{UserID: "admin-123", Email: "admin@example.com", Role: RoleSuperAdmin}, ScopeAdminChallengeMFA, "", "", 25*time.Millisecond)
		if err != nil {
			t.Fatalf("GenerateAdminChallengeToken failed: %v", err)
		}

		time.Sleep(60 * time.Millisecond)
		if _, err := svc.ValidateAdminChallengeToken(token); err != ErrTokenExpired {
			t.Fatalf("ValidateAdminChallengeToken error = %v, want %v", err, ErrTokenExpired)
		}
	})
}

func TestService_HashAndVerifyPassword(t *testing.T) {
	svc := NewService(DefaultConfig("test-secret"))

	hash, err := svc.HashPassword("my-password-123")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}
	if hash == "" {
		t.Error("Hash should not be empty")
	}

	if !svc.CheckPassword("my-password-123", hash) {
		t.Error("CheckPassword should return true for correct password")
	}
	if svc.CheckPassword("wrong-password", hash) {
		t.Error("CheckPassword should return false for wrong password")
	}
}

func TestTokenPair_Expiration(t *testing.T) {
	cfg := DefaultConfig("test-secret")
	cfg.AccessTokenTTL = 1 * time.Second
	svc := NewService(cfg)

	pair, _ := svc.GenerateTokenPair(TokenSubject{UserID: "user-1", Email: "a@b.com", Role: RoleCompanyUser, CompanyID: "company-1"})

	_, err := svc.ValidateToken(pair.AccessToken)
	if err != nil {
		t.Fatalf("Token should be valid immediately: %v", err)
	}

	time.Sleep(2 * time.Second)
	_, err = svc.ValidateToken(pair.AccessToken)
	if err == nil {
		t.Error("Token should be expired after TTL")
	}
}
