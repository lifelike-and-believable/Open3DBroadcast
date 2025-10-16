#!/usr/bin/env node

/**
 * Simple WebRTC Signaling Server for Open3DStream
 * 
 * This server facilitates WebRTC peer connections by:
 * - Managing rooms for peer grouping
 * - Relaying SDP offers/answers between peers
 * - Forwarding ICE candidates
 * 
 * Usage: node signaling-server.js [port]
 * Default port: 8080
 */

const WebSocket = require('ws');
const http = require('http');

const PORT = process.argv[2] || 8080;

// Store active rooms and their peers
const rooms = new Map();

// Create HTTP server for health checks
const server = http.createServer((req, res) => {
    if (req.url === '/health') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            status: 'ok',
            rooms: rooms.size,
            timestamp: new Date().toISOString()
        }));
    } else {
        res.writeHead(404);
        res.end();
    }
});

// Create WebSocket server
const wss = new WebSocket.Server({ server, path: '/ws' });

console.log(`Open3DStream WebRTC Signaling Server`);
console.log(`Listening on ws://localhost:${PORT}/ws`);
console.log(`Health check: http://localhost:${PORT}/health\n`);

wss.on('connection', (ws, req) => {
    const clientId = generateId();
    let currentRoom = null;
    let peerName = 'unnamed';
    
    console.log(`[${new Date().toISOString()}] Client ${clientId} connected from ${req.socket.remoteAddress}`);
    
    ws.on('message', (data) => {
        try {
            const message = JSON.parse(data.toString());
            handleMessage(ws, clientId, message);
        } catch (err) {
            console.error(`[${clientId}] Failed to parse message:`, err.message);
            ws.send(JSON.stringify({
                type: 'error',
                message: 'Invalid JSON'
            }));
        }
    });
    
    ws.on('close', () => {
        console.log(`[${new Date().toISOString()}] Client ${clientId} (${peerName}) disconnected`);
        leaveRoom(ws, currentRoom);
    });
    
    ws.on('error', (err) => {
        console.error(`[${clientId}] WebSocket error:`, err.message);
    });
    
    function handleMessage(ws, clientId, message) {
        const { type } = message;
        
        switch (type) {
            case 'join':
                handleJoin(ws, clientId, message);
                break;
                
            case 'offer':
            case 'answer':
            case 'candidate':
                relayToPeers(ws, currentRoom, message);
                break;
                
            case 'ping':
                ws.send(JSON.stringify({ type: 'pong' }));
                break;
                
            default:
                console.warn(`[${clientId}] Unknown message type: ${type}`);
        }
    }
    
    function handleJoin(ws, clientId, message) {
        const { roomId, name } = message;
        
        if (!roomId) {
            ws.send(JSON.stringify({
                type: 'error',
                message: 'roomId required'
            }));
            return;
        }
        
        // Leave current room if any
        if (currentRoom) {
            leaveRoom(ws, currentRoom);
        }
        
        // Join new room
        currentRoom = roomId;
        peerName = name || clientId;
        
        if (!rooms.has(roomId)) {
            rooms.set(roomId, new Set());
        }
        
        const room = rooms.get(roomId);
        const wasEmpty = room.size === 0;
        
        room.add(ws);
        
        console.log(`[${new Date().toISOString()}] ${peerName} joined room "${roomId}" (${room.size} peers)`);
        
        // Notify peer of successful join
        ws.send(JSON.stringify({
            type: 'joined',
            roomId,
            peerId: clientId,
            peerCount: room.size,
            isInitiator: !wasEmpty
        }));
        
        // Notify other peers
        room.forEach((peer) => {
            if (peer !== ws && peer.readyState === WebSocket.OPEN) {
                peer.send(JSON.stringify({
                    type: 'peer-joined',
                    peerId: clientId,
                    peerName
                }));
            }
        });
    }
    
    function leaveRoom(ws, roomId) {
        if (!roomId || !rooms.has(roomId)) return;
        
        const room = rooms.get(roomId);
        room.delete(ws);
        
        console.log(`[${new Date().toISOString()}] ${peerName} left room "${roomId}" (${room.size} peers remaining)`);
        
        // Notify remaining peers
        room.forEach((peer) => {
            if (peer.readyState === WebSocket.OPEN) {
                peer.send(JSON.stringify({
                    type: 'peer-left',
                    peerId: clientId
                }));
            }
        });
        
        // Clean up empty rooms
        if (room.size === 0) {
            rooms.delete(roomId);
            console.log(`[${new Date().toISOString()}] Room "${roomId}" closed (empty)`);
        }
    }
    
    function relayToPeers(ws, roomId, message) {
        if (!roomId || !rooms.has(roomId)) {
            console.warn(`[${clientId}] Cannot relay: not in a room`);
            return;
        }
        
        const room = rooms.get(roomId);
        let relayed = 0;
        
        room.forEach((peer) => {
            if (peer !== ws && peer.readyState === WebSocket.OPEN) {
                peer.send(JSON.stringify({
                    ...message,
                    from: clientId
                }));
                relayed++;
            }
        });
        
        console.log(`[${clientId}] Relayed ${message.type} to ${relayed} peers in room "${roomId}"`);
    }
});

function generateId() {
    return Math.random().toString(36).substr(2, 9);
}

// Periodic cleanup of stale connections
setInterval(() => {
    rooms.forEach((room, roomId) => {
        room.forEach((ws) => {
            if (ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING) {
                room.delete(ws);
            }
        });
        
        if (room.size === 0) {
            rooms.delete(roomId);
        }
    });
}, 60000); // Every minute

// Handle graceful shutdown
process.on('SIGINT', () => {
    console.log('\nShutting down signaling server...');
    
    wss.clients.forEach((ws) => {
        ws.send(JSON.stringify({
            type: 'server-shutdown'
        }));
        ws.close();
    });
    
    server.close(() => {
        console.log('Server stopped');
        process.exit(0);
    });
});

// Start server
server.listen(PORT, () => {
    console.log(`Ready to accept WebRTC signaling connections\n`);
});
