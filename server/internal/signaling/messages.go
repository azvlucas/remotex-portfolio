package signaling

import "encoding/json"

// Tipos de mensagem trocadas via WebSocket.
const (
	MsgSessionJoin  = "session_join"
	MsgSessionLeave = "session_leave"
	MsgOffer        = "offer"
	MsgAnswer       = "answer"
	MsgICECandidate = "ice_candidate"
	MsgError        = "error"
	MsgPeerJoined   = "peer_joined"
	MsgPeerLeft     = "peer_left"
	MsgChat         = "chat"
	MsgChatJoin     = "chat_join"
	MsgChatLeave    = "chat_leave"
)

// Roles dentro de uma sessão.
const (
	RoleHost   = "host"
	RoleViewer = "viewer"
)

// Message é o envelope JSON trocado entre cliente e servidor.
type Message struct {
	Type      string          `json:"type"`
	SessionID string          `json:"session_id,omitempty"`
	Role      string          `json:"role,omitempty"`
	MachineID string          `json:"machine_id,omitempty"`
	Payload   json.RawMessage `json:"payload,omitempty"`
	Error     string          `json:"error,omitempty"`
}

// OfferPayload contém o SDP offer.
type OfferPayload struct {
	SDP string `json:"sdp"`
}

// AnswerPayload contém o SDP answer.
type AnswerPayload struct {
	SDP string `json:"sdp"`
}

// ICECandidatePayload contém um candidato ICE.
type ICECandidatePayload struct {
	Candidate     string `json:"candidate"`
	SDPMid        string `json:"sdp_mid"`
	SDPMLineIndex int    `json:"sdp_mline_index"`
}

// NewErrorMessage cria uma mensagem de erro.
func NewErrorMessage(sessionID, text string) Message {
	return Message{
		Type:      MsgError,
		SessionID: sessionID,
		Error:     text,
	}
}
