package observability

import (
	"context"
	"encoding/json"
	"net/http"
	"sort"
	"sync"
	"time"
)

// Status is a bounded health state used across components and monitors.
type Status string

const (
	StatusHealthy  Status = "healthy"
	StatusDegraded Status = "degraded"
	StatusDisabled Status = "disabled"
	StatusStarting Status = "starting"
	StatusUnknown  Status = "unknown"
)

// ComponentStatus describes a probed subsystem such as database or Redis.
type ComponentStatus struct {
	Status    Status     `json:"status"`
	Mode      string     `json:"mode,omitempty"`
	Detail    string     `json:"detail,omitempty"`
	LastError string     `json:"last_error,omitempty"`
	Count     *int64     `json:"count,omitempty"`
	CheckedAt *time.Time `json:"checked_at"`
}

// MonitorStatus describes the latest background monitor state.
type MonitorStatus struct {
	Status          Status     `json:"status"`
	IntervalSeconds int64      `json:"interval_seconds"`
	LastRunAt       *time.Time `json:"last_run_at"`
	LastSuccessAt   *time.Time `json:"last_success_at"`
	LastFailureAt   *time.Time `json:"last_failure_at"`
	LastCount       *int64     `json:"last_count"`
	LastError       string     `json:"last_error,omitempty"`
}

// HealthSnapshot is the machine-readable /health payload.
type HealthSnapshot struct {
	Service    string                     `json:"service"`
	Status     Status                     `json:"status"`
	CheckedAt  time.Time                  `json:"checked_at"`
	Components map[string]ComponentStatus `json:"components"`
	Monitors   map[string]MonitorStatus   `json:"monitors"`
}

// ComponentCheck probes one named component with a bounded context.
type ComponentCheck func(context.Context) ComponentStatus

// Recorder stores the last known component and monitor states without global coupling.
type Recorder struct {
	mu         sync.RWMutex
	components map[string]ComponentStatus
	monitors   map[string]MonitorStatus
}

// NewRecorder creates an empty, reusable health recorder.
func NewRecorder() *Recorder {
	return &Recorder{
		components: make(map[string]ComponentStatus),
		monitors:   make(map[string]MonitorStatus),
	}
}

// SetComponent stores a bounded component status snapshot.
func (r *Recorder) SetComponent(name string, status ComponentStatus) {
	if r == nil {
		return
	}
	name = normalizeToken(name, "unknown")
	status.Status = normalizeHealthStatus(status.Status)
	status.Mode = normalizeMode(status.Mode)
	if status.CheckedAt == nil {
		now := time.Now().UTC()
		status.CheckedAt = &now
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	r.components[name] = status
}

// RegisterMonitor seeds a monitor entry so startup state is visible before the first tick.
func (r *Recorder) RegisterMonitor(name string, interval time.Duration) {
	if r == nil {
		return
	}
	name = normalizeToken(name, "unknown")
	entry := MonitorStatus{
		Status:          StatusStarting,
		IntervalSeconds: int64(interval / time.Second),
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	if existing, ok := r.monitors[name]; ok {
		entry.LastRunAt = existing.LastRunAt
		entry.LastSuccessAt = existing.LastSuccessAt
		entry.LastFailureAt = existing.LastFailureAt
		entry.LastCount = cloneInt64(existing.LastCount)
		entry.LastError = existing.LastError
		if existing.Status != "" {
			entry.Status = normalizeHealthStatus(existing.Status)
		}
	}
	r.monitors[name] = entry
}

// RecordMonitorResult updates the last-run, last-success/last-failure, and last-count state.
func (r *Recorder) RecordMonitorResult(name string, count int, err error) {
	if r == nil {
		return
	}
	name = normalizeToken(name, "unknown")
	now := time.Now().UTC()
	count64 := int64(count)

	r.mu.Lock()
	defer r.mu.Unlock()
	entry := r.monitors[name]
	entry.LastRunAt = &now
	if err != nil {
		entry.Status = StatusDegraded
		entry.LastFailureAt = &now
		entry.LastCount = nil
		entry.LastError = err.Error()
	} else {
		entry.Status = StatusHealthy
		entry.LastSuccessAt = &now
		entry.LastCount = &count64
		entry.LastError = ""
	}
	r.monitors[name] = entry
}

// Snapshot returns a copy suitable for JSON serialization or aggregation.
func (r *Recorder) Snapshot() HealthSnapshot {
	if r == nil {
		return HealthSnapshot{
			Components: map[string]ComponentStatus{},
			Monitors:   map[string]MonitorStatus{},
		}
	}

	r.mu.RLock()
	defer r.mu.RUnlock()

	components := make(map[string]ComponentStatus, len(r.components))
	for name, status := range r.components {
		cloned := status
		cloned.CheckedAt = cloneTime(status.CheckedAt)
		cloned.Count = cloneInt64(status.Count)
		components[name] = cloned
	}

	monitors := make(map[string]MonitorStatus, len(r.monitors))
	for name, status := range r.monitors {
		cloned := status
		cloned.LastRunAt = cloneTime(status.LastRunAt)
		cloned.LastSuccessAt = cloneTime(status.LastSuccessAt)
		cloned.LastFailureAt = cloneTime(status.LastFailureAt)
		cloned.LastCount = cloneInt64(status.LastCount)
		monitors[name] = cloned
	}

	return HealthSnapshot{
		Components: components,
		Monitors:   monitors,
	}
}

// BuildHealthSnapshot probes components, captures their latest state, and aggregates an overall status.
func BuildHealthSnapshot(ctx context.Context, service string, recorder *Recorder, checks map[string]ComponentCheck, probeTimeout time.Duration, now time.Time) HealthSnapshot {
	if recorder == nil {
		recorder = NewRecorder()
	}
	if probeTimeout <= 0 {
		probeTimeout = time.Second
	}

	names := make([]string, 0, len(checks))
	for name := range checks {
		names = append(names, name)
	}
	sort.Strings(names)

	for _, name := range names {
		check := checks[name]
		probeCtx, cancel := context.WithTimeout(ctx, probeTimeout)
		status := check(probeCtx)
		cancel()
		recorder.SetComponent(name, status)
	}

	snapshot := recorder.Snapshot()
	snapshot.Service = normalizeToken(service, "unknown")
	snapshot.CheckedAt = now.UTC()
	snapshot.Status = aggregateStatus(snapshot.Components, snapshot.Monitors)
	return snapshot
}

// NewHealthHandler returns a JSON /health handler backed by the recorder and bounded probes.
func NewHealthHandler(service string, recorder *Recorder, checks map[string]ComponentCheck, probeTimeout time.Duration) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		snapshot := BuildHealthSnapshot(r.Context(), service, recorder, checks, probeTimeout, time.Now())
		statusCode := httpStatusFor(snapshot.Status)

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(statusCode)
		if err := json.NewEncoder(w).Encode(snapshot); err != nil {
			http.Error(w, `{"status":"degraded","reason":"encode_error"}`+"\n", http.StatusInternalServerError)
		}
	})
}

func aggregateStatus(components map[string]ComponentStatus, monitors map[string]MonitorStatus) Status {
	overall := StatusHealthy
	for _, component := range components {
		switch normalizeHealthStatus(component.Status) {
		case StatusDegraded, StatusUnknown:
			return StatusDegraded
		case StatusStarting:
			overall = StatusStarting
		}
	}
	for _, monitor := range monitors {
		switch normalizeHealthStatus(monitor.Status) {
		case StatusDegraded, StatusUnknown:
			return StatusDegraded
		case StatusStarting:
			overall = StatusStarting
		}
	}
	return overall
}

func httpStatusFor(status Status) int {
	switch normalizeHealthStatus(status) {
	case StatusHealthy:
		return http.StatusOK
	default:
		return http.StatusServiceUnavailable
	}
}

func normalizeHealthStatus(status Status) Status {
	switch status {
	case StatusHealthy, StatusDegraded, StatusDisabled, StatusStarting:
		return status
	default:
		return StatusUnknown
	}
}

func normalizeMode(mode string) string {
	if mode == "" {
		return ""
	}
	return normalizeToken(mode, "unknown")
}

func cloneTime(value *time.Time) *time.Time {
	if value == nil {
		return nil
	}
	cloned := value.UTC()
	return &cloned
}

func cloneInt64(value *int64) *int64 {
	if value == nil {
		return nil
	}
	cloned := *value
	return &cloned
}
