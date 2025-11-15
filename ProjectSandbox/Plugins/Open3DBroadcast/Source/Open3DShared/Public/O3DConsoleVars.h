// Definitions for centralized O3D console variables (declarations)

#pragma once

#include "CoreMinimal.h"

// Extern declarations for console variables used across transports/connectors
extern TAutoConsoleVariable<int32> CVarO3DWebRTCVerbose;
extern TAutoConsoleVariable<int32> CVarO3DWebRTCDebugRx;
extern TAutoConsoleVariable<int32> CVarO3DWebRTCAudioDebug;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCAutoReconnect;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCBackoffInitialMs;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCBackoffMaxMs;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCNegoChannel;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCChannelId;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCSignalingJoinTimeoutMs;
extern TAutoConsoleVariable<int32> CVarO3DBroadcastWebRTCAudioForceSendRecv;

// Legacy compatibility aliases (to be removed once downstream code is fully migrated)
#define CVarO3DSWebRTCVerbose              CVarO3DWebRTCVerbose
#define CVarO3DSWebRTCDebugRx              CVarO3DWebRTCDebugRx
#define CVarO3DSWebRTCAudioDebug           CVarO3DWebRTCAudioDebug
#define CVarO3DSBroadcastWebRTCAutoReconnect CVarO3DBroadcastWebRTCAutoReconnect
#define CVarO3DSBroadcastWebRTCBackoffInitialMs CVarO3DBroadcastWebRTCBackoffInitialMs
#define CVarO3DSBroadcastWebRTCBackoffMaxMs CVarO3DBroadcastWebRTCBackoffMaxMs
#define CVarO3DSBroadcastWebRTCNegoChannel CVarO3DBroadcastWebRTCNegoChannel
#define CVarO3DSBroadcastWebRTCChannelId   CVarO3DBroadcastWebRTCChannelId
#define CVarO3DSBroadcastWebRTCSignalingJoinTimeoutMs CVarO3DBroadcastWebRTCSignalingJoinTimeoutMs
#define CVarO3DSBroadcastWebRTCAudioForceSendRecv CVarO3DBroadcastWebRTCAudioForceSendRecv