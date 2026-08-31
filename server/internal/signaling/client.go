package signaling

import (
	"encoding/json"
	"log/slog"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

const (
	writeWait      = 10 * time.Second
	pongWait       = 60 * time.Second
	pingInterval   = (pongWait * 9) / 10
	maxMessageSize = 64 * 1024
)

// Client represents a WebSocket connection authenticated by JWT.
type Client struct {
	hub       *Hub
	conn      *websocket.Conn
	send      chan []byte
	id        string
	claims    *auth.Claims
	role      string
	sessionID string
	mu        sync.Mutex
}

// NewClient creates a client and derives its effective identity only from JWT claims.
func NewClient(hub *Hub, conn *websocket.Conn, claims *auth.Claims) *Client {
	return &Client{
		hub:    hub,
		conn:   conn,
		send:   make(chan []byte, 64),
		id:     effectiveClientID(claims),
		claims: cloneClaims(claims),
	}
}

// ReadPump reads WebSocket messages and dispatches them to the hub.
func (c *Client) ReadPump() {
	defer func() {
		c.hub.unregister <- c
		c.conn.Close()
	}()

	c.conn.SetReadLimit(maxMessageSize)
	c.conn.SetReadDeadline(time.Now().Add(pongWait))
	c.conn.SetPongHandler(func(string) error {
		c.conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})

	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseNormalClosure) {
				slog.Warn("websocket read error", "client", c.id, "err", err)
			}
			return
		}

		var msg Message
		if err := json.Unmarshal(data, &msg); err != nil {
			slog.Warn("invalid message json", "client", c.id, "err", err)
			c.SendMessage(NewErrorMessage("", "invalid json"))
			continue
		}

		c.hub.dispatch(c, msg)
	}
}

// WritePump writes queued messages to the WebSocket.
func (c *Client) WritePump() {
	ticker := time.NewTicker(pingInterval)
	defer func() {
		ticker.Stop()
		c.conn.Close()
	}()

	for {
		select {
		case data, ok := <-c.send:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				c.conn.WriteMessage(websocket.CloseMessage, nil)
				return
			}
			if err := c.conn.WriteMessage(websocket.TextMessage, data); err != nil {
				slog.Warn("websocket write error", "client", c.id, "err", err)
				return
			}

		case <-ticker.C:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}

// SendMessage serializes and enqueues a message for the client.
func (c *Client) SendMessage(msg Message) {
	data, err := json.Marshal(msg)
	if err != nil {
		slog.Error("failed to marshal message", "err", err)
		return
	}
	select {
	case c.send <- data:
	default:
		slog.Warn("client send buffer full, dropping message", "client", c.id)
	}
}

// SetSession sets the current signaling session and role.
func (c *Client) SetSession(sessionID, role string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.sessionID = sessionID
	c.role = role
}

// GetSession returns the current signaling session and role.
func (c *Client) GetSession() (string, string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sessionID, c.role
}

func (c *Client) Claims() *auth.Claims {
	return cloneClaims(c.claims)
}

func (c *Client) ID() string {
	return c.id
}

func effectiveClientID(claims *auth.Claims) string {
	if claims == nil {
		return ""
	}
	if claims.IsMachine() && claims.MachineID != "" {
		return claims.MachineID
	}
	return claims.UserID
}

func cloneClaims(claims *auth.Claims) *auth.Claims {
	if claims == nil {
		return nil
	}
	copied := *claims
	return &copied
}
