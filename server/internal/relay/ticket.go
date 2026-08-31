package relay

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"time"
)

const ProtocolVersion = 1

type TicketClaims struct {
	Version   int    `json:"v"`
	SessionID string `json:"sid"`
	Role      string `json:"role"`
	CompanyID string `json:"cid,omitempty"`
	UserID    string `json:"uid,omitempty"`
	MachineID string `json:"mid,omitempty"`
	ExpiresAt int64  `json:"exp"`
}

type TransportBundle struct {
	RelayProtocolVersion   int    `json:"relay_protocol_version"`
	RelaySessionID         string `json:"relay_session_id"`
	RelayRole              string `json:"relay_role"`
	RelayTicket            string `json:"relay_ticket"`
	RelayTicketExpiresUnix int64  `json:"relay_ticket_expires_unix"`
	RelayHost              string `json:"relay_host,omitempty"`
	RelayPort              int    `json:"relay_port,omitempty"`
}

type TicketIssuer struct {
	secret    []byte
	relayHost string
	relayPort int
	ttl       time.Duration
}

func NewTicketIssuer(secret, relayHost string, relayPort int, ttl time.Duration) *TicketIssuer {
	if secret == "" {
		return nil
	}
	if ttl <= 0 {
		ttl = 15 * time.Minute
	}
	return &TicketIssuer{
		secret:    []byte(secret),
		relayHost: relayHost,
		relayPort: relayPort,
		ttl:       ttl,
	}
}

func (i *TicketIssuer) IssueBundle(sessionID, role, companyID, userID, machineID string) (*TransportBundle, error) {
	if i == nil {
		return nil, nil
	}
	if sessionID == "" {
		return nil, errors.New("missing relay session id")
	}
	if role != "host" && role != "viewer" {
		return nil, fmt.Errorf("invalid relay role %q", role)
	}

	expiresAt := time.Now().Add(i.ttl).Unix()
	claims := TicketClaims{
		Version:   ProtocolVersion,
		SessionID: sessionID,
		Role:      role,
		CompanyID: companyID,
		UserID:    userID,
		MachineID: machineID,
		ExpiresAt: expiresAt,
	}
	token, err := SignTicket(i.secret, claims)
	if err != nil {
		return nil, err
	}

	return &TransportBundle{
		RelayProtocolVersion:   ProtocolVersion,
		RelaySessionID:         sessionID,
		RelayRole:              role,
		RelayTicket:            token,
		RelayTicketExpiresUnix: expiresAt,
		RelayHost:              i.relayHost,
		RelayPort:              i.relayPort,
	}, nil
}

func SignTicket(secret []byte, claims TicketClaims) (string, error) {
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}

	encodedPayload := base64.RawURLEncoding.EncodeToString(payload)
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(encodedPayload))
	signature := mac.Sum(nil)

	return encodedPayload + "." + base64.RawURLEncoding.EncodeToString(signature), nil
}

func ValidateTicket(secret []byte, token, expectedRole, expectedSessionID string, now time.Time) (*TicketClaims, error) {
	parts := splitTicket(token)
	if len(parts) != 2 {
		return nil, errors.New("invalid ticket format")
	}

	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(parts[0]))
	expectedSig := mac.Sum(nil)

	gotSig, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, errors.New("invalid ticket signature encoding")
	}
	if !hmac.Equal(gotSig, expectedSig) {
		return nil, errors.New("invalid ticket signature")
	}

	payload, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return nil, errors.New("invalid ticket payload encoding")
	}

	var claims TicketClaims
	if err := json.Unmarshal(payload, &claims); err != nil {
		return nil, errors.New("invalid ticket payload")
	}
	if claims.Version != ProtocolVersion {
		return nil, fmt.Errorf("unsupported ticket version %d", claims.Version)
	}
	if claims.Role != expectedRole {
		return nil, errors.New("ticket role mismatch")
	}
	if claims.SessionID != expectedSessionID {
		return nil, errors.New("ticket session mismatch")
	}
	if claims.ExpiresAt <= now.Unix() {
		return nil, errors.New("ticket expired")
	}

	return &claims, nil
}

func splitTicket(token string) []string {
	for i := 0; i < len(token); i++ {
		if token[i] == '.' {
			return []string{token[:i], token[i+1:]}
		}
	}
	return nil
}
