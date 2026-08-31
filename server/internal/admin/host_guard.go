package admin

import (
	"log/slog"
	"net/http"
	"strings"
)

// NormalizeConfiguredAdminHost canonicalizes the configured admin host for exact
// comparisons. Only bare hosts with an optional numeric port are accepted.
func NormalizeConfiguredAdminHost(raw string) string {
	host := normalizeAdminHost(raw)
	if host == "" {
		return ""
	}
	if strings.ContainsAny(host, `/\\?#@`) {
		return ""
	}
	return host
}

func normalizeAdminRequestHost(raw string) string {
	return normalizeAdminHost(raw)
}

func normalizeAdminHost(raw string) string {
	host := strings.ToLower(strings.TrimSpace(raw))
	if host == "" {
		return ""
	}
	if strings.HasSuffix(host, ".") {
		host = strings.TrimRight(host, ".")
	}
	if name, port, ok := strings.Cut(host, ":"); ok && name != "" && port != "" && !strings.Contains(name, ":") && isNumericPort(port) {
		host = name
	}
	if strings.ContainsAny(host, " \t\r\n") {
		return ""
	}
	return host
}

func isNumericPort(raw string) bool {
	if raw == "" {
		return false
	}
	for _, r := range raw {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

func (h *Handler) requireCanonicalHost(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !h.adminHostConfigured {
			next.ServeHTTP(w, r)
			return
		}

		receivedHost := normalizeAdminRequestHost(r.Host)
		if h.adminHost != "" && receivedHost == h.adminHost {
			next.ServeHTTP(w, r)
			return
		}

		slog.Warn("admin host mismatch", "expected_host", h.adminHost, "received_host", receivedHost, "raw_host", strings.TrimSpace(r.Host), "path", r.URL.Path)
		http.NotFound(w, r)
	})
}
