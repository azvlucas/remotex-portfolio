package admin

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"html/template"
	"log/slog"
	"mime"
	"net/http"
	"net/url"
	"strings"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

type csrfContextKey struct{}

func generateCSRFToken() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func csrfFromCtx(r *http.Request) string {
	if v, ok := r.Context().Value(csrfContextKey{}).(string); ok {
		return v
	}
	return ""
}

func adminForwardedProto(r *http.Request) string {
	if r == nil {
		return ""
	}
	return strings.ToLower(strings.TrimSpace(r.Header.Get("X-Forwarded-Proto")))
}

func isAdminSecureRequest(r *http.Request) bool {
	if r != nil && r.TLS != nil {
		return true
	}
	return adminForwardedProto(r) == "https"
}

func adminInsecureTransportReason(r *http.Request) string {
	if r == nil {
		return "missing_request"
	}
	if r.TLS != nil {
		return ""
	}

	switch adminForwardedProto(r) {
	case "":
		return "missing_forwarded_proto"
	case "http":
		return "forwarded_proto_http"
	default:
		return "forwarded_proto_invalid"
	}
}

func (h *Handler) requireSecureTransport(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if isAdminSecureRequest(r) {
			next.ServeHTTP(w, r)
			return
		}

		slog.Warn("admin insecure transport rejected",
			"reason", adminInsecureTransportReason(r),
			"forwarded_proto", adminForwardedProto(r),
			"host", normalizeAdminRequestHost(r.Host),
			"path", r.URL.Path,
		)
		http.NotFound(w, r)
	})
}

func (h *Handler) requireAdmin(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		claims, err := h.currentAdmin(r)
		if err != nil {
			if wantsJSON(r) {
				writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
				return
			}
			http.Redirect(w, r, "/admin/login?error="+url.QueryEscape("faÃ§a login para continuar"), http.StatusSeeOther)
			return
		}

		ctx := context.WithValue(r.Context(), adminClaimsContextKey{}, claims)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func (h *Handler) currentAdmin(r *http.Request) (*auth.Claims, error) {
	cookie, err := r.Cookie(adminCookieName)
	if err != nil || strings.TrimSpace(cookie.Value) == "" {
		return nil, auth.ErrTokenInvalid
	}

	claims, err := h.authSvc.ValidateAdminSessionToken(cookie.Value)
	if err != nil {
		reason := "invalid"
		if errors.Is(err, auth.ErrTokenExpired) {
			reason = "expired"
		}
		slog.Warn("admin session token rejected", "reason", reason, "path", r.URL.Path)
		return nil, err
	}

	user, err := h.users.GetUserByID(r.Context(), claims.UserID)
	if err != nil {
		if errors.Is(err, auth.ErrUserNotFound) {
			slog.Warn("admin session user missing", "user_id", claims.UserID)
			return nil, auth.ErrTokenInvalid
		}
		return nil, err
	}
	if !user.IsActive {
		slog.Warn("admin session user inactive", "user_id", user.ID)
		return nil, auth.ErrTokenInvalid
	}
	if user.Role != auth.RoleSuperAdmin {
		slog.Warn("admin session role rejected", "user_id", user.ID, "role", user.Role)
		return nil, auth.ErrTokenInvalid
	}

	claims.Email = user.Email
	claims.Name = user.Name
	claims.Role = user.Role
	claims.CompanyID = ""
	return claims, nil
}

func (h *Handler) currentAdminChallenge(r *http.Request) (*auth.Claims, error) {
	cookie, err := r.Cookie(adminChallengeCookieName)
	if err != nil || strings.TrimSpace(cookie.Value) == "" {
		return nil, auth.ErrTokenInvalid
	}

	claims, err := h.authSvc.ValidateAdminChallengeToken(cookie.Value)
	if err != nil {
		reason := "invalid"
		if errors.Is(err, auth.ErrTokenExpired) {
			reason = "expired"
		}
		slog.Warn("admin challenge token rejected", "reason", reason, "path", r.URL.Path)
		return nil, err
	}

	return claims, nil
}

func (h *Handler) respondLoginError(w http.ResponseWriter, r *http.Request, status int, message string) {
	if wantsJSON(r) {
		writeJSON(w, status, map[string]string{"error": message})
		return
	}
	renderTemplate(w, status, loginTemplate, loginPageData{Error: message, CSRFToken: csrfFromCtx(r)})
}

func (h *Handler) redirectOrJSONError(w http.ResponseWriter, r *http.Request, target string, status int, message string) {
	if wantsJSON(r) {
		writeJSON(w, status, map[string]string{"error": message})
		return
	}
	separator := "?"
	if strings.Contains(target, "?") {
		separator = "&"
	}
	http.Redirect(w, r, target+separator+"error="+url.QueryEscape(message), http.StatusSeeOther)
}

func readCredentials(r *http.Request) (string, string) {
	return readParam(r, "email"), readParam(r, "password")
}

func readParam(r *http.Request, name string) string {
	_ = r.ParseForm()
	return r.FormValue(name)
}

func requestUsesJSON(r *http.Request) bool {
	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil {
		return false
	}
	return mediaType == "application/json"
}

func wantsJSON(r *http.Request) bool {
	accept := r.Header.Get("Accept")
	return strings.Contains(accept, "application/json") || requestUsesJSON(r)
}

func decodeJSONBody(r *http.Request, dst interface{}) error {
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return err
	}
	if dec.More() {
		return errors.New("multiple json values")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func renderTemplate(w http.ResponseWriter, status int, source string, data interface{}) {
	tmpl := template.Must(template.New("page").Parse(source))
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(status)
	_ = tmpl.Execute(w, data)
}

func (h *Handler) csrfProtect(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet || r.Method == http.MethodHead || r.Method == http.MethodOptions {
			// Generate or reuse CSRF token
			token := ""
			if c, err := r.Cookie(csrfCookieName); err == nil && c.Value != "" {
				token = c.Value
			} else {
				var err error
				token, err = generateCSRFToken()
				if err != nil {
					slog.Error("csrf token generation failed", "err", err)
					http.Error(w, "internal error", http.StatusInternalServerError)
					return
				}
			}
			http.SetCookie(w, &http.Cookie{
				Name:     csrfCookieName,
				Value:    token,
				Path:     "/admin",
				HttpOnly: false,
				SameSite: http.SameSiteStrictMode,
				Secure:   isAdminSecureRequest(r),
			})
			ctx := context.WithValue(r.Context(), csrfContextKey{}, token)
			next.ServeHTTP(w, r.WithContext(ctx))
			return
		}

		// POST/PUT/DELETE â€” validate
		cookieToken := ""
		if c, err := r.Cookie(csrfCookieName); err == nil {
			cookieToken = c.Value
		}
		formToken, err := readCSRFToken(w, r)
		if err != nil {
			if wantsJSON(r) {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": "falha ao ler formulÃ¡rio"})
				return
			}
			http.Error(w, "falha ao ler formulÃ¡rio", http.StatusBadRequest)
			return
		}
		if cookieToken == "" || formToken == "" || !hmac.Equal([]byte(cookieToken), []byte(formToken)) {
			if wantsJSON(r) {
				writeJSON(w, http.StatusForbidden, map[string]string{"error": "CSRF token invÃ¡lido"})
				return
			}
			http.Error(w, "CSRF token invÃ¡lido", http.StatusForbidden)
			return
		}

		ctx := context.WithValue(r.Context(), csrfContextKey{}, cookieToken)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func readCSRFToken(w http.ResponseWriter, r *http.Request) (string, error) {
	if headerToken := strings.TrimSpace(r.Header.Get("X-CSRF-Token")); headerToken != "" {
		return headerToken, nil
	}

	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil {
		mediaType = ""
	}
	if mediaType == "multipart/form-data" {
		r.Body = http.MaxBytesReader(w, r.Body, releaseUploadMaxBytes)
		if err := r.ParseMultipartForm(releaseUploadMaxBytes); err != nil {
			return "", err
		}
		return r.FormValue("csrf_token"), nil
	}
	if requestUsesJSON(r) {
		return "", nil
	}
	if err := r.ParseForm(); err != nil {
		return "", err
	}
	return r.FormValue("csrf_token"), nil
}

type adminClaimsContextKey struct{}
