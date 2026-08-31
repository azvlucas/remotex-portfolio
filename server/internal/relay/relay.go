package relay

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	redispkg "github.com/azvlucas/remotex-portfolio/server/pkg/redis"
)

const (
	RoleHost   = "host"
	RoleViewer = "viewer"
)

type relayRedisBus interface {
	Get(key string) (string, error)
	Set(key, value string, ttl time.Duration) error
	Del(keys ...string) error
}

type Relay struct {
	mu       sync.Mutex
	sessions map[string]*session

	redisBus   relayRedisBus
	nodeID     string
	relayPort  string
	authSecret []byte

	stateMu         sync.RWMutex
	listeningAddr   string
	lastListenError string

	peerWaitTimeout         time.Duration
	peerPollInterval        time.Duration
	crossNodeWaitTimeout    time.Duration
	crossNodeWaitPoll       time.Duration
	crossNodeConnectTimeout time.Duration
}

type session struct {
	host   *peerConn
	viewer *peerConn
	ready  chan struct{}
}

type peerConn struct {
	conn  net.Conn
	hello relayHello
}

type relayHello struct {
	Version      int
	Role         string
	Lane         string
	SessionID    string
	ExpiresAt    int64
	Capabilities uint32
	Ticket       string
}

// New creates a relay. redisBus can be nil for single-node mode.
func New(redisBus *redispkg.PubSub, nodeID, relayPort, authSecret string) *Relay {
	var bus relayRedisBus
	if redisBus != nil {
		bus = redisBus
	}
	return &Relay{
		sessions:                make(map[string]*session),
		redisBus:                bus,
		nodeID:                  nodeID,
		relayPort:               relayPort,
		authSecret:              []byte(authSecret),
		peerWaitTimeout:         30 * time.Second,
		peerPollInterval:        500 * time.Millisecond,
		crossNodeWaitTimeout:    30 * time.Second,
		crossNodeWaitPoll:       250 * time.Millisecond,
		crossNodeConnectTimeout: 10 * time.Second,
	}
}

func (r *Relay) ListenAndServe(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		r.markListenError(err)
		observability.RecordRelayDegradation(observability.RelaySurfaceListen, observability.RelayReasonError)
		return err
	}
	defer func() {
		r.clearListening()
		_ = ln.Close()
	}()
	r.markListening(addr)
	log.Printf("[relay] tcp relay listening on %s (node=%s)", addr, r.nodeID)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("[relay] accept error: %v", err)
			continue
		}
		go r.handleConn(conn)
	}
}

// HealthStatus returns a bounded machine-readable snapshot for signaling /health.
func (r *Relay) HealthStatus() observability.ComponentStatus {
	checkedAt := time.Now().UTC()
	r.stateMu.RLock()
	listeningAddr := r.listeningAddr
	lastListenError := r.lastListenError
	r.stateMu.RUnlock()

	switch {
	case listeningAddr != "":
		return observability.ComponentStatus{
			Status:    observability.StatusHealthy,
			Mode:      "tcp_listener",
			Detail:    "relay listener active",
			CheckedAt: &checkedAt,
		}
	case strings.TrimSpace(lastListenError) != "":
		return observability.ComponentStatus{
			Status:    observability.StatusDegraded,
			Mode:      "startup_failed",
			Detail:    "relay listener failed",
			LastError: lastListenError,
			CheckedAt: &checkedAt,
		}
	default:
		return observability.ComponentStatus{
			Status:    observability.StatusStarting,
			Mode:      "starting",
			Detail:    "relay listener not ready",
			CheckedAt: &checkedAt,
		}
	}
}

// connWithReader wraps a net.Conn with a bufio.Reader so buffered data is preserved.
type connWithReader struct {
	io.Reader
	net.Conn
}

func (c connWithReader) Read(p []byte) (int, error) {
	return c.Reader.Read(p)
}

func relaySessionKey(sessionID, lane string) string {
	if lane == "" || lane == "main" {
		return sessionID
	}
	return sessionID + "|" + lane
}

func (r *Relay) handleConn(conn net.Conn) {
	reader := bufio.NewReader(conn)
	line, err := reader.ReadString('\n')
	if err != nil {
		conn.Close()
		return
	}

	hello, err := parseHello(line)
	if err != nil {
		observability.RecordRelayDegradation(observability.RelaySurfaceHello, observability.RelayReasonMalformed)
		log.Printf("[relay] invalid hello from %s: %v", conn.RemoteAddr(), err)
		conn.Close()
		return
	}
	if err := r.validateHello(hello); err != nil {
		observability.RecordRelayDegradation(observability.RelaySurfaceHello, observability.RelayReasonRejected)
		log.Printf("[relay] rejected hello session=%s role=%s lane=%s: %v",
			hello.SessionID, hello.Role, hello.Lane, err)
		conn.Close()
		return
	}

	sessionKey := relaySessionKey(hello.SessionID, hello.Lane)
	peer := &peerConn{
		conn:  connWithReader{Reader: reader, Conn: conn},
		hello: hello,
	}

	log.Printf("[relay] %s connected for session %s lane=%s (node=%s)",
		hello.Role, hello.SessionID, hello.Lane, r.nodeID)

	r.registerInRedis(hello.SessionID, hello.Lane, hello.Role)

	r.mu.Lock()
	s, found := r.sessions[sessionKey]
	if !found {
		s = &session{ready: make(chan struct{})}
		r.sessions[sessionKey] = s
	}

	switch hello.Role {
	case RoleHost:
		if s.host != nil {
			s.host.conn.Close()
		}
		s.host = peer
	case RoleViewer:
		if s.viewer != nil {
			s.viewer.conn.Close()
		}
		s.viewer = peer
	}

	if s.host != nil && s.viewer != nil {
		select {
		case <-s.ready:
		default:
			close(s.ready)
		}
		r.mu.Unlock()
		r.bridgeSession(hello.SessionID, hello.Lane, sessionKey, s)
		return
	}

	readyCh := s.ready
	r.mu.Unlock()

	if r.redisBus != nil {
		r.awaitPeer(hello.SessionID, hello.Lane, hello.Role, sessionKey, peer, readyCh)
	}
}

// awaitPeer waits for a local peer or discovers a remote node through Redis.
func (r *Relay) awaitPeer(sessionID, lane, role, sessionKey string, localPeer *peerConn, ready <-chan struct{}) {
	otherRole := RoleViewer
	if role == RoleViewer {
		otherRole = RoleHost
	}

	deadline := time.Now().Add(r.peerWaitDeadline())
	for time.Now().Before(deadline) {
		select {
		case <-ready:
			return
		default:
		}

		key := fmt.Sprintf("relay:session:%s:%s:%s", sessionID, lane, otherRole)
		node, err := r.redisBus.Get(key)
		if err == nil && node != "" && node != r.nodeID {
			r.doCrossNodeBridge(sessionID, lane, role, sessionKey, localPeer, ready, node)
			return
		}

		time.Sleep(r.peerPollSleep())
	}

	select {
	case <-ready:
		return
	default:
	}

	observability.RecordRelayDegradation(observability.RelaySurfaceNoPeer, observability.RelayReasonTimeout)
	log.Printf("[relay] session %s lane=%s: no peer found for %s, closing (node=%s)",
		sessionID, lane, role, r.nodeID)
	localPeer.conn.Close()
	r.mu.Lock()
	delete(r.sessions, sessionKey)
	r.mu.Unlock()
	r.unregisterFromRedis(sessionID, lane, role)
}

func (r *Relay) bridgeSession(sessionID, lane, sessionKey string, s *session) {
	host := s.host.conn
	viewer := s.viewer.conn

	host.Write([]byte("READY\n"))
	viewer.Write([]byte("READY\n"))

	log.Printf("[relay] session %s lane=%s: relay started (node=%s)", sessionID, lane, r.nodeID)

	done := make(chan struct{}, 2)
	go func() {
		io.Copy(viewer, host)
		done <- struct{}{}
	}()
	go func() {
		io.Copy(host, viewer)
		done <- struct{}{}
	}()

	<-done
	host.Close()
	viewer.Close()
	<-done

	log.Printf("[relay] session %s lane=%s: relay ended (node=%s)", sessionID, lane, r.nodeID)

	r.mu.Lock()
	delete(r.sessions, sessionKey)
	r.mu.Unlock()

	r.unregisterFromRedis(sessionID, lane, RoleHost)
	r.unregisterFromRedis(sessionID, lane, RoleViewer)
}

// doCrossNodeBridge connects to a remote relay node and bridges the local client through it.
func (r *Relay) doCrossNodeBridge(sessionID, lane, localRole, sessionKey string, localPeer *peerConn, ready <-chan struct{}, remoteNode string) {
	if r.nodeID > remoteNode {
		log.Printf("[relay] session %s lane=%s: waiting for cross-node proxy from %s (node=%s)",
			sessionID, lane, remoteNode, r.nodeID)
		waitDeadline := time.Now().Add(r.crossNodeWaitDeadline())
		for time.Now().Before(waitDeadline) {
			select {
			case <-ready:
				return
			default:
			}
			time.Sleep(r.crossNodeWaitPollInterval())
		}
		observability.RecordRelayDegradation(observability.RelaySurfaceCrossNodeWait, observability.RelayReasonTimeout)
		log.Printf("[relay] session %s lane=%s: cross-node proxy wait timed out (node=%s)",
			sessionID, lane, r.nodeID)
		localPeer.conn.Close()
		r.mu.Lock()
		delete(r.sessions, sessionKey)
		r.mu.Unlock()
		r.unregisterFromRedis(sessionID, lane, localRole)
		return
	}

	remoteAddr := net.JoinHostPort(remoteNode, r.relayPort)
	log.Printf("[relay] session %s lane=%s: initiating cross-node bridge to %s as %s (node=%s)",
		sessionID, lane, remoteAddr, localRole, r.nodeID)

	remoteConn, err := net.DialTimeout("tcp", remoteAddr, r.crossNodeDialTimeout())
	if err != nil {
		observability.RecordRelayDegradation(observability.RelaySurfaceCrossNodeConnect, observability.RelayReasonError)
		log.Printf("[relay] session %s lane=%s: cross-node connect to %s failed: %v",
			sessionID, lane, remoteAddr, err)
		localPeer.conn.Close()
		r.mu.Lock()
		delete(r.sessions, sessionKey)
		r.mu.Unlock()
		r.unregisterFromRedis(sessionID, lane, localRole)
		return
	}

	hello := localPeer.hello
	hello.Role = localRole
	hello.Lane = lane
	if _, err := remoteConn.Write([]byte(formatHello(hello))); err != nil {
		observability.RecordRelayDegradation(observability.RelaySurfaceCrossNodeHandshake, observability.RelayReasonError)
		log.Printf("[relay] session %s lane=%s: cross-node handshake failed: %v", sessionID, lane, err)
		remoteConn.Close()
		localPeer.conn.Close()
		r.mu.Lock()
		delete(r.sessions, sessionKey)
		r.mu.Unlock()
		r.unregisterFromRedis(sessionID, lane, localRole)
		return
	}

	remoteReader := bufio.NewReader(remoteConn)
	okLine, err := remoteReader.ReadString('\n')
	if err != nil || strings.TrimSpace(okLine) != "READY" {
		observability.RecordRelayDegradation(observability.RelaySurfaceCrossNodeHandshake, observability.RelayReasonError)
		log.Printf("[relay] session %s lane=%s: cross-node relay did not acknowledge: %v", sessionID, lane, err)
		remoteConn.Close()
		localPeer.conn.Close()
		r.mu.Lock()
		delete(r.sessions, sessionKey)
		r.mu.Unlock()
		r.unregisterFromRedis(sessionID, lane, localRole)
		return
	}

	localPeer.conn.Write([]byte("READY\n"))

	log.Printf("[relay] session %s lane=%s: cross-node bridge established via %s (node=%s)",
		sessionID, lane, remoteNode, r.nodeID)

	remoteWrapped := connWithReader{Reader: remoteReader, Conn: remoteConn}
	done := make(chan struct{}, 2)
	go func() {
		io.Copy(remoteWrapped, localPeer.conn)
		done <- struct{}{}
	}()
	go func() {
		io.Copy(localPeer.conn, remoteWrapped)
		done <- struct{}{}
	}()

	<-done
	localPeer.conn.Close()
	remoteConn.Close()
	<-done

	log.Printf("[relay] session %s lane=%s: cross-node bridge ended (node=%s)", sessionID, lane, r.nodeID)

	r.mu.Lock()
	delete(r.sessions, sessionKey)
	r.mu.Unlock()
	r.unregisterFromRedis(sessionID, lane, localRole)
}

func (r *Relay) registerInRedis(sessionID, lane, role string) {
	if r.redisBus == nil {
		return
	}
	key := fmt.Sprintf("relay:session:%s:%s:%s", sessionID, lane, role)
	if err := r.redisBus.Set(key, r.nodeID, 120*time.Second); err != nil {
		log.Printf("[relay] failed to register in redis: %v", err)
	}
}

func (r *Relay) unregisterFromRedis(sessionID, lane, role string) {
	if r.redisBus == nil {
		return
	}
	key := fmt.Sprintf("relay:session:%s:%s:%s", sessionID, lane, role)
	r.redisBus.Del(key)
}

func parseHello(line string) (relayHello, error) {
	fields := strings.Fields(strings.TrimSpace(line))
	if len(fields) != 8 {
		return relayHello{}, errors.New("expected 8 hello fields")
	}
	if fields[0] != "HELLO" {
		return relayHello{}, errors.New("missing HELLO prefix")
	}

	version, err := strconv.Atoi(fields[1])
	if err != nil {
		return relayHello{}, fmt.Errorf("invalid version: %w", err)
	}
	role := strings.TrimSpace(fields[2])
	if role != RoleHost && role != RoleViewer {
		return relayHello{}, errors.New("invalid role")
	}
	lane := strings.TrimSpace(fields[3])
	if lane == "" {
		return relayHello{}, errors.New("missing lane")
	}
	sessionID := strings.TrimSpace(fields[4])
	if sessionID == "" {
		return relayHello{}, errors.New("missing session id")
	}
	expiresAt, err := strconv.ParseInt(fields[5], 10, 64)
	if err != nil {
		return relayHello{}, fmt.Errorf("invalid expiry: %w", err)
	}
	caps, err := strconv.ParseUint(fields[6], 10, 32)
	if err != nil {
		return relayHello{}, fmt.Errorf("invalid capabilities: %w", err)
	}
	ticket := strings.TrimSpace(fields[7])
	if ticket == "" {
		return relayHello{}, errors.New("missing ticket")
	}

	return relayHello{
		Version:      version,
		Role:         role,
		Lane:         lane,
		SessionID:    sessionID,
		ExpiresAt:    expiresAt,
		Capabilities: uint32(caps),
		Ticket:       ticket,
	}, nil
}

func formatHello(hello relayHello) string {
	return fmt.Sprintf("HELLO %d %s %s %s %d %d %s\n",
		hello.Version,
		hello.Role,
		hello.Lane,
		hello.SessionID,
		hello.ExpiresAt,
		hello.Capabilities,
		hello.Ticket,
	)
}

func (r *Relay) validateHello(hello relayHello) error {
	if len(r.authSecret) == 0 {
		return errors.New("relay auth secret not configured")
	}
	if hello.Version != ProtocolVersion {
		return fmt.Errorf("unsupported protocol version %d", hello.Version)
	}

	claims, err := ValidateTicket(r.authSecret, hello.Ticket, hello.Role, hello.SessionID, time.Now())
	if err != nil {
		return err
	}
	if claims.ExpiresAt != hello.ExpiresAt {
		return errors.New("ticket expiry mismatch")
	}
	return nil
}

func (r *Relay) markListening(addr string) {
	r.stateMu.Lock()
	defer r.stateMu.Unlock()
	r.listeningAddr = strings.TrimSpace(addr)
	r.lastListenError = ""
}

func (r *Relay) clearListening() {
	r.stateMu.Lock()
	defer r.stateMu.Unlock()
	r.listeningAddr = ""
}

func (r *Relay) markListenError(err error) {
	r.stateMu.Lock()
	defer r.stateMu.Unlock()
	r.listeningAddr = ""
	if err != nil {
		r.lastListenError = err.Error()
		return
	}
	r.lastListenError = ""
}

func (r *Relay) peerWaitDeadline() time.Duration {
	if r.peerWaitTimeout > 0 {
		return r.peerWaitTimeout
	}
	return 30 * time.Second
}

func (r *Relay) peerPollSleep() time.Duration {
	if r.peerPollInterval > 0 {
		return r.peerPollInterval
	}
	return 500 * time.Millisecond
}

func (r *Relay) crossNodeWaitDeadline() time.Duration {
	if r.crossNodeWaitTimeout > 0 {
		return r.crossNodeWaitTimeout
	}
	return 30 * time.Second
}

func (r *Relay) crossNodeWaitPollInterval() time.Duration {
	if r.crossNodeWaitPoll > 0 {
		return r.crossNodeWaitPoll
	}
	return 250 * time.Millisecond
}

func (r *Relay) crossNodeDialTimeout() time.Duration {
	if r.crossNodeConnectTimeout > 0 {
		return r.crossNodeConnectTimeout
	}
	return 10 * time.Second
}
