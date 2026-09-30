#!/usr/bin/env python3
"""
Mock Token Server for Open3DTransportWebRTC Testing

A small HTTP server that answers token requests with JWT-format tokens, for testing token
auto-fetch without a full token service. It is a reference for the request/response contract
only; it is not a production token service.

Usage:
    API_SECRET=<signing secret> python mock-token-server.py [--port 8080] [--host localhost]

Requirements:
    pip install flask pyjwt

Environment Variables:
    API_SECRET (required): Secret for signing JWTs. The server refuses to start without it.
        For tokens a real LiveKit server accepts, use that server's API secret
        (for `livekit-server --dev`: "secret").
    LIVEKIT_API_KEY: JWT issuer (iss). A real LiveKit server only accepts tokens whose issuer is
        one of its API keys (for `livekit-server --dev`: "devkey"). Default: "mock-token-server",
        which a real LiveKit server rejects.
    API_KEY: Credential callers must send as "Authorization: Bearer <API_KEY>". In the plugin this
        is the `webrtc.tokenEndpointAuth` secret (or O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH). Strongly
        recommended: without it, anyone who can reach the server can get a token.
    TOKEN_TTL: Token lifetime in seconds (default: 3600)

Grants are decided here from the requested role (publisher or subscriber). Any "grants" object a
client sends is ignored, so a client cannot give itself more rights.
"""

import argparse
import datetime
import hmac
import json
import os
import sys
from flask import Flask, request, jsonify
import jwt

app = Flask(__name__)

# Configuration. API_SECRET has no default (LIC-1): the server exits in main() when it is unset.
API_KEY = os.environ.get('API_KEY', '')
API_SECRET = os.environ.get('API_SECRET', '')
TOKEN_TTL = int(os.environ.get('TOKEN_TTL', '3600'))  # 1 hour default
TOKEN_ISSUER = os.environ.get('LIVEKIT_API_KEY', 'mock-token-server')

# Grants per role. The server decides these; the client only asks for a role.
ROLE_GRANTS = {
    'publisher': {'roomCreate': True, 'canPublish': True, 'canSubscribe': False},
    'subscriber': {'roomCreate': False, 'canPublish': False, 'canSubscribe': True},
}


@app.route('/health', methods=['GET'])
def health():
    """Health check endpoint"""
    return jsonify({
        'status': 'ok',
        'service': 'mock-token-server'
    })


@app.route('/token', methods=['POST'])
def generate_token():
    """Generate a JWT token for testing."""
    # Check API key if configured
    if API_KEY:
        auth_header = request.headers.get('Authorization', '')
        if not auth_header.startswith('Bearer '):
            return jsonify({'error': 'Missing or invalid Authorization header'}), 401

        provided_key = auth_header[7:]  # Remove "Bearer " prefix
        if not hmac.compare_digest(provided_key.encode('utf-8'), API_KEY.encode('utf-8')):
            return jsonify({'error': 'Invalid API key'}), 401

    # Parse request
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({'error': 'Invalid JSON'}), 400

    # Extract fields. "grants" from the client is ignored on purpose.
    room = data.get('room', 'test-room')
    identity = data.get('identity', 'test-user')
    role = data.get('role', 'publisher')
    if role not in ROLE_GRANTS:
        return jsonify({'error': 'role must be "publisher" or "subscriber"'}), 400
    grants = ROLE_GRANTS[role]
    if 'grants' in data:
        print("[TOKEN] Ignored client-supplied grants; grants come from the server")

    # Calculate expiry in UTC (TRF-35: a naive utcnow().timestamp() is read as local time).
    now = datetime.datetime.now(datetime.timezone.utc)
    expiry = now + datetime.timedelta(seconds=TOKEN_TTL)
    expiry_timestamp = int(expiry.timestamp())

    # Build JWT payload (LiveKit-compatible format)
    payload = {
        'exp': expiry_timestamp,
        'iss': TOKEN_ISSUER,
        'sub': identity,
        'nbf': int(now.timestamp()),
        'video': {
            'room': room,
            'roomJoin': True,
            'roomCreate': grants['roomCreate'],
            'canPublish': grants['canPublish'],
            'canSubscribe': grants['canSubscribe'],
        },
        'metadata': json.dumps({'role': role})
    }

    # Generate token
    token = jwt.encode(payload, API_SECRET, algorithm='HS256')

    # Log request (never the token)
    print(f"[TOKEN] Generated for room={room}, identity={identity}, role={role}, ttl={TOKEN_TTL}s")

    # Return response
    return jsonify({
        'token': token,
        'expiresAt': expiry_timestamp,
        'ttl': TOKEN_TTL
    })


def main():
    parser = argparse.ArgumentParser(description='Mock Token Server')
    parser.add_argument('--host', default='localhost', help='Host to bind to')
    parser.add_argument('--port', type=int, default=8080, help='Port to listen on')
    parser.add_argument('--debug', action='store_true', help='Enable Flask debug mode')
    args = parser.parse_args()

    if not API_SECRET:
        print("error: API_SECRET is not set. Set it to the JWT signing secret "
              "(for tokens a LiveKit server accepts, that server's API secret).", file=sys.stderr)
        sys.exit(1)

    print(f"Mock Token Server starting on http://{args.host}:{args.port}")
    print(f"API Key: {'(not set)' if not API_KEY else '****'}")
    if not API_KEY:
        print("warning: API_KEY is not set, so any caller that can reach this server gets a token. "
              "Set API_KEY and give the plugin the same value as its token endpoint credential.")
    if args.host not in ('localhost', '127.0.0.1', '::1'):
        print("warning: the plugin only accepts plain http:// token endpoints on localhost, 127.0.0.1 "
              "or ::1. Put this server behind HTTPS to use it from another machine.")
    print(f"Token issuer: {TOKEN_ISSUER}")
    print(f"Token TTL: {TOKEN_TTL} seconds")
    print()

    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == '__main__':
    main()
