package relay

import (
	"testing"
	"time"
)

func TestTicketIssuerRoundTrip(t *testing.T) {
	t.Parallel()

	issuer := NewTicketIssuer("secret", "relay.example.com", 9100, 5*time.Minute)
	bundle, err := issuer.IssueBundle("session-1", RoleViewer, "company-1", "user-1", "machine-1")
	if err != nil {
		t.Fatalf("IssueBundle returned error: %v", err)
	}
	if bundle == nil {
		t.Fatal("IssueBundle returned nil bundle")
	}
	if bundle.RelaySessionID != "session-1" {
		t.Fatalf("RelaySessionID = %q, want session-1", bundle.RelaySessionID)
	}

	claims, err := ValidateTicket([]byte("secret"), bundle.RelayTicket, RoleViewer, "session-1", time.Now())
	if err != nil {
		t.Fatalf("ValidateTicket returned error: %v", err)
	}
	if claims.Role != RoleViewer {
		t.Fatalf("claims.Role = %q, want %q", claims.Role, RoleViewer)
	}
}

func TestValidateTicketRejectsWrongRole(t *testing.T) {
	t.Parallel()

	issuer := NewTicketIssuer("secret", "", 9100, 5*time.Minute)
	bundle, err := issuer.IssueBundle("session-1", RoleViewer, "company-1", "user-1", "machine-1")
	if err != nil {
		t.Fatalf("IssueBundle returned error: %v", err)
	}

	if _, err := ValidateTicket([]byte("secret"), bundle.RelayTicket, RoleHost, "session-1", time.Now()); err == nil {
		t.Fatal("ValidateTicket succeeded with wrong role")
	}
}

func TestParseHello(t *testing.T) {
	t.Parallel()

	hello, err := parseHello("HELLO 1 host realtime session-1 1893456000 3 token-value\n")
	if err != nil {
		t.Fatalf("parseHello returned error: %v", err)
	}
	if hello.Role != RoleHost || hello.Lane != "realtime" || hello.SessionID != "session-1" {
		t.Fatalf("unexpected hello: %+v", hello)
	}
}
