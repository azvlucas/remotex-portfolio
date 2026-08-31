package signaling

import (
	"log/slog"
	"sync"
)

// Session representa um pareamento entre um Host e um Viewer.
type Session struct {
	ID     string
	Host   *Client
	Viewer *Client
	mu     sync.RWMutex
}

// SessionStore gerencia todas as sessões ativas.
type SessionStore struct {
	sessions map[string]*Session
	mu       sync.RWMutex
}

// NewSessionStore cria um novo store de sessões.
func NewSessionStore() *SessionStore {
	return &SessionStore{
		sessions: make(map[string]*Session),
	}
}

// Join adiciona um client a uma sessão.
// Retorna a sessão e o peer (se já estiver pareado).
func (s *SessionStore) Join(sessionID, role string, client *Client) (*Session, *Client) {
	s.mu.Lock()
	defer s.mu.Unlock()

	sess, exists := s.sessions[sessionID]
	if !exists {
		sess = &Session{ID: sessionID}
		s.sessions[sessionID] = sess
	}

	sess.mu.Lock()
	defer sess.mu.Unlock()

	var peer *Client

	switch role {
	case RoleHost:
		if sess.Host != nil && sess.Host != client {
			slog.Warn("replacing existing host in session", "session", sessionID)
		}
		sess.Host = client
		peer = sess.Viewer
	case RoleViewer:
		if sess.Viewer != nil && sess.Viewer != client {
			slog.Warn("replacing existing viewer in session", "session", sessionID)
		}
		sess.Viewer = client
		peer = sess.Host
	}

	client.SetSession(sessionID, role)

	slog.Info("client joined session",
		"session", sessionID,
		"role", role,
		"client", client.id,
		"paired", peer != nil,
	)

	return sess, peer
}

// Leave remove um client de sua sessão.
// Retorna o peer que ficou (se houver), e se a sessão ficou vazia.
func (s *SessionStore) Leave(client *Client) (*Client, bool) {
	sessionID, role := client.GetSession()
	if sessionID == "" {
		return nil, false
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	sess, exists := s.sessions[sessionID]
	if !exists {
		return nil, false
	}

	sess.mu.Lock()
	defer sess.mu.Unlock()

	var peer *Client

	switch role {
	case RoleHost:
		if sess.Host == client {
			sess.Host = nil
		}
		peer = sess.Viewer
	case RoleViewer:
		if sess.Viewer == client {
			sess.Viewer = nil
		}
		peer = sess.Host
	}

	// Se a sessão ficou vazia, remove do store.
	empty := sess.Host == nil && sess.Viewer == nil
	if empty {
		delete(s.sessions, sessionID)
	}

	client.SetSession("", "")

	slog.Info("client left session",
		"session", sessionID,
		"role", role,
		"client", client.id,
		"session_empty", empty,
	)

	return peer, empty
}

// GetPeer retorna o peer do client na mesma sessão.
func (s *SessionStore) GetPeer(client *Client) *Client {
	sessionID, role := client.GetSession()
	if sessionID == "" {
		return nil
	}

	s.mu.RLock()
	defer s.mu.RUnlock()

	sess, exists := s.sessions[sessionID]
	if !exists {
		return nil
	}

	sess.mu.RLock()
	defer sess.mu.RUnlock()

	switch role {
	case RoleHost:
		return sess.Viewer
	case RoleViewer:
		return sess.Host
	}

	return nil
}

// Count retorna o número de sessões ativas.
func (s *SessionStore) Count() int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.sessions)
}
