// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DControlSettings.generated.h"

/**
 * Project-wide settings for receiving control messages (docs/adr/0011-control-channel.md,
 * item 8). Project Settings > Plugins > Open3DBroadcast Control; saved to the project's
 * Config/DefaultGame.ini, which is staged into packaged builds, so ticking Accept Control here
 * is all a project does to ship a client with control on.
 *
 * Receiving is off by default: control triggers gameplay, and UDP and NNG carry no
 * authentication.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Open3DBroadcast Control"))
class OPEN3DRECEIVER_API UO3DControlSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UO3DControlSettings();

	/** Accept control messages (events and values) on receiver sources. Off by default. */
	UPROPERTY(Config, EditAnywhere, Category = "Receiving")
	bool bAcceptControl = false;

	/**
	 * Key and event-name prefixes accepted, for example "env." or "vfx.". Empty accepts every name.
	 * Case-sensitive.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Receiving")
	TArray<FString> ControlAllowlist;

	/** Hold events and value changes until the mocap they were sent with is shown (recommended for cues). */
	UPROPERTY(Config, EditAnywhere, Category = "Timing")
	bool bAlignControlToMocap = true;

	/** Longest a change is held for alignment, in milliseconds, before it is delivered late (never dropped). */
	UPROPERTY(Config, EditAnywhere, Category = "Timing", meta = (EditCondition = "bAlignControlToMocap", ClampMin = "0", ClampMax = "5000"))
	int32 MaxAlignmentHoldMs = 500;

	/** Byte budget per sender for live control messages. The default is twice what a sender produces at most. */
	UPROPERTY(Config, EditAnywhere, Category = "Limits", AdvancedDisplay, meta = (ClampMin = "2048"))
	int32 MaxControlLiveBytesPerSecond = 64 * 1024;

	/** Byte budget per sender for snapshot parts. The default is twice what a sender produces at most. */
	UPROPERTY(Config, EditAnywhere, Category = "Limits", AdvancedDisplay, meta = (ClampMin = "2048"))
	int32 MaxControlSnapshotBytesPerSecond = 512 * 1024;

	/** Most values (plus pending clears) kept per sender. */
	UPROPERTY(Config, EditAnywhere, Category = "Limits", AdvancedDisplay, meta = (ClampMin = "1", ClampMax = "1024"))
	int32 MaxControlKeysPerSource = 1024;

	/**
	 * The effective setting for one receiver source: PerSource unless it is ProjectDefault, then
	 * the runtime override (FO3DControlBus::GetReceiveOverride) if set, then bAcceptControl.
	 * Game thread.
	 */
	static bool IsReceiveEnabled(EO3DControlAcceptMode PerSource);
};

/** Turns control receiving on or off from a client's own code, including in Shipping builds. */
UCLASS()
class OPEN3DRECEIVER_API UO3DControlLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Accept (or refuse) control messages in this process, overriding the project setting until
	 * ClearControlReceiveOverride. A receiver source set to Enabled or Disabled keeps its own
	 * setting. Takes effect from the next message; values arrive within one snapshot interval.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Control")
	static void SetControlReceiveEnabled(bool bEnabled);

	/** Return to the project setting. */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Control")
	static void ClearControlReceiveOverride();

	/** Whether receiver sources set to Project Default accept control right now. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static bool IsControlReceiveEnabled();
};
