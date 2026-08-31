package auth

import (
	"errors"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/pquerna/otp"
	"github.com/pquerna/otp/totp"
	"golang.org/x/crypto/bcrypt"
)

var (
	ErrInvalidCredentials = errors.New("invalid email or password")
	ErrTokenExpired       = errors.New("token expired")
	ErrTokenInvalid       = errors.New("invalid token")
	ErrUserExists         = errors.New("user already exists")
	ErrTOTPCodeMalformed  = errors.New("malformed totp code")
)

type Role string

const (
	RoleSuperAdmin     Role = "super_admin"
	RoleCompanyManager Role = "company_manager"
	RoleCompanyUser    Role = "company_user"
	RoleMachineAgent   Role = "machine_agent"
)

type SubjectType string

const (
	SubjectTypeUser    SubjectType = "user"
	SubjectTypeMachine SubjectType = "machine"

	ScopeHostEnroll          = "host_enroll"
	ScopeAdminSession        = "admin_session"
	ScopeAdminChallengeSetup = "admin_setup_pending"
	ScopeAdminChallengeMFA   = "admin_mfa_pending"

	accessTokenIssuer         = "remotex"
	refreshTokenIssuer        = "remotex-refresh"
	enrollmentTokenIssuer     = "remotex-enroll"
	adminSessionTokenIssuer   = "remotex-admin"
	adminChallengeTokenIssuer = "remotex-admin-challenge"
	adminTOTPIssuer           = "RemoteX Admin"
	adminTOTPSecretSize       = 20
	adminTOTPPeriodSeconds    = 30
	adminTOTPSkew             = 1
)

// Claims personalizados do JWT.
type Claims struct {
	jwt.RegisteredClaims
	UserID            string      `json:"user_id"`
	Email             string      `json:"email"`
	Name              string      `json:"name"`
	Role              Role        `json:"role"`
	CompanyID         string      `json:"company_id,omitempty"`
	SubjectType       SubjectType `json:"subject_type,omitempty"`
	MachineID         string      `json:"machine_id,omitempty"`
	Scope             string      `json:"scope,omitempty"`
	PendingTOTPSecret string      `json:"pending_totp_secret,omitempty"`
	PendingTOTPURL    string      `json:"pending_totp_url,omitempty"`
}

// TokenSubject representa o ator autenticado que será serializado no JWT.
type TokenSubject struct {
	UserID      string
	Email       string
	Name        string
	Role        Role
	CompanyID   string
	SubjectType SubjectType
	MachineID   string
	Scope       string
}

type AdminTOTPEnrollment struct {
	Secret     string
	OTPAuthURL string
}

// TokenPair contém access + refresh tokens.
type TokenPair struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int64  `json:"expires_in"` // segundos até expirar o access token
}

// ServiceConfig contém a configuração do serviço de autenticação.
type ServiceConfig struct {
	JWTSecret       string
	AccessTokenTTL  time.Duration
	RefreshTokenTTL time.Duration
	BcryptCost      int
}

// DefaultConfig retorna uma configuração padrão.
func DefaultConfig(jwtSecret string) ServiceConfig {
	return ServiceConfig{
		JWTSecret:       jwtSecret,
		AccessTokenTTL:  15 * time.Minute,
		RefreshTokenTTL: 7 * 24 * time.Hour,
		BcryptCost:      12,
	}
}

// Service contém a lógica de autenticação.
type Service struct {
	cfg ServiceConfig
}

// NewService cria um novo serviço de autenticação.
func NewService(cfg ServiceConfig) *Service {
	return &Service{cfg: cfg}
}

func (c *Claims) IsMachine() bool {
	return c != nil && c.SubjectType == SubjectTypeMachine
}

// HashPassword gera hash bcrypt da senha.
func (s *Service) HashPassword(password string) (string, error) {
	hash, err := bcrypt.GenerateFromPassword([]byte(password), s.cfg.BcryptCost)
	if err != nil {
		return "", err
	}
	return string(hash), nil
}

// CheckPassword verifica se a senha corresponde ao hash.
func (s *Service) CheckPassword(password, hash string) bool {
	return bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)) == nil
}

func (s *Service) GenerateAdminTOTPEnrollment(email string) (*AdminTOTPEnrollment, error) {
	key, err := totp.Generate(totp.GenerateOpts{
		Issuer:      adminTOTPIssuer,
		AccountName: strings.TrimSpace(strings.ToLower(email)),
		Period:      adminTOTPPeriodSeconds,
		SecretSize:  adminTOTPSecretSize,
		Digits:      otp.DigitsSix,
		Algorithm:   otp.AlgorithmSHA1,
	})
	if err != nil {
		return nil, err
	}

	return &AdminTOTPEnrollment{
		Secret:     key.Secret(),
		OTPAuthURL: key.URL(),
	}, nil
}

func (s *Service) ValidateTOTPCode(secret, code string, now time.Time) (bool, error) {
	if strings.TrimSpace(secret) == "" {
		return false, ErrTokenInvalid
	}

	normalized := normalizeTOTPCode(code)
	if normalized == "" {
		return false, ErrTOTPCodeMalformed
	}

	return totp.ValidateCustom(normalized, strings.TrimSpace(secret), now.UTC(), totp.ValidateOpts{
		Period:    adminTOTPPeriodSeconds,
		Skew:      adminTOTPSkew,
		Digits:    otp.DigitsSix,
		Algorithm: otp.AlgorithmSHA1,
	})
}

func normalizeTOTPCode(code string) string {
	trimmed := strings.TrimSpace(code)
	if len(trimmed) != 6 {
		return ""
	}
	for _, ch := range trimmed {
		if ch < '0' || ch > '9' {
			return ""
		}
	}
	return trimmed
}

// GenerateTokenPair gera access + refresh tokens para o usuário.
func (s *Service) GenerateTokenPair(subject TokenSubject) (*TokenPair, error) {
	subject = normalizeSubject(subject)
	now := time.Now()
	principalID := subject.principalID()

	accessClaims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   principalID,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(s.cfg.AccessTokenTTL)),
			Issuer:    accessTokenIssuer,
		},
		UserID:      subject.UserID,
		Email:       subject.Email,
		Name:        subject.Name,
		Role:        subject.Role,
		CompanyID:   subject.CompanyID,
		SubjectType: subject.SubjectType,
		MachineID:   subject.MachineID,
		Scope:       subject.Scope,
	}

	accessToken := jwt.NewWithClaims(jwt.SigningMethodHS256, accessClaims)
	accessStr, err := accessToken.SignedString([]byte(s.cfg.JWTSecret))
	if err != nil {
		return nil, err
	}

	refreshClaims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   principalID,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(s.cfg.RefreshTokenTTL)),
			Issuer:    refreshTokenIssuer,
		},
		UserID:      subject.UserID,
		Email:       subject.Email,
		Name:        subject.Name,
		Role:        subject.Role,
		CompanyID:   subject.CompanyID,
		SubjectType: subject.SubjectType,
		MachineID:   subject.MachineID,
		Scope:       subject.Scope,
	}

	refreshToken := jwt.NewWithClaims(jwt.SigningMethodHS256, refreshClaims)
	refreshStr, err := refreshToken.SignedString([]byte(s.cfg.JWTSecret))
	if err != nil {
		return nil, err
	}

	return &TokenPair{
		AccessToken:  accessStr,
		RefreshToken: refreshStr,
		ExpiresIn:    int64(s.cfg.AccessTokenTTL.Seconds()),
	}, nil
}

func (s *Service) GenerateEnrollmentToken(subject TokenSubject, ttl time.Duration) (string, error) {
	subject = normalizeSubject(subject)
	now := time.Now()

	claims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   subject.principalID(),
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(ttl)),
			Issuer:    enrollmentTokenIssuer,
		},
		UserID:      subject.UserID,
		Email:       subject.Email,
		Name:        subject.Name,
		Role:        subject.Role,
		CompanyID:   subject.CompanyID,
		SubjectType: subject.SubjectType,
		MachineID:   subject.MachineID,
		Scope:       ScopeHostEnroll,
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString([]byte(s.cfg.JWTSecret))
}

func (s *Service) GenerateAdminSessionToken(subject TokenSubject) (string, error) {
	subject = normalizeSubject(subject)
	if subject.SubjectType != SubjectTypeUser || subject.UserID == "" || subject.Role != RoleSuperAdmin {
		return "", ErrTokenInvalid
	}

	now := time.Now()
	claims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   subject.UserID,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(s.cfg.AccessTokenTTL)),
			Issuer:    adminSessionTokenIssuer,
		},
		UserID:      subject.UserID,
		Email:       subject.Email,
		Name:        subject.Name,
		Role:        subject.Role,
		SubjectType: SubjectTypeUser,
		Scope:       ScopeAdminSession,
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString([]byte(s.cfg.JWTSecret))
}

func (s *Service) GenerateAdminChallengeToken(subject TokenSubject, scope, pendingTOTPSecret, pendingTOTPURL string, ttl time.Duration) (string, error) {
	subject = normalizeSubject(subject)
	if subject.SubjectType != SubjectTypeUser || subject.UserID == "" || subject.Role != RoleSuperAdmin || ttl <= 0 {
		return "", ErrTokenInvalid
	}
	if scope != ScopeAdminChallengeSetup && scope != ScopeAdminChallengeMFA {
		return "", ErrTokenInvalid
	}

	pendingTOTPSecret = strings.TrimSpace(pendingTOTPSecret)
	pendingTOTPURL = strings.TrimSpace(pendingTOTPURL)
	if scope == ScopeAdminChallengeSetup && (pendingTOTPSecret == "" || pendingTOTPURL == "") {
		return "", ErrTokenInvalid
	}
	if scope == ScopeAdminChallengeMFA {
		pendingTOTPSecret = ""
		pendingTOTPURL = ""
	}

	now := time.Now()
	claims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   subject.UserID,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(ttl)),
			Issuer:    adminChallengeTokenIssuer,
		},
		UserID:            subject.UserID,
		Email:             subject.Email,
		Name:              subject.Name,
		Role:              subject.Role,
		SubjectType:       SubjectTypeUser,
		Scope:             scope,
		PendingTOTPSecret: pendingTOTPSecret,
		PendingTOTPURL:    pendingTOTPURL,
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString([]byte(s.cfg.JWTSecret))
}

// ValidateToken valida e parseia um JWT, retornando os claims.
func (s *Service) ValidateToken(tokenStr string) (*Claims, error) {
	token, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (interface{}, error) {
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, ErrTokenInvalid
		}
		return []byte(s.cfg.JWTSecret), nil
	})

	if err != nil {
		if errors.Is(err, jwt.ErrTokenExpired) {
			return nil, ErrTokenExpired
		}
		return nil, ErrTokenInvalid
	}

	claims, ok := token.Claims.(*Claims)
	if !ok || !token.Valid {
		return nil, ErrTokenInvalid
	}

	if claims.SubjectType == "" {
		claims.SubjectType = SubjectTypeUser
	}
	if claims.IsMachine() {
		if claims.MachineID == "" {
			claims.MachineID = claims.UserID
		}
		if claims.UserID == "" {
			claims.UserID = claims.MachineID
		}
	}

	return claims, nil
}

// ValidateRefreshToken valida um refresh token especificamente.
func (s *Service) ValidateRefreshToken(tokenStr string) (*Claims, error) {
	claims, err := s.ValidateToken(tokenStr)
	if err != nil {
		return nil, err
	}

	if claims.Issuer != refreshTokenIssuer {
		return nil, ErrTokenInvalid
	}

	return claims, nil
}

func (s *Service) ValidateAdminSessionToken(tokenStr string) (*Claims, error) {
	claims, err := s.ValidateToken(tokenStr)
	if err != nil {
		return nil, err
	}

	if claims.Issuer != adminSessionTokenIssuer ||
		claims.Scope != ScopeAdminSession ||
		claims.SubjectType != SubjectTypeUser ||
		claims.UserID == "" ||
		claims.Subject != claims.UserID ||
		claims.Role != RoleSuperAdmin {
		return nil, ErrTokenInvalid
	}

	claims.CompanyID = ""
	claims.MachineID = ""
	claims.PendingTOTPSecret = ""
	claims.PendingTOTPURL = ""
	return claims, nil
}

func (s *Service) ValidateAdminChallengeToken(tokenStr string) (*Claims, error) {
	claims, err := s.ValidateToken(tokenStr)
	if err != nil {
		return nil, err
	}

	if claims.Issuer != adminChallengeTokenIssuer ||
		(claims.Scope != ScopeAdminChallengeSetup && claims.Scope != ScopeAdminChallengeMFA) ||
		claims.SubjectType != SubjectTypeUser ||
		claims.UserID == "" ||
		claims.Subject != claims.UserID ||
		claims.Role != RoleSuperAdmin {
		return nil, ErrTokenInvalid
	}
	if claims.Scope == ScopeAdminChallengeSetup && (strings.TrimSpace(claims.PendingTOTPSecret) == "" || strings.TrimSpace(claims.PendingTOTPURL) == "") {
		return nil, ErrTokenInvalid
	}
	if claims.Scope == ScopeAdminChallengeMFA {
		claims.PendingTOTPSecret = ""
		claims.PendingTOTPURL = ""
	}

	claims.CompanyID = ""
	claims.MachineID = ""
	return claims, nil
}

func (s *Service) ValidateEnrollmentToken(tokenStr string) (*Claims, error) {
	claims, err := s.ValidateToken(tokenStr)
	if err != nil {
		return nil, err
	}

	if claims.Issuer != enrollmentTokenIssuer || claims.Scope != ScopeHostEnroll {
		return nil, ErrTokenInvalid
	}

	return claims, nil
}

func normalizeSubject(subject TokenSubject) TokenSubject {
	if subject.SubjectType == "" {
		subject.SubjectType = SubjectTypeUser
	}

	if subject.SubjectType == SubjectTypeMachine {
		if subject.MachineID == "" {
			subject.MachineID = subject.UserID
		}
		if subject.UserID == "" {
			subject.UserID = subject.MachineID
		}
		if subject.Role == "" {
			subject.Role = RoleMachineAgent
		}
	}

	return subject
}

func (s TokenSubject) principalID() string {
	if s.SubjectType == SubjectTypeMachine && s.MachineID != "" {
		return s.MachineID
	}
	return s.UserID
}
