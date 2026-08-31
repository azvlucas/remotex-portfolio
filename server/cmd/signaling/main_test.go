package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	dto "github.com/prometheus/client_model/go"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	"github.com/azvlucas/remotex-portfolio/server/internal/signaling"
)

func newMainTestMetrics(t *testing.T) *observability.Metrics {
	t.Helper()

	metrics, err := observability.NewMetrics("signaling-main-test")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}
	return metrics
}

func mainMetricValue(t *testing.T, metrics *observability.Metrics, familyName, surface, reason string) float64 {
	t.Helper()

	families, err := metrics.Registry.Gather()
	if err != nil {
		t.Fatalf("Gather() error = %v", err)
	}
	for _, family := range families {
		if family.GetName() != familyName {
			continue
		}
		for _, metric := range family.GetMetric() {
			if mainLabelValue(metric.GetLabel(), "surface") == surface && mainLabelValue(metric.GetLabel(), "reason") == reason {
				return metric.GetCounter().GetValue()
			}
		}
		return 0
	}
	return 0
}

func mainLabelValue(labels []*dto.LabelPair, name string) string {
	for _, label := range labels {
		if label.GetName() == name {
			return label.GetValue()
		}
	}
	return ""
}

func testAdminToken(t *testing.T, svc *auth.Service) string {
	t.Helper()

	token, err := svc.GenerateAdminSessionToken(auth.TokenSubject{
		UserID: "admin-1",
		Email:  "admin@example.com",
		Role:   auth.RoleSuperAdmin,
	})
	if err != nil {
		t.Fatalf("GenerateAdminSessionToken() error = %v", err)
	}
	return token
}

func TestWebsocketAuthMetricsMiddlewareRecordsUnauthorizedAndForbidden(t *testing.T) {
	metrics := newMainTestMetrics(t)
	authSvc := auth.NewService(auth.DefaultConfig("test-secret"))

	r := chi.NewRouter()
	r.Use(websocketAuthMetricsMiddleware)
	r.Use(auth.JWTAuthMiddleware(authSvc, nil))
	r.Use(auth.RequireCompanyMiddleware())
	r.Get("/ws", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusSwitchingProtocols)
	})

	unauthorizedReq := httptest.NewRequest(http.MethodGet, "/ws", nil)
	unauthorizedResp := httptest.NewRecorder()
	r.ServeHTTP(unauthorizedResp, unauthorizedReq)
	if unauthorizedResp.Code != http.StatusUnauthorized {
		t.Fatalf("unauthorized status = %d, want %d", unauthorizedResp.Code, http.StatusUnauthorized)
	}
	if got := mainMetricValue(t, metrics, "remotex_signaling_events_total", observability.SignalingSurfaceWebSocketAuth, observability.SignalingReasonUnauthorized); got != 1 {
		t.Fatalf("unauthorized websocket auth counter = %v, want 1", got)
	}

	forbiddenReq := httptest.NewRequest(http.MethodGet, "/ws", nil)
	forbiddenReq.Header.Set("Authorization", "Bearer "+testAdminToken(t, authSvc))
	forbiddenResp := httptest.NewRecorder()
	r.ServeHTTP(forbiddenResp, forbiddenReq)
	if forbiddenResp.Code != http.StatusForbidden {
		t.Fatalf("forbidden status = %d, want %d", forbiddenResp.Code, http.StatusForbidden)
	}
	if got := mainMetricValue(t, metrics, "remotex_signaling_events_total", observability.SignalingSurfaceWebSocketAuth, observability.SignalingReasonForbidden); got != 1 {
		t.Fatalf("forbidden websocket auth counter = %v, want 1", got)
	}
}

func TestSignalingHealthHandlerReportsStructuredRedisAndWebsocketState(t *testing.T) {
	hub := signaling.NewHub(nil, nil, "test-node")

	handler := observability.NewHealthHandler("signaling", observability.NewRecorder(), map[string]observability.ComponentCheck{
		"redis":     signalingRedisComponentCheck("", nil, nil),
		"websocket": websocketComponentCheck(hub),
	}, 100*time.Millisecond)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	resp := httptest.NewRecorder()
	handler.ServeHTTP(resp, req)
	if resp.Code != http.StatusOK {
		t.Fatalf("GET /health status = %d, want %d", resp.Code, http.StatusOK)
	}

	var snapshot observability.HealthSnapshot
	if err := json.NewDecoder(resp.Body).Decode(&snapshot); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if snapshot.Service != "signaling" {
		t.Fatalf("snapshot.Service = %q, want %q", snapshot.Service, "signaling")
	}
	if snapshot.Status != observability.StatusHealthy {
		t.Fatalf("snapshot.Status = %q, want %q", snapshot.Status, observability.StatusHealthy)
	}
	if got := snapshot.Components["redis"].Status; got != observability.StatusDisabled {
		t.Fatalf("redis status = %q, want %q", got, observability.StatusDisabled)
	}
	websocket := snapshot.Components["websocket"]
	if websocket.Count == nil || *websocket.Count != 0 {
		t.Fatalf("websocket count = %v, want 0", websocket.Count)
	}
	if websocket.Mode != "authenticated_clients" {
		t.Fatalf("websocket mode = %q, want %q", websocket.Mode, "authenticated_clients")
	}
}

func TestSignalingRedisComponentCheckReturnsDegradedConfiguredSingleNodeState(t *testing.T) {
	check := signalingRedisComponentCheck("127.0.0.1:6379", nil, context.DeadlineExceeded)
	status := check(context.Background())

	if status.Status != observability.StatusDegraded {
		t.Fatalf("status = %q, want %q", status.Status, observability.StatusDegraded)
	}
	if status.Mode != "single_node" {
		t.Fatalf("mode = %q, want %q", status.Mode, "single_node")
	}
	if status.LastError != context.DeadlineExceeded.Error() {
		t.Fatalf("last_error = %q, want %q", status.LastError, context.DeadlineExceeded.Error())
	}
}
