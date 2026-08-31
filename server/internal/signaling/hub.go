package signaling

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"sync"

	"github.com/azvlucas/remotex-portfolio/server/internal/observability"
	"github.com/azvlucas/remotex-portfolio/server/pkg/redis"
)

// Hub manages WebSocket clients and signaling/chat routing.
type Hub struct {
	clients   map[string]*Client
	clientsMu sync.RWMutex

	register   chan *Client
	unregister chan *Client

	authorizer AccessAuthorizer
	sessions   *SessionStore
	redisBus   *redis.PubSub
	nodeID     string

	chatSubs   map[string][]*Client
	chatSubsMu sync.RWMutex
}

// NewHub creates a hub. redisBus can be nil for single-node mode.
func NewHub(redisBus *redis.PubSub, authorizer AccessAuthorizer, nodeID string) *Hub {
	return &Hub{
		clients:    make(map[string]*Client),
		register:   make(chan *Client),
		unregister: make(chan *Client),
		authorizer: authorizer,
		sessions:   NewSessionStore(),
		redisBus:   redisBus,
		nodeID:     nodeID,
		chatSubs:   make(map[string][]*Client),
	}
}

// Run starts the hub loop.
func (h *Hub) Run() {
	if h.redisBus != nil {
		go h.subscribeRedis()
		go h.subscribeSessionSync()
	}

	for {
		select {
		case client := <-h.register:
			h.registerClient(client)

		case client := <-h.unregister:
			h.unregisterClient(client)
		}
	}
}

// Register adds a client and starts its pumps.
func (h *Hub) Register(client *Client) {
	h.register <- client
	go client.WritePump()
	go client.ReadPump()
}

func (h *Hub) registerClient(client *Client) {
	h.clientsMu.Lock()
	h.clients[client.id] = client
	active := len(h.clients)
	h.clientsMu.Unlock()
	observability.SetSignalingActiveConnections(active)
	slog.Info("client registered", "client", client.id)
}

func (h *Hub) unregisterClient(client *Client) {
	sessionID, role := client.GetSession()

	h.clientsMu.Lock()
	if _, ok := h.clients[client.id]; ok {
		delete(h.clients, client.id)
		close(client.send)
	}
	active := len(h.clients)
	h.clientsMu.Unlock()
	observability.SetSignalingActiveConnections(active)

	h.removeChatSubscriber(client)

	peer, _ := h.sessions.Leave(client)
	if peer != nil {
		peer.SendMessage(Message{
			Type:      MsgPeerLeft,
			SessionID: sessionID,
		})
	} else if h.redisBus != nil && sessionID != "" {
		// Peer may be on another node — publish leave via Redis.
		h.publishSessionLeave(sessionID, role)
	}
	slog.Info("client unregistered", "client", client.id)
}

func (h *Hub) ActiveClientCount() int {
	h.clientsMu.RLock()
	defer h.clientsMu.RUnlock()
	return len(h.clients)
}

// dispatch processes a message received from a client.
func (h *Hub) dispatch(sender *Client, msg Message) {
	switch msg.Type {
	case MsgSessionJoin:
		h.handleSessionJoin(sender, msg)
	case MsgSessionLeave:
		h.handleSessionLeave(sender)
	case MsgOffer, MsgAnswer, MsgICECandidate, MsgError:
		h.handleRelay(sender, msg)
	case MsgChatJoin:
		h.handleChatJoin(sender, msg)
	case MsgChatLeave:
		h.handleChatLeave(sender, msg)
	case MsgChat:
		h.handleChat(sender, msg)
	default:
		slog.Warn("unknown message type", "type", msg.Type, "client", sender.id)
		sender.SendMessage(NewErrorMessage(msg.SessionID, "unknown message type: "+msg.Type))
	}
}

func (h *Hub) handleSessionJoin(sender *Client, msg Message) {
	if msg.SessionID == "" {
		observability.RecordSignalingEvent(observability.SignalingSurfaceSessionJoin, observability.SignalingReasonMissingSessionID)
		sender.SendMessage(NewErrorMessage("", "session_id is required"))
		return
	}
	if msg.Role != RoleHost && msg.Role != RoleViewer {
		observability.RecordSignalingEvent(observability.SignalingSurfaceSessionJoin, observability.SignalingReasonInvalidRole)
		sender.SendMessage(NewErrorMessage(msg.SessionID, "role must be 'host' or 'viewer'"))
		return
	}
	if err := h.authorizeSessionJoin(sender, msg); err != nil {
		return
	}

	_, peer := h.sessions.Join(msg.SessionID, msg.Role, sender)
	if peer != nil {
		peer.SendMessage(Message{
			Type:      MsgPeerJoined,
			SessionID: msg.SessionID,
			Role:      msg.Role,
		})
		peerSessionID, peerRole := peer.GetSession()
		sender.SendMessage(Message{
			Type:      MsgPeerJoined,
			SessionID: peerSessionID,
			Role:      peerRole,
		})
	} else if h.redisBus != nil {
		// No local peer — announce to other nodes via Redis.
		h.publishSessionJoin(msg.SessionID, msg.Role, false)
	}
}

func (h *Hub) handleSessionLeave(sender *Client) {
	sessionID, role := sender.GetSession()
	peer, _ := h.sessions.Leave(sender)
	if peer != nil {
		peer.SendMessage(Message{
			Type:      MsgPeerLeft,
			SessionID: sessionID,
		})
	} else if h.redisBus != nil && sessionID != "" {
		h.publishSessionLeave(sessionID, role)
	}
}

func (h *Hub) handleRelay(sender *Client, msg Message) {
	sessionID, _ := sender.GetSession()
	if sessionID == "" {
		observability.RecordSignalingEvent(observability.SignalingSurfaceSessionRelay, observability.SignalingReasonNotInSession)
		sender.SendMessage(NewErrorMessage("", "not in a session, send session_join first"))
		return
	}
	msg.SessionID = sessionID

	peer := h.sessions.GetPeer(sender)
	if peer != nil {
		peer.SendMessage(msg)
		return
	}

	if h.redisBus != nil {
		h.publishRedis(sender, msg)
		return
	}

	sender.SendMessage(NewErrorMessage(sessionID, "peer not connected"))
	observability.RecordSignalingEvent(observability.SignalingSurfaceSessionRelay, observability.SignalingReasonPeerNotConnected)
}

func (h *Hub) handleChatJoin(sender *Client, msg Message) {
	if msg.MachineID == "" {
		observability.RecordSignalingEvent(observability.SignalingSurfaceChatJoin, observability.SignalingReasonMissingMachineID)
		sender.SendMessage(NewErrorMessage("", "machine_id is required for chat_join"))
		return
	}
	if err := h.authorizeChat(sender, msg.MachineID, MsgChatJoin, observability.SignalingSurfaceChatJoin); err != nil {
		return
	}

	h.chatSubsMu.Lock()
	defer h.chatSubsMu.Unlock()

	subs := h.chatSubs[msg.MachineID]
	for _, c := range subs {
		if c == sender {
			return
		}
	}
	h.chatSubs[msg.MachineID] = append(subs, sender)
	slog.Info("chat subscriber added", "machine_id", msg.MachineID, "client", sender.id)
}

func (h *Hub) handleChatLeave(sender *Client, msg Message) {
	if msg.MachineID == "" {
		return
	}

	h.chatSubsMu.Lock()
	defer h.chatSubsMu.Unlock()

	subs := h.chatSubs[msg.MachineID]
	for i, c := range subs {
		if c == sender {
			h.chatSubs[msg.MachineID] = append(subs[:i], subs[i+1:]...)
			break
		}
	}
	if len(h.chatSubs[msg.MachineID]) == 0 {
		delete(h.chatSubs, msg.MachineID)
	}
	slog.Info("chat subscriber removed", "machine_id", msg.MachineID, "client", sender.id)
}

func (h *Hub) handleChat(sender *Client, msg Message) {
	if msg.MachineID == "" {
		observability.RecordSignalingEvent(observability.SignalingSurfaceChatMessage, observability.SignalingReasonMissingMachineID)
		sender.SendMessage(NewErrorMessage("", "machine_id is required for chat"))
		return
	}
	if err := h.authorizeChat(sender, msg.MachineID, MsgChat, observability.SignalingSurfaceChatMessage); err != nil {
		return
	}

	claims := sender.Claims()
	isHost := claims != nil && claims.IsMachine() && sender.id == msg.MachineID

	if isHost {
		h.chatSubsMu.RLock()
		subs := h.chatSubs[msg.MachineID]
		for _, c := range subs {
			c.SendMessage(msg)
		}
		h.chatSubsMu.RUnlock()
		return
	}

	h.clientsMu.RLock()
	host, exists := h.clients[msg.MachineID]
	h.clientsMu.RUnlock()

	if exists {
		host.SendMessage(msg)
		return
	}
	if h.redisBus != nil {
		h.publishRedisChatToHost(msg)
		return
	}

	sender.SendMessage(NewErrorMessage("", "machine not connected"))
	observability.RecordSignalingEvent(observability.SignalingSurfaceChatMessage, observability.SignalingReasonMachineNotConnected)
}

func (h *Hub) authorizeSessionJoin(sender *Client, msg Message) error {
	if h.authorizer == nil {
		return nil
	}

	err := h.authorizer.AuthorizeSessionJoin(context.Background(), sender.Claims(), msg.SessionID, msg.Role)
	if err == nil {
		return nil
	}

	slog.Warn("session join denied",
		"session_id", msg.SessionID,
		"role", msg.Role,
		"client", sender.id,
		"err", err,
	)
	if errors.Is(err, ErrAccessDenied) {
		observability.RecordSignalingEvent(observability.SignalingSurfaceSessionJoin, observability.SignalingReasonAccessDenied)
		sender.SendMessage(NewErrorMessage(msg.SessionID, "session access denied"))
		return err
	}

	observability.RecordSignalingEvent(observability.SignalingSurfaceSessionJoin, observability.SignalingReasonAuthorizationFailed)
	sender.SendMessage(NewErrorMessage(msg.SessionID, "authorization failed"))
	return err
}

func (h *Hub) authorizeChat(sender *Client, machineID, action, surface string) error {
	if h.authorizer == nil {
		return nil
	}

	err := h.authorizer.AuthorizeChat(context.Background(), sender.Claims(), machineID)
	if err == nil {
		return nil
	}

	slog.Warn("chat access denied",
		"action", action,
		"machine_id", machineID,
		"client", sender.id,
		"err", err,
	)
	if errors.Is(err, ErrAccessDenied) {
		observability.RecordSignalingEvent(surface, observability.SignalingReasonAccessDenied)
		sender.SendMessage(NewErrorMessage("", "chat access denied"))
		return err
	}

	observability.RecordSignalingEvent(surface, observability.SignalingReasonAuthorizationFailed)
	sender.SendMessage(NewErrorMessage("", "authorization failed"))
	return err
}

func (h *Hub) removeChatSubscriber(client *Client) {
	h.chatSubsMu.Lock()
	defer h.chatSubsMu.Unlock()

	for machineID, subs := range h.chatSubs {
		for i, c := range subs {
			if c == client {
				h.chatSubs[machineID] = append(subs[:i], subs[i+1:]...)
				break
			}
		}
		if len(h.chatSubs[machineID]) == 0 {
			delete(h.chatSubs, machineID)
		}
	}
}

const redisChannel = "remotex:signaling"
const redisChatChannel = "remotex:chat"
const redisSessionChannel = "remotex:session_sync"

// SessionSyncEnvelope is used for cross-node session join/leave events.
type SessionSyncEnvelope struct {
	NodeID    string `json:"node_id"`
	SessionID string `json:"session_id"`
	Role      string `json:"role"`
	EventType string `json:"event_type"` // "join", "join_ack", "leave"
	AckRole   string `json:"ack_role,omitempty"`
}

func (h *Hub) publishSessionJoin(sessionID, role string, isAck bool) {
	eventType := "join"
	if isAck {
		eventType = "join_ack"
	}
	env := SessionSyncEnvelope{
		NodeID:    h.nodeID,
		SessionID: sessionID,
		Role:      role,
		EventType: eventType,
	}
	data, err := json.Marshal(env)
	if err != nil {
		return
	}
	if err := h.redisBus.Publish(redisSessionChannel, data); err != nil {
		slog.Error("failed to publish session join to redis", "err", err)
	}
}

func (h *Hub) publishSessionLeave(sessionID, role string) {
	env := SessionSyncEnvelope{
		NodeID:    h.nodeID,
		SessionID: sessionID,
		Role:      role,
		EventType: "leave",
	}
	data, err := json.Marshal(env)
	if err != nil {
		return
	}
	if err := h.redisBus.Publish(redisSessionChannel, data); err != nil {
		slog.Error("failed to publish session leave to redis", "err", err)
	}
}

func oppositeRole(role string) string {
	if role == RoleHost {
		return RoleViewer
	}
	return RoleHost
}

func (h *Hub) subscribeSessionSync() {
	ch, err := h.redisBus.Subscribe(redisSessionChannel)
	if err != nil {
		slog.Error("failed to subscribe to session sync channel", "err", err)
		return
	}

	slog.Info("session sync subscriber started", "channel", redisSessionChannel)

	for data := range ch {
		var env SessionSyncEnvelope
		if err := json.Unmarshal([]byte(data), &env); err != nil {
			slog.Warn("invalid session sync envelope", "err", err)
			continue
		}

		// Skip our own broadcasts.
		if env.NodeID == h.nodeID {
			continue
		}

		switch env.EventType {
		case "join":
			// Another node has a client that joined a session.
			// Check if we have the complementary role locally.
			wantRole := oppositeRole(env.Role)
			localClient := h.findLocalSessionClient(env.SessionID, wantRole)
			if localClient != nil {
				slog.Info("cross-node peer found",
					"session", env.SessionID,
					"remote_role", env.Role,
					"local_role", wantRole,
					"remote_node", env.NodeID,
				)
				// Send PeerJoined to our local client.
				localClient.SendMessage(Message{
					Type:      MsgPeerJoined,
					SessionID: env.SessionID,
					Role:      env.Role,
				})
				// Acknowledge back so the remote node also sends PeerJoined.
				h.publishSessionJoin(env.SessionID, wantRole, true)
			}

		case "join_ack":
			// Another node acknowledged our join — they have the peer.
			wantRole := oppositeRole(env.Role)
			localClient := h.findLocalSessionClient(env.SessionID, wantRole)
			if localClient != nil {
				localClient.SendMessage(Message{
					Type:      MsgPeerJoined,
					SessionID: env.SessionID,
					Role:      env.Role,
				})
			}

		case "leave":
			// A peer on another node left. Notify our local client.
			wantRole := oppositeRole(env.Role)
			localClient := h.findLocalSessionClient(env.SessionID, wantRole)
			if localClient != nil {
				localClient.SendMessage(Message{
					Type:      MsgPeerLeft,
					SessionID: env.SessionID,
				})
			}
		}
	}
}

// findLocalSessionClient finds a client in a specific session with a specific role.
func (h *Hub) findLocalSessionClient(sessionID, role string) *Client {
	h.clientsMu.RLock()
	defer h.clientsMu.RUnlock()

	for _, client := range h.clients {
		sid, r := client.GetSession()
		if sid == sessionID && r == role {
			return client
		}
	}
	return nil
}

// RedisEnvelope encapsulates a message and its target for inter-node routing.
type RedisEnvelope struct {
	TargetSessionID string  `json:"target_session_id"`
	TargetRole      string  `json:"target_role"`
	TargetClientID  string  `json:"target_client_id,omitempty"`
	Msg             Message `json:"msg"`
}

func (h *Hub) publishRedis(sender *Client, msg Message) {
	sessionID, senderRole := sender.GetSession()

	targetRole := ""
	switch senderRole {
	case RoleHost:
		targetRole = RoleViewer
	case RoleViewer:
		targetRole = RoleHost
	}

	env := RedisEnvelope{
		TargetSessionID: sessionID,
		TargetRole:      targetRole,
		Msg:             msg,
	}

	data, err := json.Marshal(env)
	if err != nil {
		slog.Error("failed to marshal redis envelope", "err", err)
		return
	}

	if err := h.redisBus.Publish(redisChannel, data); err != nil {
		slog.Error("failed to publish to redis", "err", err)
	}
}

func (h *Hub) publishRedisChatToHost(msg Message) {
	env := RedisEnvelope{
		TargetClientID: msg.MachineID,
		Msg:            msg,
	}

	data, err := json.Marshal(env)
	if err != nil {
		slog.Error("failed to marshal redis chat envelope", "err", err)
		return
	}

	if err := h.redisBus.Publish(redisChatChannel, data); err != nil {
		slog.Error("failed to publish chat to redis", "err", err)
	}
}

func (h *Hub) subscribeRedis() {
	ch, err := h.redisBus.Subscribe(redisChannel)
	if err != nil {
		slog.Error("failed to subscribe to redis", "err", err)
		return
	}

	slog.Info("redis subscriber started", "channel", redisChannel)
	go h.subscribeRedisChatChannel()

	for data := range ch {
		var env RedisEnvelope
		if err := json.Unmarshal([]byte(data), &env); err != nil {
			slog.Warn("invalid redis envelope", "err", err)
			continue
		}

		h.clientsMu.RLock()
		for _, client := range h.clients {
			sid, role := client.GetSession()
			if sid != env.TargetSessionID {
				continue
			}
			if env.TargetRole != "" && role != env.TargetRole {
				continue
			}
			client.SendMessage(env.Msg)
		}
		h.clientsMu.RUnlock()
	}
}

func (h *Hub) subscribeRedisChatChannel() {
	ch, err := h.redisBus.Subscribe(redisChatChannel)
	if err != nil {
		slog.Error("failed to subscribe to redis chat channel", "err", err)
		return
	}

	slog.Info("redis chat subscriber started", "channel", redisChatChannel)

	for data := range ch {
		var env RedisEnvelope
		if err := json.Unmarshal([]byte(data), &env); err != nil {
			slog.Warn("invalid redis chat envelope", "err", err)
			continue
		}

		if env.TargetClientID == "" {
			continue
		}

		h.clientsMu.RLock()
		if client, ok := h.clients[env.TargetClientID]; ok {
			client.SendMessage(env.Msg)
		}
		h.clientsMu.RUnlock()
	}
}
