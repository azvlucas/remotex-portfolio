package observability

import (
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/go-chi/chi/v5"
	chimiddleware "github.com/go-chi/chi/v5/middleware"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

const metricNamespace = "remotex"

const (
	AuthAbuseSurfacePublicLogin   = "public_login"
	AuthAbuseSurfaceAdminPassword = "admin_password"
	AuthAbuseSurfaceAdminTOTP     = "admin_totp"
	AuthAbuseSurfaceUnknown       = "unknown"

	AuthAbuseReasonLimited    = "limited"
	AuthAbuseReasonRejected   = "rejected"
	AuthAbuseReasonStoreError = "store_error"
	authAbuseReasonOther      = "other"
)

const (
	SignalingSurfaceWebSocketAuth    = "websocket_auth"
	SignalingSurfaceWebSocketUpgrade = "websocket_upgrade"
	SignalingSurfaceSessionJoin      = "session_join"
	SignalingSurfaceSessionRelay     = "session_relay"
	SignalingSurfaceChatJoin         = "chat_join"
	SignalingSurfaceChatMessage      = "chat_message"
	signalingSurfaceUnknown          = "unknown"

	SignalingReasonUnauthorized        = "unauthorized"
	SignalingReasonForbidden           = "forbidden"
	SignalingReasonMissingClaims       = "missing_claims"
	SignalingReasonUpgradeFailed       = "upgrade_failed"
	SignalingReasonAccessDenied        = "access_denied"
	SignalingReasonAuthorizationFailed = "authorization_failed"
	SignalingReasonMissingSessionID    = "missing_session_id"
	SignalingReasonInvalidRole         = "invalid_role"
	SignalingReasonNotInSession        = "not_in_session"
	SignalingReasonPeerNotConnected    = "peer_not_connected"
	SignalingReasonMissingMachineID    = "missing_machine_id"
	SignalingReasonMachineNotConnected = "machine_not_connected"
	signalingReasonOther               = "other"
)

const (
	RelaySurfaceListen             = "listen"
	RelaySurfaceHello              = "hello"
	RelaySurfaceNoPeer             = "no_peer"
	RelaySurfaceCrossNodeWait      = "cross_node_wait"
	RelaySurfaceCrossNodeConnect   = "cross_node_connect"
	RelaySurfaceCrossNodeHandshake = "cross_node_handshake"
	relaySurfaceUnknown            = "unknown"

	RelayReasonError     = "error"
	RelayReasonTimeout   = "timeout"
	RelayReasonMalformed = "malformed"
	RelayReasonRejected  = "rejected"
	relayReasonOther     = "other"
)

type authAbuseRecorder struct {
	counter *prometheus.CounterVec
}

func (r *authAbuseRecorder) Inc(surface, reason string) {
	if r == nil || r.counter == nil {
		return
	}
	r.counter.WithLabelValues(normalizeAuthAbuseSurface(surface), normalizeAuthAbuseReason(reason)).Inc()
}

type signalingRecorder struct {
	events            *prometheus.CounterVec
	activeConnections prometheus.Gauge
}

func (r *signalingRecorder) Inc(surface, reason string) {
	if r == nil || r.events == nil {
		return
	}
	r.events.WithLabelValues(normalizeSignalingSurface(surface), normalizeSignalingReason(reason)).Inc()
}

func (r *signalingRecorder) SetActiveConnections(count int) {
	if r == nil || r.activeConnections == nil {
		return
	}
	if count < 0 {
		count = 0
	}
	r.activeConnections.Set(float64(count))
}

type relayRecorder struct {
	degradations *prometheus.CounterVec
}

func (r *relayRecorder) Inc(surface, reason string) {
	if r == nil || r.degradations == nil {
		return
	}
	r.degradations.WithLabelValues(normalizeRelaySurface(surface), normalizeRelayReason(reason)).Inc()
}

var currentAuthAbuseRecorder atomic.Pointer[authAbuseRecorder]
var currentSignalingRecorder atomic.Pointer[signalingRecorder]
var currentRelayRecorder atomic.Pointer[relayRecorder]

func init() {
	currentAuthAbuseRecorder.Store(&authAbuseRecorder{})
	currentSignalingRecorder.Store(&signalingRecorder{})
	currentRelayRecorder.Store(&relayRecorder{})
}

// Metrics owns a custom Prometheus registry plus the shared HTTP metrics.
type Metrics struct {
	Registry                   *prometheus.Registry
	requestsTotal              *prometheus.CounterVec
	requestDuration            *prometheus.HistogramVec
	serviceInfo                *prometheus.GaugeVec
	authAbuseEvents            *prometheus.CounterVec
	signalingEvents            *prometheus.CounterVec
	signalingActiveConnections prometheus.Gauge
	relayDegradationEvents     *prometheus.CounterVec
}

// NewMetrics builds a registry with Go, process, build, and shared HTTP collectors.
func NewMetrics(service string) (*Metrics, error) {
	service = normalizeToken(service, "unknown")
	registry := prometheus.NewRegistry()
	m := &Metrics{
		Registry: registry,
		requestsTotal: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Namespace: metricNamespace,
				Name:      "http_requests_total",
				Help:      "Total HTTP requests handled by method, route, and status.",
			},
			[]string{"method", "route", "status"},
		),
		requestDuration: prometheus.NewHistogramVec(
			prometheus.HistogramOpts{
				Namespace: metricNamespace,
				Name:      "http_request_duration_seconds",
				Help:      "HTTP request duration in seconds by method, route, and status.",
				Buckets:   prometheus.DefBuckets,
			},
			[]string{"method", "route", "status"},
		),
		serviceInfo: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Namespace: metricNamespace,
				Name:      "service_info",
				Help:      "Static service identity for the running binary.",
			},
			[]string{"service"},
		),
		authAbuseEvents: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Namespace: metricNamespace,
				Name:      "auth_abuse_events_total",
				Help:      "Total auth abuse and limiter failure events by surface and reason.",
			},
			[]string{"surface", "reason"},
		),
		signalingEvents: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Namespace: metricNamespace,
				Name:      "signaling_events_total",
				Help:      "Total signaling websocket and denial events by surface and reason.",
			},
			[]string{"surface", "reason"},
		),
		signalingActiveConnections: prometheus.NewGauge(
			prometheus.GaugeOpts{
				Namespace: metricNamespace,
				Name:      "signaling_active_connections",
				Help:      "Current number of active authenticated signaling websocket connections.",
			},
		),
		relayDegradationEvents: prometheus.NewCounterVec(
			prometheus.CounterOpts{
				Namespace: metricNamespace,
				Name:      "relay_degradation_events_total",
				Help:      "Total relay degradation events by surface and reason.",
			},
			[]string{"surface", "reason"},
		),
	}

	collectorsToRegister := []prometheus.Collector{
		collectors.NewGoCollector(),
		collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}),
		collectors.NewBuildInfoCollector(),
		m.requestsTotal,
		m.requestDuration,
		m.serviceInfo,
		m.authAbuseEvents,
		m.signalingEvents,
		m.signalingActiveConnections,
		m.relayDegradationEvents,
	}
	for _, collector := range collectorsToRegister {
		if err := m.Register(collector); err != nil {
			return nil, err
		}
	}

	m.serviceInfo.WithLabelValues(service).Set(1)
	primeCounterVecLabels(m.authAbuseEvents,
		[]string{AuthAbuseSurfacePublicLogin, AuthAbuseSurfaceAdminPassword, AuthAbuseSurfaceAdminTOTP, AuthAbuseSurfaceUnknown},
		[]string{AuthAbuseReasonLimited, AuthAbuseReasonRejected, AuthAbuseReasonStoreError, authAbuseReasonOther},
	)
	primeCounterVecLabels(m.signalingEvents,
		[]string{SignalingSurfaceWebSocketAuth, SignalingSurfaceWebSocketUpgrade, SignalingSurfaceSessionJoin, SignalingSurfaceSessionRelay, SignalingSurfaceChatJoin, SignalingSurfaceChatMessage, signalingSurfaceUnknown},
		[]string{SignalingReasonUnauthorized, SignalingReasonForbidden, SignalingReasonMissingClaims, SignalingReasonUpgradeFailed, SignalingReasonAccessDenied, SignalingReasonAuthorizationFailed, SignalingReasonMissingSessionID, SignalingReasonInvalidRole, SignalingReasonNotInSession, SignalingReasonPeerNotConnected, SignalingReasonMissingMachineID, SignalingReasonMachineNotConnected, signalingReasonOther},
	)
	primeCounterVecLabels(m.relayDegradationEvents,
		[]string{RelaySurfaceListen, RelaySurfaceHello, RelaySurfaceNoPeer, RelaySurfaceCrossNodeWait, RelaySurfaceCrossNodeConnect, RelaySurfaceCrossNodeHandshake, relaySurfaceUnknown},
		[]string{RelayReasonError, RelayReasonTimeout, RelayReasonMalformed, RelayReasonRejected, relayReasonOther},
	)
	currentAuthAbuseRecorder.Store(&authAbuseRecorder{counter: m.authAbuseEvents})
	currentSignalingRecorder.Store(&signalingRecorder{events: m.signalingEvents, activeConnections: m.signalingActiveConnections})
	currentRelayRecorder.Store(&relayRecorder{degradations: m.relayDegradationEvents})
	return m, nil
}

func primeCounterVecLabels(counter *prometheus.CounterVec, primaryValues, secondaryValues []string) {
	if counter == nil {
		return
	}
	for _, primary := range primaryValues {
		for _, secondary := range secondaryValues {
			counter.WithLabelValues(primary, secondary).Add(0)
		}
	}
}

// Register adds a collector to the custom registry and returns duplicate/invalid registration errors.
func (m *Metrics) Register(collector prometheus.Collector) error {
	if err := m.Registry.Register(collector); err != nil {
		return fmt.Errorf("register collector: %w", err)
	}
	return nil
}

// Handler exposes the custom registry through the official Prometheus HTTP handler.
func (m *Metrics) Handler() http.Handler {
	return promhttp.HandlerFor(m.Registry, promhttp.HandlerOpts{
		Registry:      m.Registry,
		ErrorHandling: promhttp.HTTPErrorOnError,
	})
}

// Middleware records bounded method/route/status request labels for the wrapped handler.
func (m *Metrics) Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		wrapped := chimiddleware.NewWrapResponseWriter(w, r.ProtoMajor)
		next.ServeHTTP(wrapped, r)

		method := normalizeMethod(r.Method)
		status := normalizeStatusCode(wrapped.Status())
		route := normalizeRouteLabel(r)

		m.requestsTotal.WithLabelValues(method, route, status).Inc()
		m.requestDuration.WithLabelValues(method, route, status).Observe(time.Since(start).Seconds())
	})
}

// RecordAuthAbuse increments the shared bounded auth abuse counter when a registry is active.
func RecordAuthAbuse(surface, reason string) {
	currentAuthAbuseRecorder.Load().Inc(surface, reason)
}

// RecordSignalingEvent increments the shared bounded signaling event counter when a registry is active.
func RecordSignalingEvent(surface, reason string) {
	currentSignalingRecorder.Load().Inc(surface, reason)
}

// SetSignalingActiveConnections updates the shared active websocket gauge when a registry is active.
func SetSignalingActiveConnections(count int) {
	currentSignalingRecorder.Load().SetActiveConnections(count)
}

// RecordRelayDegradation increments the shared bounded relay degradation counter when a registry is active.
func RecordRelayDegradation(surface, reason string) {
	currentRelayRecorder.Load().Inc(surface, reason)
}

func normalizeMethod(method string) string {
	switch strings.ToUpper(strings.TrimSpace(method)) {
	case http.MethodGet,
		http.MethodHead,
		http.MethodPost,
		http.MethodPut,
		http.MethodPatch,
		http.MethodDelete,
		http.MethodOptions,
		http.MethodTrace,
		http.MethodConnect:
		return strings.ToUpper(strings.TrimSpace(method))
	case "":
		return "UNKNOWN"
	default:
		return "OTHER"
	}
}

func normalizeRouteLabel(r *http.Request) string {
	rctx := chi.RouteContext(r.Context())
	if rctx == nil {
		return "unknown"
	}

	pattern := strings.TrimSpace(rctx.RoutePattern())
	if pattern == "" {
		return "unknown"
	}

	for strings.Contains(pattern, "//") {
		pattern = strings.ReplaceAll(pattern, "//", "/")
	}
	if !strings.HasPrefix(pattern, "/") {
		pattern = "/" + pattern
	}
	return pattern
}

func normalizeStatusCode(status int) string {
	if status <= 0 {
		status = http.StatusOK
	}
	return strconv.Itoa(status)
}

func normalizeToken(raw, fallback string) string {
	raw = strings.TrimSpace(strings.ToLower(raw))
	if raw == "" {
		return fallback
	}

	var b strings.Builder
	for _, r := range raw {
		switch {
		case r >= 'a' && r <= 'z':
			b.WriteRune(r)
		case r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == '_' || r == '-':
			b.WriteRune(r)
		case r == ' ':
			b.WriteRune('_')
		}
	}
	if b.Len() == 0 {
		return fallback
	}
	return b.String()
}

func normalizeAuthAbuseSurface(surface string) string {
	switch normalized := normalizeToken(surface, AuthAbuseSurfaceUnknown); normalized {
	case AuthAbuseSurfacePublicLogin,
		AuthAbuseSurfaceAdminPassword,
		AuthAbuseSurfaceAdminTOTP:
		return normalized
	default:
		return AuthAbuseSurfaceUnknown
	}
}

func normalizeAuthAbuseReason(reason string) string {
	switch normalized := normalizeToken(reason, authAbuseReasonOther); normalized {
	case AuthAbuseReasonLimited,
		AuthAbuseReasonRejected,
		AuthAbuseReasonStoreError:
		return normalized
	default:
		return authAbuseReasonOther
	}
}

func normalizeSignalingSurface(surface string) string {
	switch normalized := normalizeToken(surface, signalingSurfaceUnknown); normalized {
	case SignalingSurfaceWebSocketAuth,
		SignalingSurfaceWebSocketUpgrade,
		SignalingSurfaceSessionJoin,
		SignalingSurfaceSessionRelay,
		SignalingSurfaceChatJoin,
		SignalingSurfaceChatMessage:
		return normalized
	default:
		return signalingSurfaceUnknown
	}
}

func normalizeSignalingReason(reason string) string {
	switch normalized := normalizeToken(reason, signalingReasonOther); normalized {
	case SignalingReasonUnauthorized,
		SignalingReasonForbidden,
		SignalingReasonMissingClaims,
		SignalingReasonUpgradeFailed,
		SignalingReasonAccessDenied,
		SignalingReasonAuthorizationFailed,
		SignalingReasonMissingSessionID,
		SignalingReasonInvalidRole,
		SignalingReasonNotInSession,
		SignalingReasonPeerNotConnected,
		SignalingReasonMissingMachineID,
		SignalingReasonMachineNotConnected:
		return normalized
	default:
		return signalingReasonOther
	}
}

func normalizeRelaySurface(surface string) string {
	switch normalized := normalizeToken(surface, relaySurfaceUnknown); normalized {
	case RelaySurfaceListen,
		RelaySurfaceHello,
		RelaySurfaceNoPeer,
		RelaySurfaceCrossNodeWait,
		RelaySurfaceCrossNodeConnect,
		RelaySurfaceCrossNodeHandshake:
		return normalized
	default:
		return relaySurfaceUnknown
	}
}

func normalizeRelayReason(reason string) string {
	switch normalized := normalizeToken(reason, relayReasonOther); normalized {
	case RelayReasonError,
		RelayReasonTimeout,
		RelayReasonMalformed,
		RelayReasonRejected:
		return normalized
	default:
		return relayReasonOther
	}
}
