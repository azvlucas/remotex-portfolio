package observability

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestHealthHandlerSerializesStableStartupSnapshot(t *testing.T) {
	recorder := NewRecorder()
	recorder.RegisterMonitor("heartbeat", 30*time.Second)
	recorder.RegisterMonitor("ghost_session", time.Minute)
	recorder.RegisterMonitor("auto_block", time.Hour)

	handler := NewHealthHandler("api", recorder, map[string]ComponentCheck{
		"database": func(context.Context) ComponentStatus {
			return ComponentStatus{Status: StatusHealthy, Mode: "primary", Detail: "database ping ok"}
		},
		"redis": func(context.Context) ComponentStatus {
			return ComponentStatus{Status: StatusDisabled, Mode: "disabled", Detail: "redis not configured"}
		},
	}, 250*time.Millisecond)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	resp := httptest.NewRecorder()
	handler.ServeHTTP(resp, req)
	if resp.Code != http.StatusServiceUnavailable {
		t.Fatalf("GET /health status = %d, want %d", resp.Code, http.StatusServiceUnavailable)
	}

	var snapshot HealthSnapshot
	if err := json.NewDecoder(resp.Body).Decode(&snapshot); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}

	if snapshot.Service != "api" {
		t.Fatalf("snapshot.Service = %q, want %q", snapshot.Service, "api")
	}
	if snapshot.Status != StatusStarting {
		t.Fatalf("snapshot.Status = %q, want %q", snapshot.Status, StatusStarting)
	}
	if got := snapshot.Components["database"].Status; got != StatusHealthy {
		t.Fatalf("database status = %q, want %q", got, StatusHealthy)
	}
	if got := snapshot.Components["redis"].Status; got != StatusDisabled {
		t.Fatalf("redis status = %q, want %q", got, StatusDisabled)
	}
	if got := snapshot.Components["redis"].Mode; got != "disabled" {
		t.Fatalf("redis mode = %q, want %q", got, "disabled")
	}

	heartbeat := snapshot.Monitors["heartbeat"]
	if heartbeat.Status != StatusStarting {
		t.Fatalf("heartbeat status = %q, want %q", heartbeat.Status, StatusStarting)
	}
	if heartbeat.LastRunAt != nil || heartbeat.LastSuccessAt != nil || heartbeat.LastFailureAt != nil || heartbeat.LastCount != nil {
		t.Fatalf("heartbeat startup state = %+v, want nil history", heartbeat)
	}
}

func TestHealthHandlerShowsDegradedComponentAndMonitorState(t *testing.T) {
	recorder := NewRecorder()
	recorder.RegisterMonitor("heartbeat", 30*time.Second)
	recorder.RegisterMonitor("ghost_session", time.Minute)
	recorder.RegisterMonitor("auto_block", time.Hour)
	recorder.RecordMonitorResult("heartbeat", 2, nil)
	recorder.RecordMonitorResult("ghost_session", 0, errors.New("query timeout"))
	recorder.RecordMonitorResult("auto_block", 0, nil)

	handler := NewHealthHandler("api", recorder, map[string]ComponentCheck{
		"database": func(context.Context) ComponentStatus {
			return ComponentStatus{Status: StatusHealthy, Mode: "primary", Detail: "database ping ok"}
		},
		"redis": func(context.Context) ComponentStatus {
			return ComponentStatus{Status: StatusDegraded, Mode: "fallback only", Detail: "redis unavailable; using in-memory fallback", LastError: "redis down"}
		},
	}, 250*time.Millisecond)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	resp := httptest.NewRecorder()
	handler.ServeHTTP(resp, req)
	if resp.Code != http.StatusServiceUnavailable {
		t.Fatalf("GET /health status = %d, want %d", resp.Code, http.StatusServiceUnavailable)
	}

	var snapshot HealthSnapshot
	if err := json.NewDecoder(resp.Body).Decode(&snapshot); err != nil {
		t.Fatalf("Decode() error = %v", err)
	}
	if snapshot.Status != StatusDegraded {
		t.Fatalf("snapshot.Status = %q, want %q", snapshot.Status, StatusDegraded)
	}

	redis := snapshot.Components["redis"]
	if redis.Mode != "fallback_only" {
		t.Fatalf("redis mode = %q, want %q", redis.Mode, "fallback_only")
	}
	if redis.LastError != "redis down" {
		t.Fatalf("redis last_error = %q, want %q", redis.LastError, "redis down")
	}

	ghost := snapshot.Monitors["ghost_session"]
	if ghost.Status != StatusDegraded {
		t.Fatalf("ghost status = %q, want %q", ghost.Status, StatusDegraded)
	}
	if ghost.LastFailureAt == nil {
		t.Fatal("ghost LastFailureAt = nil, want timestamp")
	}
	if ghost.LastCount != nil {
		t.Fatalf("ghost LastCount = %v, want nil on failure", *ghost.LastCount)
	}
	if ghost.LastError != "query timeout" {
		t.Fatalf("ghost last_error = %q, want %q", ghost.LastError, "query timeout")
	}
}

func TestBuildHealthSnapshotBoundsSlowChecks(t *testing.T) {
	recorder := NewRecorder()
	recorder.RegisterMonitor("heartbeat", 30*time.Second)

	ctx := context.Background()
	snapshot := BuildHealthSnapshot(ctx, "api", recorder, map[string]ComponentCheck{
		"database": func(ctx context.Context) ComponentStatus {
			<-ctx.Done()
			return ComponentStatus{Status: StatusDegraded, Mode: "primary", Detail: "database ping failed", LastError: ctx.Err().Error()}
		},
	}, 25*time.Millisecond, time.Now())

	if snapshot.Status != StatusDegraded {
		t.Fatalf("snapshot.Status = %q, want %q", snapshot.Status, StatusDegraded)
	}
	if got := snapshot.Components["database"].LastError; got != context.DeadlineExceeded.Error() {
		t.Fatalf("database last_error = %q, want %q", got, context.DeadlineExceeded.Error())
	}
}
