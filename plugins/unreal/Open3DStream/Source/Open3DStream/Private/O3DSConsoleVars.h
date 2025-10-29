// Centralized O3DS console variables to avoid unity-build redefinition conflicts

#pragma once

#include "HAL/IConsoleManager.h"

// Logging/diagnostics
extern TAutoConsoleVariable<int32> CVarO3DSWebRTCVerbose;
extern TAutoConsoleVariable<int32> CVarO3DSWebRTCDebugRx;
extern TAutoConsoleVariable<int32> CVarO3DSWebRTCAudioDebug;

// Resiliency and negotiation
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCAutoReconnect;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffInitialMs;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffMaxMs;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCNegoChannel;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCChannelId;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCSignalingJoinTimeoutMs;
extern TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCAudioForceSendRecv;
