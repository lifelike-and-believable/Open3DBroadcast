#!/usr/bin/env python3
"""
Mock Token Server for Open3DTransportWebRTC Testing

A simple HTTP server that responds to token requests with valid JWT-format tokens.
Useful for testing token auto-fetch functionality without requiring a full LiveKit deployment.

Usage:
    python mock-token-server.py [--port 8080] [--host localhost]

Requirements:
    pip install flask pyjwt

Environment Variables:
    API_KEY: Expected API key for authentication (optional)
    API_SECRET: Secret for signing JWTs (default: "test-secret")
    TOKEN_TTL: Token lifetime in seconds (default: 3600)
"""

import argparse
import datetime
import json
import os
from flask import Flask, request, jsonify
import jwt

app = Flask(__name__)

# Configuration
API_KEY = os.environ.get('API_KEY', '')
API_SECRET = os.environ.get('API_SECRET', 'test-secret')
TOKEN_TTL = int(os.environ.get('TOKEN_TTL', '3600'))  # 1 hour default

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
        if provided_key != API_KEY:
            return jsonify({'error': 'Invalid API key'}), 401

    # Parse request
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'Invalid JSON'}), 400
    except Exception as e:
        return jsonify({'error': f'Failed to parse JSON: {str(e)}'}), 400

    # Extract fields
    room = data.get('room', 'test-room')
    identity = data.get('identity', 'test-user')
    role = data.get('role', 'publisher')
    grants = data.get('grants', {})

    # Calculate expiry
    now = datetime.datetime.utcnow()
    expiry = now + datetime.timedelta(seconds=TOKEN_TTL)
    expiry_timestamp = int(expiry.timestamp())

    # Build JWT payload (LiveKit-compatible format)
    payload = {
        'exp': expiry_timestamp,
        'iss': 'mock-token-server',
        'sub': identity,
        'nbf': int(now.timestamp()),
        'video': {
            'room': room,
            'roomJoin': True,
            'roomCreate': grants.get('roomCreate', role == 'publisher'),
            'canPublish': grants.get('canPublish', role == 'publisher'),
            'canSubscribe': grants.get('canSubscribe', role == 'subscriber'),
        },
        'metadata': json.dumps({'role': role})
    }

    # Generate token
    token = jwt.encode(payload, API_SECRET, algorithm='HS256')

    # Log request
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

    print(f"Mock Token Server starting on http://{args.host}:{args.port}")
    print(f"API Key: {'(not set)' if not API_KEY else '****'}")
    print(f"Token TTL: {TOKEN_TTL} seconds")
    print()

    app.run(host=args.host, port=args.port, debug=args.debug)

if __name__ == '__main__':
    main()
