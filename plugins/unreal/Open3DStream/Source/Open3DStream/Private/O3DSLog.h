// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Dedicated logging category for O3DS WebRTC audio send/receive diagnostics
// Use O3DSWebRTCAudioLog to isolate audio-related logs from other O3DS systems
DECLARE_LOG_CATEGORY_EXTERN(O3DSWebRTCAudioLog, Log, All);

// Backward compatibility: previously named LogO3DSAudio. Keep an alias so existing
// call sites continue to compile while we migrate usage.
#ifndef LogO3DSAudio
#define LogO3DSAudio O3DSWebRTCAudioLog
#endif
