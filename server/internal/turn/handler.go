package turn

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

const (
	defaultTURNPort         = 3478
	defaultTURNProbeTimeout = 750 * time.Millisecond

	stunBindingRequestType = 0x0001
	stunBindingSuccessType = 0x0101
	stunMagicCookie        = 0x2112A442

	turnReasonBindingSuccess = "binding_success"
	turnReasonProbeFailed    = "probe_failed"
	turnReasonProbeTimeout   = "probe_timeout"
	turnReasonMalformed      = "malformed_response"
	turnReasonInvalidServer  = "invalid_server"
	turnReasonEmptyServer    = "empty_server"
	turnReasonNotConfigured  = "not_configured"
)

// Config contém a configuração para geração de credenciais TURN.
type Config struct {
	// Shared secret com os servidores Coturn (use-auth-secret).
	Secret string

	// Lista de servidores TURN no formato "host:port".
	Servers []string

	// Tempo de validade das credenciais (padrão 24h).
	CredentialTTL time.Duration
}

// TURNCredential representa credenciais temporárias para TURN.
type TURNCredential struct {
	Username string   `json:"username"`
	Password string   `json:"password"`
	TTL      int      `json:"ttl"`
	URIs     []string `json:"uris"`
}

// TURNServer representa um servidor TURN com status explícito de saúde.
type TURNServer struct {
	Host      string               `json:"host"`
	Port      int                  `json:"port"`
	Healthy   bool                 `json:"healthy"`
	Status    observability.Status `json:"status"`
	Checked   bool                 `json:"checked"`
	Reason    string               `json:"reason,omitempty"`
	LastError string               `json:"last_error,omitempty"`
	LatencyMS *int64               `json:"latency_ms,omitempty"`
	CheckedAt *time.Time           `json:"checked_at"`
}

type turnProbeResult struct {
	status    observability.Status
	checked   bool
	reason    string
	lastError string
	latencyMS *int64
	checkedAt time.Time
}

type turnProbeFunc func(ctx context.Context, address string) turnProbeResult

// Handler expõe endpoints de TURN credentials.
type Handler struct {
	config       Config
	probeTimeout time.Duration
	probeFn      turnProbeFunc
}

// NewHandler cria um novo handler de TURN.
func NewHandler(cfg Config) *Handler {
	return newHandlerWithProbe(cfg, defaultTURNProbeTimeout, probeTURNServer)
}

func newHandlerWithProbe(cfg Config, probeTimeout time.Duration, probeFn turnProbeFunc) *Handler {
	if cfg.CredentialTTL == 0 {
		cfg.CredentialTTL = 24 * time.Hour
	}
	if probeTimeout <= 0 {
		probeTimeout = defaultTURNProbeTimeout
	}
	if probeFn == nil {
		probeFn = probeTURNServer
	}
	return &Handler{config: cfg, probeTimeout: probeTimeout, probeFn: probeFn}
}

// Routes registra as rotas de TURN.
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Get("/credentials", h.GetCredentials)
	r.Get("/servers", h.GetServers)
	return r
}

// ─── GET /turn/credentials ──────────────────────────────────────────────────
// Gera credenciais temporárias TURN usando o mecanismo TURN REST API.
// Username = "timestamp:userid", Password = HMAC-SHA1(username, secret) base64.
func (h *Handler) GetCredentials(w http.ResponseWriter, r *http.Request) {
	userID := "anonymous"
	if claims, ok := auth.UserFromContext(r.Context()); ok && claims != nil && claims.UserID != "" {
		userID = claims.UserID
	}

	ttlSec := int(h.config.CredentialTTL.Seconds())
	expiry := time.Now().Unix() + int64(ttlSec)
	username := fmt.Sprintf("%d:%s", expiry, userID)

	// HMAC-SHA1 do username com o shared secret.
	mac := hmac.New(sha1.New, []byte(h.config.Secret))
	mac.Write([]byte(username))
	password := base64.StdEncoding.EncodeToString(mac.Sum(nil))

	// Construir URIs TURN.
	var uris []string
	for _, server := range h.config.Servers {
		if strings.TrimSpace(server) == "" {
			continue
		}
		uris = append(uris, "stun:"+server)
		uris = append(uris, "turn:"+server+"?transport=udp")
		uris = append(uris, "turn:"+server+"?transport=tcp")
	}

	cred := TURNCredential{
		Username: username,
		Password: password,
		TTL:      ttlSec,
		URIs:     uris,
	}

	slog.Info("turn credentials generated", "user_id", userID, "ttl", ttlSec)
	writeJSON(w, http.StatusOK, cred)
}

// ─── GET /turn/servers ──────────────────────────────────────────────────────
// Retorna lista de TURN servers com status de saúde explícito e honesto.
func (h *Handler) GetServers(w http.ResponseWriter, r *http.Request) {
	if len(h.config.Servers) == 0 {
		writeJSON(w, http.StatusOK, []TURNServer{notConfiguredTURNServer()})
		return
	}

	servers := make([]TURNServer, 0, len(h.config.Servers))
	for _, raw := range h.config.Servers {
		servers = append(servers, h.inspectServer(r.Context(), raw))
	}

	writeJSON(w, http.StatusOK, servers)
}

func (h *Handler) inspectServer(parent context.Context, raw string) TURNServer {
	host, port, address, ok, reason, detail := parseTURNServer(raw)
	checkedAt := time.Now().UTC()
	if !ok {
		return TURNServer{
			Host:      host,
			Port:      port,
			Healthy:   false,
			Status:    observability.StatusUnknown,
			Checked:   false,
			Reason:    reason,
			LastError: detail,
			CheckedAt: &checkedAt,
		}
	}

	probeCtx, cancel := context.WithTimeout(parent, h.probeTimeout)
	defer cancel()

	probe := h.probeFn(probeCtx, address)
	if probe.checkedAt.IsZero() {
		probe.checkedAt = checkedAt
	}

	return TURNServer{
		Host:      host,
		Port:      port,
		Healthy:   probe.status == observability.StatusHealthy,
		Status:    probe.status,
		Checked:   probe.checked,
		Reason:    probe.reason,
		LastError: probe.lastError,
		LatencyMS: probe.latencyMS,
		CheckedAt: &probe.checkedAt,
	}
}

func notConfiguredTURNServer() TURNServer {
	checkedAt := time.Now().UTC()
	return TURNServer{
		Healthy:   false,
		Status:    observability.StatusUnknown,
		Checked:   false,
		Reason:    turnReasonNotConfigured,
		LastError: "turn server list is empty",
		CheckedAt: &checkedAt,
	}
}

func parseTURNServer(raw string) (host string, port int, address string, ok bool, reason string, detail string) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return "", 0, "", false, turnReasonEmptyServer, "turn server entry is empty"
	}

	if !strings.Contains(trimmed, ":") {
		return trimmed, defaultTURNPort, net.JoinHostPort(trimmed, strconv.Itoa(defaultTURNPort)), true, "", ""
	}

	host, portText, err := net.SplitHostPort(trimmed)
	if err != nil {
		if strings.Contains(err.Error(), "missing port in address") {
			return trimmed, defaultTURNPort, net.JoinHostPort(trimmed, strconv.Itoa(defaultTURNPort)), true, "", ""
		}
		return trimmed, 0, "", false, turnReasonInvalidServer, err.Error()
	}
	if strings.TrimSpace(host) == "" {
		return trimmed, 0, "", false, turnReasonInvalidServer, "turn server host is empty"
	}

	port, err = strconv.Atoi(portText)
	if err != nil || port <= 0 || port > 65535 {
		return host, 0, "", false, turnReasonInvalidServer, fmt.Sprintf("invalid turn server port %q", portText)
	}

	return host, port, net.JoinHostPort(host, strconv.Itoa(port)), true, "", ""
}

func probeTURNServer(ctx context.Context, address string) turnProbeResult {
	checkedAt := time.Now().UTC()
	conn, err := (&net.Dialer{}).DialContext(ctx, "udp", address)
	if err != nil {
		return classifyProbeError(checkedAt, err)
	}
	defer conn.Close()

	if deadline, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(deadline)
	}

	request, txID, err := newSTUNBindingRequest()
	if err != nil {
		return turnProbeResult{
			status:    observability.StatusDegraded,
			checked:   true,
			reason:    turnReasonProbeFailed,
			lastError: err.Error(),
			checkedAt: checkedAt,
		}
	}

	startedAt := time.Now()
	if _, err := conn.Write(request); err != nil {
		return classifyProbeError(checkedAt, err)
	}

	response := make([]byte, 1024)
	n, err := conn.Read(response)
	if err != nil {
		return classifyProbeError(checkedAt, err)
	}

	if err := validateSTUNBindingResponse(response[:n], txID); err != nil {
		return turnProbeResult{
			status:    observability.StatusDegraded,
			checked:   true,
			reason:    turnReasonMalformed,
			lastError: err.Error(),
			checkedAt: checkedAt,
		}
	}

	latency := time.Since(startedAt).Milliseconds()
	return turnProbeResult{
		status:    observability.StatusHealthy,
		checked:   true,
		reason:    turnReasonBindingSuccess,
		latencyMS: &latency,
		checkedAt: checkedAt,
	}
}

func classifyProbeError(checkedAt time.Time, err error) turnProbeResult {
	reason := turnReasonProbeFailed
	if isTimeoutError(err) {
		reason = turnReasonProbeTimeout
	}
	return turnProbeResult{
		status:    observability.StatusDegraded,
		checked:   true,
		reason:    reason,
		lastError: err.Error(),
		checkedAt: checkedAt,
	}
}

func isTimeoutError(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return true
	}
	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}

func newSTUNBindingRequest() ([]byte, [12]byte, error) {
	var txID [12]byte
	if _, err := rand.Read(txID[:]); err != nil {
		return nil, txID, fmt.Errorf("generate stun transaction id: %w", err)
	}

	request := make([]byte, 20)
	binary.BigEndian.PutUint16(request[0:2], stunBindingRequestType)
	binary.BigEndian.PutUint16(request[2:4], 0)
	binary.BigEndian.PutUint32(request[4:8], stunMagicCookie)
	copy(request[8:20], txID[:])
	return request, txID, nil
}

func validateSTUNBindingResponse(message []byte, txID [12]byte) error {
	if len(message) < 20 {
		return errors.New("stun response too short")
	}
	if binary.BigEndian.Uint16(message[0:2]) != stunBindingSuccessType {
		return fmt.Errorf("unexpected stun message type 0x%04x", binary.BigEndian.Uint16(message[0:2]))
	}
	declaredLength := int(binary.BigEndian.Uint16(message[2:4]))
	if len(message[20:]) < declaredLength {
		return errors.New("stun response truncated")
	}
	if binary.BigEndian.Uint32(message[4:8]) != stunMagicCookie {
		return errors.New("unexpected stun magic cookie")
	}
	if !bytes.Equal(message[8:20], txID[:]) {
		return errors.New("stun transaction id mismatch")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if v != nil {
		json.NewEncoder(w).Encode(v)
	}
}
