// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Shared WebRTC backend selection used by both receiver and broadcaster code paths.
// Exposed to UPROPERTY/Blueprint, so declare as UENUM here to avoid duplicate UI enums.
UENUM(BlueprintType)
enum class EO3DSWebRtcBackend : uint8
{
    LibDataChannel UMETA(DisplayName = "Peer-to-Peer (libdatachannel)"),
    LiveKit       UMETA(DisplayName = "LiveKit SFU")
};
