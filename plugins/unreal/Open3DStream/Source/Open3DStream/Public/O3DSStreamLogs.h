// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Receiver-specific log categories for easy filtering in Output Log
// Usage examples:
//   UE_LOG(LogO3DSReceiver, Log, TEXT("..."));
//   UE_LOG(LogO3DSReceiverWebRTC, Verbose, TEXT("..."));
//   UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("..."));

DECLARE_LOG_CATEGORY_EXTERN(LogO3DSReceiver, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogO3DSReceiverWebRTC, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogO3DSReceiverAudio, Log, All);
