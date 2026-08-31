package relay

import (
	"bufio"
	"net"
	"strconv"
	"testing"
	"time"

	dto "github.com/prometheus/client_model/go"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

type fakeRelayRedisBus struct {
	getValue string
	getErr   error
	delKeys  []string
}

func (f *fakeRelayRedisBus) Get(string) (string, error) {
	return f.getValue, f.getErr
}

func (f *fakeRelayRedisBus) Set(string, string, time.Duration) error {
	return nil
}

func (f *fakeRelayRedisBus) Del(keys ...string) error {
	f.delKeys = append(f.delKeys, keys...)
	return nil
}

func newRelayTestMetrics(t *testing.T) *observability.Metrics {
	t.Helper()

	metrics, err := observability.NewMetrics("relay-test")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}
	return metrics
}

func relayMetricValue(t *testing.T, metrics *observability.Metrics, surface, reason string) float64 {
	t.Helper()

	families, err := metrics.Registry.Gather()
	if err != nil {
		t.Fatalf("Gather() error = %v", err)
	}
	for _, family := range families {
		if family.GetName() != "remotex_relay_degradation_events_total" {
			continue
		}
		for _, metric := range family.GetMetric() {
			if relayLabelValue(metric.GetLabel(), "surface") == surface && relayLabelValue(metric.GetLabel(), "reason") == reason {
				return metric.GetCounter().GetValue()
			}
		}
		return 0
	}
	return 0
}

func relayLabelValue(labels []*dto.LabelPair, name string) string {
	for _, label := range labels {
		if label.GetName() == name {
			return label.GetValue()
		}
	}
	return ""
}

func TestListenAndServeRecordsListenErrors(t *testing.T) {
	metrics := newRelayTestMetrics(t)
	r := New(nil, "node-a", "9100", "secret")

	err := r.ListenAndServe("127.0.0.1:bad")
	if err == nil {
		t.Fatal("ListenAndServe() error = nil, want listen failure")
	}
	if got := relayMetricValue(t, metrics, observability.RelaySurfaceListen, observability.RelayReasonError); got != 1 {
		t.Fatalf("listen error counter = %v, want 1", got)
	}
	if status := r.HealthStatus(); status.Status != observability.StatusDegraded {
		t.Fatalf("HealthStatus().Status = %q, want %q", status.Status, observability.StatusDegraded)
	}
}

func TestHandleConnRecordsMalformedAndRejectedHello(t *testing.T) {
	t.Run("malformed", func(t *testing.T) {
		metrics := newRelayTestMetrics(t)
		r := New(nil, "node-a", "9100", "secret")
		serverConn, clientConn := net.Pipe()
		defer clientConn.Close()

		done := make(chan struct{})
		go func() {
			defer close(done)
			r.handleConn(serverConn)
		}()

		if _, err := clientConn.Write([]byte("HELLO 1 host\n")); err != nil {
			t.Fatalf("Write() error = %v", err)
		}
		clientConn.Close()
		<-done

		if got := relayMetricValue(t, metrics, observability.RelaySurfaceHello, observability.RelayReasonMalformed); got != 1 {
			t.Fatalf("malformed hello counter = %v, want 1", got)
		}
	})

	t.Run("rejected", func(t *testing.T) {
		metrics := newRelayTestMetrics(t)
		r := New(nil, "node-a", "9100", "secret")
		serverConn, clientConn := net.Pipe()
		defer clientConn.Close()

		done := make(chan struct{})
		go func() {
			defer close(done)
			r.handleConn(serverConn)
		}()

		line := "HELLO 1 host main session-1 " + strconv.FormatInt(time.Now().Add(time.Hour).Unix(), 10) + " 0 invalid-ticket\n"
		if _, err := clientConn.Write([]byte(line)); err != nil {
			t.Fatalf("Write() error = %v", err)
		}
		clientConn.Close()
		<-done

		if got := relayMetricValue(t, metrics, observability.RelaySurfaceHello, observability.RelayReasonRejected); got != 1 {
			t.Fatalf("rejected hello counter = %v, want 1", got)
		}
	})
}

func TestAwaitPeerRecordsNoPeerTimeout(t *testing.T) {
	metrics := newRelayTestMetrics(t)
	bus := &fakeRelayRedisBus{}
	r := New(nil, "node-a", "9100", "secret")
	r.redisBus = bus
	r.peerWaitTimeout = 5 * time.Millisecond
	r.peerPollInterval = 1 * time.Millisecond

	localConn, peerSide := net.Pipe()
	defer peerSide.Close()
	sessionKey := relaySessionKey("session-1", "main")
	r.sessions[sessionKey] = &session{ready: make(chan struct{})}

	r.awaitPeer("session-1", "main", RoleHost, sessionKey, &peerConn{conn: localConn}, make(chan struct{}))

	if got := relayMetricValue(t, metrics, observability.RelaySurfaceNoPeer, observability.RelayReasonTimeout); got != 1 {
		t.Fatalf("no peer timeout counter = %v, want 1", got)
	}
	if _, ok := r.sessions[sessionKey]; ok {
		t.Fatal("session still registered after no-peer timeout")
	}
}

func TestDoCrossNodeBridgeRecordsWaitTimeout(t *testing.T) {
	metrics := newRelayTestMetrics(t)
	r := New(nil, "node-z", "9100", "secret")
	r.crossNodeWaitTimeout = 5 * time.Millisecond
	r.crossNodeWaitPoll = 1 * time.Millisecond

	localConn, peerSide := net.Pipe()
	defer peerSide.Close()
	sessionKey := relaySessionKey("session-1", "main")
	r.sessions[sessionKey] = &session{ready: make(chan struct{})}

	r.doCrossNodeBridge("session-1", "main", RoleHost, sessionKey, &peerConn{conn: localConn}, make(chan struct{}), "node-a")

	if got := relayMetricValue(t, metrics, observability.RelaySurfaceCrossNodeWait, observability.RelayReasonTimeout); got != 1 {
		t.Fatalf("cross-node wait timeout counter = %v, want 1", got)
	}
}

func TestDoCrossNodeBridgeRecordsConnectError(t *testing.T) {
	metrics := newRelayTestMetrics(t)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("Listen() error = %v", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()

	r := New(nil, "0", strconv.Itoa(port), "secret")
	r.crossNodeConnectTimeout = 20 * time.Millisecond
	localConn, peerSide := net.Pipe()
	defer peerSide.Close()
	sessionKey := relaySessionKey("session-1", "main")
	r.sessions[sessionKey] = &session{ready: make(chan struct{})}

	hello := relayHello{Version: ProtocolVersion, Role: RoleHost, Lane: "main", SessionID: "session-1", ExpiresAt: time.Now().Add(time.Hour).Unix(), Ticket: "ticket"}
	r.doCrossNodeBridge("session-1", "main", RoleHost, sessionKey, &peerConn{conn: localConn, hello: hello}, make(chan struct{}), "127.0.0.1")

	if got := relayMetricValue(t, metrics, observability.RelaySurfaceCrossNodeConnect, observability.RelayReasonError); got != 1 {
		t.Fatalf("cross-node connect error counter = %v, want 1", got)
	}
}

func TestDoCrossNodeBridgeRecordsHandshakeError(t *testing.T) {
	metrics := newRelayTestMetrics(t)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("Listen() error = %v", err)
	}
	defer ln.Close()

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		reader := bufio.NewReader(conn)
		_, _ = reader.ReadString('\n')
		_, _ = conn.Write([]byte("NOPE\n"))
	}()

	port := ln.Addr().(*net.TCPAddr).Port
	r := New(nil, "0", strconv.Itoa(port), "secret")
	localConn, peerSide := net.Pipe()
	defer peerSide.Close()
	sessionKey := relaySessionKey("session-1", "main")
	r.sessions[sessionKey] = &session{ready: make(chan struct{})}

	hello := relayHello{Version: ProtocolVersion, Role: RoleHost, Lane: "main", SessionID: "session-1", ExpiresAt: time.Now().Add(time.Hour).Unix(), Ticket: "ticket"}
	r.doCrossNodeBridge("session-1", "main", RoleHost, sessionKey, &peerConn{conn: localConn, hello: hello}, make(chan struct{}), "127.0.0.1")

	if got := relayMetricValue(t, metrics, observability.RelaySurfaceCrossNodeHandshake, observability.RelayReasonError); got != 1 {
		t.Fatalf("cross-node handshake error counter = %v, want 1", got)
	}
}
