// Simple WebRTC signaling server for Open3DStream testing
// Usage:
//   1) Install deps: npm install
//   2) Run: node signaling-server.js [--port 8080]
//
// Protocol (JSON over WebSocket at /ws):
//   Client sends: { type: "join", room: "<room>", name: "unreal-client|unreal-server" }
//   Server replies: { type: "joined" }
//   Server notifies others: { type: "peer-joined" }
//   Offer/Answer relayed: { type: "offer"|"answer", sdp: "..." }
//   ICE relayed: { type: "ice", candidate, sdpMid, sdpMLineIndex }
//   On disconnect, server notifies others: { type: "peer-left" }
//
// Enhancement (Issue #87):
// - Late-join resilience via latest-offer cache per room with TTL (default 5s).
//   When a peer joins, if a recent offer is cached for the room, it is replayed
//   to the newcomer so they can answer even if the remote offered before join.
//
// Enhancement: Glare handling (simultaneous offers)
// - Introduces a per-room negotiation lock window so only one offer is accepted
//   at a time. If another peer sends an offer while a negotiation is in-flight,
//   the server responds with { type: "collision", action: "wait-retry", retryAfterMs }
//   and drops the colliding offer. The lock is cleared on answer or timeout.

const http = require('http');
const { WebSocketServer } = require('ws');

const args = process.argv.slice(2);
const portFlagIndex = args.indexOf('--port');
const PORT = portFlagIndex >= 0 ? parseInt(args[portFlagIndex + 1], 10) : 8080;
const OFFER_TTL_MS = 5000; // configurable TTL for cached offers
const NEGOTIATION_WINDOW_MS = 3000; // soft lock window to serialize offers

// In-memory rooms: roomName -> Set of clients
const rooms = new Map();
// Latest offer cache: roomName -> { sdp: string, time: number }
const latestOffers = new Map();
// Per-room negotiation lock to mitigate glare: roomName -> { offerer: WebSocket|null, lockUntil: number }
const negotiations = new Map();

function ensureNegotiation(room) {
  if (!negotiations.has(room)) {
    negotiations.set(room, { offerer: null, lockUntil: 0 });
  }
  return negotiations.get(room);
}

function addToRoom(room, ws) {
  if (!rooms.has(room)) {
    rooms.set(room, new Set());
  }
  rooms.get(room).add(ws);
}

function removeFromRoom(room, ws) {
  if (!rooms.has(room)) return;
  const set = rooms.get(room);
  set.delete(ws);
  const nego = negotiations.get(room);
  // If the leaving peer was the current offerer, release the lock early
  if (nego && nego.offerer === ws) {
    nego.offerer = null;
    nego.lockUntil = 0;
  }
  if (set.size === 0) {
    rooms.delete(room);
    latestOffers.delete(room);
    negotiations.delete(room);
  }
}

function broadcastToRoom(room, sender, messageObj) {
  const set = rooms.get(room);
  if (!set) return;
  const msg = JSON.stringify(messageObj);
  for (const client of set) {
    if (client !== sender && client.readyState === 1) {
      client.send(msg);
    }
  }
}

function sendTo(ws, obj) {
  if (ws.readyState === 1) {
    ws.send(JSON.stringify(obj));
  }
}

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('Open3DStream signaling server is running. Connect via WebSocket at /ws.\n');
});

const wss = new WebSocketServer({ server, path: '/ws' });

wss.on('connection', (ws) => {
  ws.room = null;
  ws.name = null;

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch (e) {
      console.warn('Invalid JSON:', e);
      return;
    }

    const type = msg.type;

    if (type === 'join') {
      const room = String(msg.room || '').trim();
      if (!room) {
        sendTo(ws, { type: 'error', message: 'room required' });
        return;
      }
      ws.room = room;
      ws.name = msg.name || 'peer';
      addToRoom(room, ws);
      // Ack join
      sendTo(ws, { type: 'joined' });
      // Send cached latest offer if still fresh
      const cached = latestOffers.get(room);
      const now = Date.now();
      if (cached && (now - cached.time) <= OFFER_TTL_MS) {
        sendTo(ws, { type: 'offer', sdp: cached.sdp });
      }
      // Notify others
      broadcastToRoom(room, ws, { type: 'peer-joined' });
      return;
    }

    // Relay typical WebRTC negotiation messages
    if (!ws.room) return;

    switch (type) {
      case 'offer': {
        if (typeof msg.sdp !== 'string') break;
        const room = ws.room;
        const now = Date.now();
        const nego = ensureNegotiation(room);
        // Release stale lock automatically
        if (nego.lockUntil && now > nego.lockUntil) {
          nego.offerer = null;
          nego.lockUntil = 0;
        }
        // Accept if no active offerer or this ws is the active offerer (renegotiation)
        const canAccept = !nego.offerer || nego.offerer === ws;
        if (canAccept) {
          nego.offerer = ws;
          nego.lockUntil = now + NEGOTIATION_WINDOW_MS;
          // Cache only accepted offers for late join replay
          latestOffers.set(room, { sdp: msg.sdp, time: now });
          broadcastToRoom(room, ws, { type: 'offer', sdp: msg.sdp });
        } else {
          // Collision: another peer is currently the offerer, ask to retry later
          const retryAfterMs = Math.max(0, nego.lockUntil - now);
          sendTo(ws, { type: 'collision', action: 'wait-retry', retryAfterMs });
          // Do not update cached offer or broadcast the colliding one
        }
        break;
      }
      case 'answer': {
        if (typeof msg.sdp !== 'string') break;
        const room = ws.room;
        broadcastToRoom(room, ws, { type: 'answer', sdp: msg.sdp });
        // Clear negotiation lock on answer receipt
        const nego = ensureNegotiation(room);
        nego.offerer = null;
        nego.lockUntil = 0;
        break;
      }
      case 'ice': {
        if (typeof msg.candidate === 'string') {
          broadcastToRoom(ws.room, ws, {
            type: 'ice',
            candidate: msg.candidate,
            sdpMid: msg.sdpMid || '',
            sdpMLineIndex: typeof msg.sdpMLineIndex === 'number' ? msg.sdpMLineIndex : 0,
          });
        }
        break;
      }
      default:
        // ignore unknown
        break;
    }
  });

  ws.on('close', () => {
    const room = ws.room;
    if (room) {
      removeFromRoom(room, ws);
      broadcastToRoom(room, ws, { type: 'peer-left' });
    }
  });
});

server.listen(PORT, () => {
  console.log(`Open3DStream signaling server listening on http://localhost:${PORT}`);
});
