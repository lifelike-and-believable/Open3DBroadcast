# WebRTC DataChannel Quick Start (M3.4)

Purpose
- Verify mocap broadcast over WebRTC DataChannel from the Open3DBroadcast plugin to the Open3DStream LiveLink Source.
- Minimal steps for a local or LAN test.

Prereqs
- Signaling server available (see repository WebRTC docs). Example: `examples/signaling-server.js` on port 8080.
- Both Editor instances can reach the signaling server host/port. Configure STUN if crossing NAT.

Sender (Broadcast) — Editor Instance A
1) Add an actor with `UO3DSBroadcastComponent` and `UO3DSBroadcastTransportAdapter` (or enable the component’s built-in transport).
2) Set in the adapter (or component transport):
   - `Transport = WebRTC`
   - `Url = webrtc://<signaling-host>:8080/o3ds?role=server&stun=stun:stun.l.google.com:19302`
   - Optionally set `o3ds.Broadcast.Enable=1` to force-enable at runtime.
3) Play In Editor so the broadcaster starts sending serialized frames.
4) Use: `o3ds.Broadcast.Transport.DumpTransportStats` to see Connected/FramesSent/BytesSent.

Receiver (LiveLink) — Editor Instance B
1) Add LiveLink Source: Open3DStream.
2) Protocol: `WebRTC Client`.
3) Url: `webrtc://<signaling-host>:8080/o3ds` (room must match the sender URL).
4) Press OK. In the LiveLink panel, watch for subject(s) appearing and updating.

Notes
- Roles must be complementary: Sender role=server pairs with LiveLink Protocol=`WebRTC Client`. If using sender role=client, select `WebRTC Server` in LiveLink.
- For localhost testing, use `webrtc://localhost:8080/o3ds?...` on both.
- STUN is optional on LAN; recommended across NAT. Add `&stun=stun:stun.l.google.com:19302`.
- If no subjects appear, check the Output Log for status from `Open3DStream` and verify the signaling server is reachable.

Troubleshooting
- Connection fails: confirm signaling server is running, roles are complementary, and room matches.
- No data: ensure the broadcast component is capturing (auto-start on BeginPlay or call StartCapture), and that `o3ds.Broadcast.Enable=1` if using the adapter.
- Firewall: open the signaling port (default 8080) and allow outbound UDP for ICE candidates.

References
- Broadcast side: `UO3DSBroadcastComponent`, `UO3DSBroadcastTransportAdapter`, `FO3DSWebRtcTransport`.
- Receiver side: `FOpen3DStreamSource`, `O3DSServer` (WebRTC Client/Server), `FWebRTCConnector`.
- Docs: `BROADCAST_TRANSPORT_GUIDE.md`, `WEBRTC_IMPLEMENTATION_SUMMARY.md`.
