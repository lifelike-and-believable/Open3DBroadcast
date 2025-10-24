import { AccessToken } from 'livekit-server-sdk';

const apiKey = process.env.LK_API_KEY || 'REPLACE_WITH_API_KEY';
const apiSecret = process.env.LK_API_SECRET || 'REPLACE_WITH_API_SECRET';

// Customize if needed
const roomName = process.env.LK_ROOM || 'ue-test';
const identity = process.env.LK_ID || 'unreal-sender-1';

// Grants: allow publish/subscribe and create the room if needed
const at = new AccessToken(apiKey, apiSecret, { identity });
at.addGrant({
  room: roomName,
  roomCreate: true,
  canPublish: true,
  canSubscribe: true,
});

const token = await at.toJwt();
console.log(token);