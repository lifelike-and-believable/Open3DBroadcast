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

const http = require('http');
const { WebSocketServer } = require('ws');

const args = process.argv.slice(2);
const portFlagIndex = args.indexOf('--port');
const PORT = portFlagIndex >= 0 ? parseInt(args[portFlagIndex + 1], 10) : 8080;

// In-memory rooms: roomName -> Set of clients
const rooms = new Map();

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
  if (set.size === 0) {
    rooms.delete(room);
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
        ws.send(JSON.stringify({ type: 'error', message: 'room required' }));
        return;
      }
      ws.room = room;
      ws.name = msg.name || 'peer';
      addToRoom(room, ws);
      // Ack join
      ws.send(JSON.stringify({ type: 'joined' }));
      // Notify others
      broadcastToRoom(room, ws, { type: 'peer-joined' });
      return;
    }

    // Relay typical WebRTC negotiation messages
    if (!ws.room) return;

    switch (type) {
      case 'offer':
        if (typeof msg.sdp === 'string') {
          broadcastToRoom(ws.room, ws, { type: 'offer', sdp: msg.sdp });
        }
        break;
      case 'answer':
        if (typeof msg.sdp === 'string') {
          broadcastToRoom(ws.room, ws, { type: 'answer', sdp: msg.sdp });
        }
        break;
      case 'ice':
        if (typeof msg.candidate === 'string') {
          broadcastToRoom(ws.room, ws, {
            type: 'ice',
            candidate: msg.candidate,
            sdpMid: msg.sdpMid || '',
            sdpMLineIndex: typeof msg.sdpMLineIndex === 'number' ? msg.sdpMLineIndex : 0,
          });
        }
        break;
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
