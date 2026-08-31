package turn

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

func newTestRouter(cfg Config) http.Handler {
	h := NewHandler(cfg)
	r := chi.NewRouter()
	r.Mount("/turn", h.Routes())
	return r
}

func newTestRouterWithHandler(h *Handler) http.Handler {
	r := chi.NewRouter()
	r.Mount("/turn", h.Routes())
	return r
}

func TestCredentials_Success(t *testing.T) {
	cfg := Config{
		Secret:        "test-shared-secret",
		Servers:       []string{"turn-1:3478", "turn-2:3479"},
		CredentialTTL: 24 * time.Hour,
	}
	router := newTestRouter(cfg)

	req := httptest.NewRequest("GET", "/turn/credentials?user_id=user-123", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var cred TURNCredential
	if err := json.NewDecoder(w.Body).Decode(&cred); err != nil {
		t.Fatalf("decode error: %v", err)
	}

	if cred.Username == "" {
		t.Error("Username should not be empty")
	}
	if cred.Password == "" {
		t.Error("Password should not be empty")
	}
	if cred.TTL <= 0 {
		t.Error("TTL should be positive")
	}
	if len(cred.URIs) == 0 {
		t.Error("URIs should not be empty")
	}
}

func TestCredentials_UsernameFormat(t *testing.T) {
	cfg := Config{
		Secret:        "test-secret",
		Servers:       []string{"turn:3478"},
		CredentialTTL: 1 * time.Hour,
	}
	router := newTestRouter(cfg)

	req := httptest.NewRequest("GET", "/turn/credentials?user_id=alice", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	var cred TURNCredential
	_ = json.NewDecoder(w.Body).Decode(&cred)

	if cred.Username == "" {
		t.Fatal("Username should not be empty")
	}
	found := false
	for _, c := range cred.Username {
		if c == ':' {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("Username %q should contain ':'", cred.Username)
	}
}

func TestServers_ListHealthy(t *testing.T) {
	latency := int64(12)
	h := newHandlerWithProbe(Config{Servers: []string{"turn-1:3478"}}, time.Second, func(ctx context.Context, address string) turnProbeResult {
		if address != net.JoinHostPort("turn-1", "3478") {
			t.Fatalf("probe address = %q, want %q", address, net.JoinHostPort("turn-1", "3478"))
		}
		return turnProbeResult{
			status:    observability.StatusHealthy,
			checked:   true,
			reason:    turnReasonBindingSuccess,
			latencyMS: &latency,
			checkedAt: time.Unix(1700000000, 0).UTC(),
		}
	})
	router := newTestRouterWithHandler(h)

	servers := getServers(t, router)
	if len(servers) != 1 {
		t.Fatalf("len(servers) = %d, want 1", len(servers))
	}
	server := servers[0]
	if !server.Healthy {
		t.Fatal("Healthy = false, want true")
	}
	if server.Status != observability.StatusHealthy {
		t.Fatalf("Status = %q, want %q", server.Status, observability.StatusHealthy)
	}
	if !server.Checked {
		t.Fatal("Checked = false, want true")
	}
	if server.Reason != turnReasonBindingSuccess {
		t.Fatalf("Reason = %q, want %q", server.Reason, turnReasonBindingSuccess)
	}
	if server.LatencyMS == nil || *server.LatencyMS != latency {
		t.Fatalf("LatencyMS = %v, want %d", server.LatencyMS, latency)
	}
	if server.CheckedAt == nil {
		t.Fatal("CheckedAt should not be nil")
	}
}

func TestServers_ListProbeFailure(t *testing.T) {
	h := newHandlerWithProbe(Config{Servers: []string{"turn-1:3478"}}, time.Second, func(ctx context.Context, address string) turnProbeResult {
		return turnProbeResult{
			status:    observability.StatusDegraded,
			checked:   true,
			reason:    turnReasonProbeFailed,
			lastError: "dial udp: no route to host",
			checkedAt: time.Unix(1700000000, 0).UTC(),
		}
	})
	router := newTestRouterWithHandler(h)

	servers := getServers(t, router)
	server := servers[0]
	if server.Healthy {
		t.Fatal("Healthy = true, want false")
	}
	if server.Status != observability.StatusDegraded {
		t.Fatalf("Status = %q, want %q", server.Status, observability.StatusDegraded)
	}
	if !server.Checked {
		t.Fatal("Checked = false, want true")
	}
	if server.Reason != turnReasonProbeFailed {
		t.Fatalf("Reason = %q, want %q", server.Reason, turnReasonProbeFailed)
	}
	if server.LastError == "" {
		t.Fatal("LastError should not be empty")
	}
}

func TestServers_ListProbeTimeout(t *testing.T) {
	h := newHandlerWithProbe(Config{Servers: []string{"turn-1:3478"}}, time.Second, func(ctx context.Context, address string) turnProbeResult {
		return turnProbeResult{
			status:    observability.StatusDegraded,
			checked:   true,
			reason:    turnReasonProbeTimeout,
			lastError: context.DeadlineExceeded.Error(),
			checkedAt: time.Unix(1700000000, 0).UTC(),
		}
	})
	router := newTestRouterWithHandler(h)

	servers := getServers(t, router)
	server := servers[0]
	if server.Healthy {
		t.Fatal("Healthy = true, want false")
	}
	if server.Status != observability.StatusDegraded {
		t.Fatalf("Status = %q, want %q", server.Status, observability.StatusDegraded)
	}
	if !server.Checked {
		t.Fatal("Checked = false, want true")
	}
	if server.Reason != turnReasonProbeTimeout {
		t.Fatalf("Reason = %q, want %q", server.Reason, turnReasonProbeTimeout)
	}
}

func TestServers_ListInvalidConfig(t *testing.T) {
	probeCalls := 0
	h := newHandlerWithProbe(Config{Servers: []string{"turn-1:not-a-port"}}, time.Second, func(ctx context.Context, address string) turnProbeResult {
		probeCalls++
		return turnProbeResult{}
	})
	router := newTestRouterWithHandler(h)

	servers := getServers(t, router)
	server := servers[0]
	if probeCalls != 0 {
		t.Fatalf("probeCalls = %d, want 0", probeCalls)
	}
	if server.Healthy {
		t.Fatal("Healthy = true, want false")
	}
	if server.Status != observability.StatusUnknown {
		t.Fatalf("Status = %q, want %q", server.Status, observability.StatusUnknown)
	}
	if server.Checked {
		t.Fatal("Checked = true, want false")
	}
	if server.Reason != turnReasonInvalidServer {
		t.Fatalf("Reason = %q, want %q", server.Reason, turnReasonInvalidServer)
	}
	if server.LastError == "" {
		t.Fatal("LastError should not be empty")
	}
}

func TestServers_ListNotConfigured(t *testing.T) {
	router := newTestRouterWithHandler(newHandlerWithProbe(Config{}, time.Second, nil))
	servers := getServers(t, router)
	if len(servers) != 1 {
		t.Fatalf("len(servers) = %d, want 1", len(servers))
	}
	server := servers[0]
	if server.Status != observability.StatusUnknown {
		t.Fatalf("Status = %q, want %q", server.Status, observability.StatusUnknown)
	}
	if server.Checked {
		t.Fatal("Checked = true, want false")
	}
	if server.Reason != turnReasonNotConfigured {
		t.Fatalf("Reason = %q, want %q", server.Reason, turnReasonNotConfigured)
	}
}

func TestProbeTURNServer_HealthyBindingResponse(t *testing.T) {
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ListenPacket: %v", err)
	}
	defer conn.Close()

	done := make(chan struct{})
	go func() {
		defer close(done)
		buf := make([]byte, 256)
		n, addr, err := conn.ReadFrom(buf)
		if err != nil {
			return
		}
		if n < 20 {
			return
		}
		response := make([]byte, 20)
		binary.BigEndian.PutUint16(response[0:2], stunBindingSuccessType)
		binary.BigEndian.PutUint16(response[2:4], 0)
		binary.BigEndian.PutUint32(response[4:8], stunMagicCookie)
		copy(response[8:20], buf[8:20])
		_, _ = conn.WriteTo(response, addr)
	}()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()

	result := probeTURNServer(ctx, conn.LocalAddr().String())
	if result.status != observability.StatusHealthy {
		t.Fatalf("status = %q, want %q", result.status, observability.StatusHealthy)
	}
	if !result.checked {
		t.Fatal("checked = false, want true")
	}
	if result.reason != turnReasonBindingSuccess {
		t.Fatalf("reason = %q, want %q", result.reason, turnReasonBindingSuccess)
	}
	if result.latencyMS == nil {
		t.Fatal("latencyMS should not be nil")
	}
	<-done
}

func getServers(t *testing.T, router http.Handler) []TURNServer {
	t.Helper()

	req := httptest.NewRequest("GET", "/turn/servers", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var servers []TURNServer
	if err := json.NewDecoder(w.Body).Decode(&servers); err != nil {
		t.Fatalf("decode error: %v", err)
	}
	return servers
}
