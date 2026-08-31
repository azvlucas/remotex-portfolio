package observability

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"
)

func TestMetricsHandlerExposesRuntimeAndHTTPMetrics(t *testing.T) {
	metrics, err := NewMetrics("api")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}

	r := chi.NewRouter()
	r.Use(metrics.Middleware)
	r.Get("/hello", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte("hello"))
	})
	r.Handle("/metrics", metrics.Handler())

	req := httptest.NewRequest(http.MethodGet, "/hello", nil)
	resp := httptest.NewRecorder()
	r.ServeHTTP(resp, req)
	if resp.Code != http.StatusCreated {
		t.Fatalf("GET /hello status = %d, want %d", resp.Code, http.StatusCreated)
	}

	metricsReq := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	metricsResp := httptest.NewRecorder()
	r.ServeHTTP(metricsResp, metricsReq)
	if metricsResp.Code != http.StatusOK {
		t.Fatalf("GET /metrics status = %d, want %d", metricsResp.Code, http.StatusOK)
	}

	body, err := io.ReadAll(metricsResp.Body)
	if err != nil {
		t.Fatalf("ReadAll(metrics) error = %v", err)
	}
	text := string(body)

	for _, want := range []string{
		"go_goroutines",
		"go_build_info",
		"process_cpu_seconds_total",
		`remotex_service_info{service="api"} 1`,
		`remotex_http_requests_total{method="GET",route="/hello",status="201"} 1`,
		`remotex_http_request_duration_seconds_count{method="GET",route="/hello",status="201"} 1`,
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics output missing %q\n%s", want, text)
		}
	}
}

func TestMetricsHandlerExposesBoundedEventFamiliesBeforeTraffic(t *testing.T) {
	metrics, err := NewMetrics("api")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}

	r := chi.NewRouter()
	r.Handle("/metrics", metrics.Handler())

	metricsReq := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	metricsResp := httptest.NewRecorder()
	r.ServeHTTP(metricsResp, metricsReq)
	if metricsResp.Code != http.StatusOK {
		t.Fatalf("GET /metrics status = %d, want %d", metricsResp.Code, http.StatusOK)
	}

	text := metricsResp.Body.String()
	for _, want := range []string{
		`remotex_auth_abuse_events_total{reason="limited",surface="public_login"} 0`,
		`remotex_signaling_events_total{reason="unauthorized",surface="websocket_auth"} 0`,
		`remotex_relay_degradation_events_total{reason="timeout",surface="cross_node_wait"} 0`,
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics output missing %q\n%s", want, text)
		}
	}
}

func TestMetricsMiddlewareNormalizesLabels(t *testing.T) {
	metrics, err := NewMetrics("api")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}

	r := chi.NewRouter()
	r.Use(metrics.Middleware)
	r.Get("/machines/{id}", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	r.Handle("/metrics", metrics.Handler())

	matchedReq := httptest.NewRequest(http.MethodGet, "/machines/abc123", nil)
	matchedResp := httptest.NewRecorder()
	r.ServeHTTP(matchedResp, matchedReq)
	if matchedResp.Code != http.StatusNoContent {
		t.Fatalf("GET /machines/{id} status = %d, want %d", matchedResp.Code, http.StatusNoContent)
	}

	missingReq := httptest.NewRequest(http.MethodGet, "/missing", nil)
	missingResp := httptest.NewRecorder()
	r.ServeHTTP(missingResp, missingReq)
	if missingResp.Code != http.StatusNotFound {
		t.Fatalf("GET /missing status = %d, want %d", missingResp.Code, http.StatusNotFound)
	}

	metricsReq := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	metricsResp := httptest.NewRecorder()
	r.ServeHTTP(metricsResp, metricsReq)
	text := metricsResp.Body.String()

	for _, want := range []string{
		`remotex_http_requests_total{method="GET",route="/machines/{id}",status="204"} 1`,
		`remotex_http_requests_total{method="GET",route="unknown",status="404"} 1`,
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics output missing %q\n%s", want, text)
		}
	}
}

func TestNormalizeMethodMapsUnknownVerbsToOther(t *testing.T) {
	if got := normalizeMethod("BREW"); got != "OTHER" {
		t.Fatalf("normalizeMethod(BREW) = %q, want %q", got, "OTHER")
	}
}

func TestMetricsRegisterReturnsDuplicateCollectorError(t *testing.T) {
	metrics, err := NewMetrics("api")
	if err != nil {
		t.Fatalf("NewMetrics() error = %v", err)
	}

	collector := prometheus.NewGauge(prometheus.GaugeOpts{
		Name: "custom_duplicate_metric",
		Help: "test collector",
	})
	if err := metrics.Register(collector); err != nil {
		t.Fatalf("first Register() error = %v", err)
	}
	if err := metrics.Register(collector); err == nil {
		t.Fatal("second Register() error = nil, want duplicate registration error")
	}
}
