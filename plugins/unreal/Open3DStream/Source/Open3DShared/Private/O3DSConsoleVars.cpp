// Definitions for centralized O3DS console variables

#include "O3DSConsoleVars.h"

// Logging/diagnostics
TAutoConsoleVariable<int32> CVarO3DSWebRTCVerbose(
    TEXT("o3ds.WebRTC.Verbose"),
    0,
    TEXT("Enable extra verbose logging for WebRTC connector (0/1)."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSWebRTCDebugRx(
    TEXT("o3ds.WebRTC.DebugRx"),
    1,
    TEXT("Enable receiver-side debug logging for WebRTC data (0/1). Logs first packet and occasional stats."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSWebRTCAudioDebug(
    TEXT("o3ds.WebRTC.Audio.Debug"),
    0,
    TEXT("Enable periodic audio send stats (0/1). Logs packets/sec and bytes/sec."),
    ECVF_Default);

// Resiliency and negotiation
TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCAutoReconnect(
    TEXT("o3ds.Broadcast.WebRTC.AutoReconnect"),
    1,
    TEXT("Enable auto-reconnect/re-offer logic on failures (0/1)."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffInitialMs(
    TEXT("o3ds.Broadcast.WebRTC.BackoffInitialMs"),
    500,
    TEXT("Initial backoff for re-offer/reconnect in milliseconds."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffMaxMs(
    TEXT("o3ds.Broadcast.WebRTC.BackoffMaxMs"),
    10000,
    TEXT("Maximum backoff for re-offer/reconnect in milliseconds."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCNegoChannel(
    TEXT("o3ds.Broadcast.WebRTC.NegotiatedChannel"),
    0,
    TEXT("Use negotiated data channel with fixed id on both sides (0/1)."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCChannelId(
    TEXT("o3ds.Broadcast.WebRTC.ChannelId"),
    42,
    TEXT("Fixed DataChannel id to use when NegotiatedChannel=1."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCSignalingJoinTimeoutMs(
    TEXT("o3ds.Broadcast.WebRTC.SignalingJoinTimeoutMs"),
    4000,
    TEXT("If > 0, report an error if signaling doesn't deliver a 'joined' ack within this time (milliseconds)."),
    ECVF_Default);

TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCAudioForceSendRecv(
    TEXT("o3ds.Broadcast.WebRTC.AudioForceSendRecv"),
    0,
    TEXT("If 1, set local audio m-line direction to sendrecv instead of sendonly (helps some stacks open tracks)."),
    ECVF_Default);
