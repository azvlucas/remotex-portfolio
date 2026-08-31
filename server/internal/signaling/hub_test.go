package signaling

import (
	"context"
	"encoding/json"
	"errors"
	"testing"

	dto "github.com/prometheus/client_model/go"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
)

type fakeAccessAuthorizer struct {
	sessionJoinFn func(ctx context.Context, claims *auth.Claims, sessionID, role string) error
	chatFn        func(ctx context.Context, claims *auth.Claims, machineID string) error
}

func (f fakeAccessAuthorizer) AuthorizeSessionJoin(ctx context.Context, claims *auth.Claims, sessionID, role string) error {
	if f.sessionJoinFn != nil {
		return f.sessionJoinFn(ctx, claims, sessionID, role)
	}
	return nil
}

func (f fakeAccessAuthorizer) AuthorizeChat(ctx context.Context, claims *auth.Claims, machineID string) error {
	if f.chatFn != nil {
		return f.chatFn(ctx, claims, machineID)
	}
	return nil
}

func newSignalingTestMetrics(t *testing.T) *observability.Metrics {
	t.Helper()

	metrics, err := observability.NewMetrics("signaling-test")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}
	return metrics
}

func signalingMetricValue(t *testing.T, metrics *observability.Metrics, surface, reason string) float64 {
	t.Helper()

	families, err := metrics.Registry.Gather()
	if err != nil {
		t.Fatalf("Gather() error = %v", err)
	}
	for _, family := range families {
		if family.GetName() != "remotex_signaling_events_total" {
			continue
		}
		for _, metric := range family.GetMetric() {
			if labelValue(metric.GetLabel(), "surface") == surface && labelValue(metric.GetLabel(), "reason") == reason {
				return metric.GetCounter().GetValue()
			}
		}
		return 0
	}
	return 0
}

func signalingActiveConnections(t *testing.T, metrics *observability.Metrics) float64 {
	t.Helper()

	families, err := metrics.Registry.Gather()
	if err != nil {
		t.Fatalf("Gather() error = %v", err)
	}
	for _, family := range families {
		if family.GetName() != "remotex_signaling_active_connections" {
			continue
		}
		for _, metric := range family.GetMetric() {
			return metric.GetGauge().GetValue()
		}
	}
	return 0
}

func labelValue(labels []*dto.LabelPair, name string) string {
	for _, label := range labels {
		if label.GetName() == name {
			return label.GetValue()
		}
	}
	return ""
}

func TestSessionJoinRejectsViewerForUnauthorizedMachine(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, fakeAccessAuthorizer{
		sessionJoinFn: func(_ context.Context, claims *auth.Claims, sessionID, role string) error {
			if claims.UserID == "viewer-1" && sessionID == "machine-2" && role == RoleViewer {
				return ErrAccessDenied
			}
			return nil
		},
	}, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-1",
	})

	hub.dispatch(viewer, Message{
		Type:      MsgSessionJoin,
		SessionID: "machine-2",
		Role:      RoleViewer,
	})

	msg := mustReadClientMessage(t, viewer)
	if msg.Type != MsgError || msg.Error != "session access denied" {
		t.Fatalf("unexpected message: %+v", msg)
	}
	if sid, _ := viewer.GetSession(); sid != "" {
		t.Fatalf("viewer session = %q, want empty", sid)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceSessionJoin, observability.SignalingReasonAccessDenied); got != 1 {
		t.Fatalf("access_denied counter = %v, want 1", got)
	}
}

func TestSessionJoinRejectsHostForDifferentMachine(t *testing.T) {
	hub := NewHub(nil, fakeAccessAuthorizer{
		sessionJoinFn: func(_ context.Context, claims *auth.Claims, sessionID, role string) error {
			if claims.IsMachine() && claims.MachineID != sessionID {
				return ErrAccessDenied
			}
			return nil
		},
	}, "test-node")
	host := NewClient(hub, nil, &auth.Claims{
		UserID:      "machine-1",
		MachineID:   "machine-1",
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
	})

	hub.dispatch(host, Message{
		Type:      MsgSessionJoin,
		SessionID: "machine-2",
		Role:      RoleHost,
	})

	msg := mustReadClientMessage(t, host)
	if msg.Type != MsgError || msg.Error != "session access denied" {
		t.Fatalf("unexpected message: %+v", msg)
	}
}

func TestSessionJoinRejectsMissingSessionIDAndInvalidRole(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, nil, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{UserID: "viewer-1", CompanyID: "company-1"})

	hub.dispatch(viewer, Message{Type: MsgSessionJoin, Role: RoleViewer})
	msg := mustReadClientMessage(t, viewer)
	if msg.Error != "session_id is required" {
		t.Fatalf("missing session_id message = %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceSessionJoin, observability.SignalingReasonMissingSessionID); got != 1 {
		t.Fatalf("missing_session_id counter = %v, want 1", got)
	}

	hub.dispatch(viewer, Message{Type: MsgSessionJoin, SessionID: "machine-1", Role: "invalid"})
	msg = mustReadClientMessage(t, viewer)
	if msg.Error != "role must be 'host' or 'viewer'" {
		t.Fatalf("invalid role message = %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceSessionJoin, observability.SignalingReasonInvalidRole); got != 1 {
		t.Fatalf("invalid_role counter = %v, want 1", got)
	}
}

func TestHandleRelayWithoutSessionRecordsMetric(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, nil, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{UserID: "viewer-1", CompanyID: "company-1"})

	hub.dispatch(viewer, Message{Type: MsgOffer})

	msg := mustReadClientMessage(t, viewer)
	if msg.Error != "not in a session, send session_join first" {
		t.Fatalf("unexpected message: %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceSessionRelay, observability.SignalingReasonNotInSession); got != 1 {
		t.Fatalf("not_in_session counter = %v, want 1", got)
	}
}

func TestNewClientUsesJWTIdentityInsteadOfCallerProvidedClientID(t *testing.T) {
	viewer := NewClient(nil, nil, &auth.Claims{
		UserID:    "viewer-123",
		CompanyID: "company-1",
	})
	if viewer.ID() != "viewer-123" {
		t.Fatalf("viewer id = %q, want %q", viewer.ID(), "viewer-123")
	}

	host := NewClient(nil, nil, &auth.Claims{
		UserID:      "ignored-user-id",
		MachineID:   "machine-9",
		CompanyID:   "company-1",
		SubjectType: auth.SubjectTypeMachine,
	})
	if host.ID() != "machine-9" {
		t.Fatalf("host id = %q, want %q", host.ID(), "machine-9")
	}
}

func TestChatJoinRejectsUnauthorizedMachine(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, fakeAccessAuthorizer{
		chatFn: func(_ context.Context, claims *auth.Claims, machineID string) error {
			if claims.CompanyID != "company-2" || machineID != "machine-2" {
				return ErrAccessDenied
			}
			return nil
		},
	}, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-1",
	})

	hub.dispatch(viewer, Message{
		Type:      MsgChatJoin,
		MachineID: "machine-2",
	})

	msg := mustReadClientMessage(t, viewer)
	if msg.Type != MsgError || msg.Error != "chat access denied" {
		t.Fatalf("unexpected message: %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceChatJoin, observability.SignalingReasonAccessDenied); got != 1 {
		t.Fatalf("chat access_denied counter = %v, want 1", got)
	}
}

func TestChatRejectsMissingMachineIDAndDisconnectedHost(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, nil, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{UserID: "viewer-1", CompanyID: "company-1"})

	hub.dispatch(viewer, Message{Type: MsgChat})
	msg := mustReadClientMessage(t, viewer)
	if msg.Error != "machine_id is required for chat" {
		t.Fatalf("missing machine_id message = %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceChatMessage, observability.SignalingReasonMissingMachineID); got != 1 {
		t.Fatalf("missing_machine_id counter = %v, want 1", got)
	}

	hub.dispatch(viewer, Message{Type: MsgChat, MachineID: "machine-2"})
	msg = mustReadClientMessage(t, viewer)
	if msg.Error != "machine not connected" {
		t.Fatalf("machine not connected message = %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceChatMessage, observability.SignalingReasonMachineNotConnected); got != 1 {
		t.Fatalf("machine_not_connected counter = %v, want 1", got)
	}
}

func TestAuthorizeSessionJoinReturnsInternalFailure(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, fakeAccessAuthorizer{
		sessionJoinFn: func(context.Context, *auth.Claims, string, string) error {
			return errors.New("db unavailable")
		},
	}, "test-node")
	viewer := NewClient(hub, nil, &auth.Claims{
		UserID:    "viewer-1",
		CompanyID: "company-1",
	})

	hub.dispatch(viewer, Message{
		Type:      MsgSessionJoin,
		SessionID: "machine-1",
		Role:      RoleViewer,
	})

	msg := mustReadClientMessage(t, viewer)
	if msg.Type != MsgError || msg.Error != "authorization failed" {
		t.Fatalf("unexpected message: %+v", msg)
	}
	if got := signalingMetricValue(t, metrics, observability.SignalingSurfaceSessionJoin, observability.SignalingReasonAuthorizationFailed); got != 1 {
		t.Fatalf("authorization_failed counter = %v, want 1", got)
	}
}

func TestRegisterAndUnregisterUpdateActiveConnectionGauge(t *testing.T) {
	metrics := newSignalingTestMetrics(t)
	hub := NewHub(nil, nil, "test-node")
	client := NewClient(hub, nil, &auth.Claims{UserID: "viewer-1", CompanyID: "company-1"})

	hub.registerClient(client)
	if got := signalingActiveConnections(t, metrics); got != 1 {
		t.Fatalf("active connections after register = %v, want 1", got)
	}

	hub.unregisterClient(client)
	if got := signalingActiveConnections(t, metrics); got != 0 {
		t.Fatalf("active connections after unregister = %v, want 0", got)
	}
}

func mustReadClientMessage(t *testing.T, client *Client) Message {
	t.Helper()

	select {
	case data := <-client.send:
		var msg Message
		if err := json.Unmarshal(data, &msg); err != nil {
			t.Fatalf("json.Unmarshal() error = %v", err)
		}
		return msg
	default:
		t.Fatal("expected message in client send buffer")
		return Message{}
	}
}
