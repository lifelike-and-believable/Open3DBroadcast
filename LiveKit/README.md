# LiveKit on AWS for Open3DStream (livekit.maamawi.dance)

This guide sets up a self-hosted LiveKit WebRTC server behind HTTPS on AWS and connects it to the Open3DStream Unreal plugin.

What you’ll deploy
- LiveKit server (signaling + RTP media) via Docker
- Caddy reverse proxy (automatic HTTPS via Let’s Encrypt)
- Domain: https://livekit.maamawi.dance
- A simple local token generator to create JWTs for the Unreal plugin

Prerequisites
- AWS account with EC2 + Security Groups + Elastic IP
- A domain with DNS control (A record for livekit.maamawi.dance)
- SSH client and a key pair
- Node.js (locally) for token generation (recommended v18+)

Security Group inbound rules (EC2)
- TCP 22: your IP (SSH)
- TCP 80: 0.0.0.0/0 (HTTP for ACME)
- TCP 443: 0.0.0.0/0 (HTTPS)
- UDP 50000–50050: 0.0.0.0/0 (LiveKit media)

DNS
- Create an A record: livekit.maamawi.dance → your instance’s Elastic IP
- Wait a few minutes for propagation

1) Launch and prepare the EC2 instance
- AMI: Ubuntu Server 22.04 LTS (x86_64)
- Size: t3.medium (good starting point)
- Auto-assign public IP: enabled

SSH in:
```bash
ssh -i /path/to/key.pem ubuntu@YOUR_ELASTIC_IP
```

Install Docker and prepare directory:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y docker.io docker-compose-plugin
sudo usermod -aG docker $USER
newgrp docker
sudo mkdir -p /opt/livekit && sudo chown -R $USER:$USER /opt/livekit
cd /opt/livekit
```

2) Add the config files
Place these files in /opt/livekit:
- docker-compose.yml
- livekit.yaml
- Caddyfile

3) Generate API credentials and update livekit.yaml
```bash
API_KEY=$(openssl rand -hex 8)
API_SECRET=$(openssl rand -hex 32)
echo "API_KEY=$API_KEY"
echo "API_SECRET=$API_SECRET"
```
- Edit livekit.yaml and replace:
  - API_KEY_HERE → the value of $API_KEY
  - API_SECRET_HERE → the value of $API_SECRET

4) Start the stack and verify HTTPS
```bash
docker compose up -d
docker compose logs -f caddy
```
- Wait until Caddy obtains a certificate (ACME success)
- Verify:
```bash
curl -I http://livekit.maamawi.dance     # should redirect to https
curl -I https://livekit.maamawi.dance    # should return a valid response (200/404)
```

5) Generate a test token locally (on your laptop)
Create gen-token.mjs (see file in this folder) and run:
```bash
mkdir livekit-token && cd livekit-token
npm init -y
npm install livekit-server-sdk
# Put gen-token.mjs in this folder
LK_API_KEY=YOUR_API_KEY LK_API_SECRET=YOUR_API_SECRET node gen-token.mjs
```
- Copy the printed JWT token

Defaults in the script:
- Room: ue-test
- Identity: unreal-sender-1

6) Configure Unreal Open3DStream plugin
- Server URL: wss://livekit.maamawi.dance
- Room: ue-test
- Token: paste the JWT
- Identity: unreal-sender-1

7) Test
- Start Unreal with the plugin configured
- On the server:
```bash
docker compose logs -f livekit
```
- You should see the client join and publish tracks

Troubleshooting
- DNS/ACME:
  - Ensure livekit.maamawi.dance resolves to your Elastic IP
  - Port 80 must be open for Let’s Encrypt
- Media/connectivity:
  - Ensure UDP 50000–50050 is open in the Security Group
  - Port range in livekit.yaml matches the SG ports
- Token:
  - Unique identity per client
  - Grants must match usage (publish/subscribe)

Optional: TURN for restrictive networks
- Later, add TURN (managed or self-hosted coturn) and configure rtc.ice_servers in livekit.yaml
- Open ports 3478/UDP+TCP, 5349/TCP, and a TURN relay UDP range

Operations
- Update: `docker compose pull && docker compose up -d`
- Logs: `docker compose logs -f caddy` or `docker compose logs -f livekit`
- Security: keep SSH locked to your IP; rotate API keys periodically

Quick reference (Unreal)
- URL: wss://livekit.maamawi.dance
- Room: ue-test
- Identity: unreal-sender-1
- Token: from gen-token.mjs