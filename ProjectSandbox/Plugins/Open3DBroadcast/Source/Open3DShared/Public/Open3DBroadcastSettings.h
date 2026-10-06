// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportTypes.h"
#include "Open3DBroadcastSettings.generated.h"

/** Option values for one transport, by option key (the keys its Transport Options panel uses). */
USTRUCT()
struct OPEN3DSHARED_API FO3DTransportDefaultOptions
{
	GENERATED_BODY()

	/** Option key and value, for example "port" and "17700", or "webrtc.tokenEndpointUrl". Secret options are ignored here. */
	UPROPERTY(Config, EditAnywhere, Category = "Open3DBroadcast")
	TMap<FString, FString> Options;
};

/**
 * Project-wide defaults for transport options (WP-U1; UX-2). Project Settings > Plugins >
 * Open3DBroadcast; saved to the project's Config/DefaultGame.ini, which is staged into packaged
 * builds. A sender component or receiver source uses these for every option it leaves empty: its
 * own value wins, then the project default here, then the transport's built-in default. Server
 * and relay URLs, ports and the WebRTC token endpoint (webrtc.tokenEndpointUrl) can be set once
 * here instead of in every component and LiveLink source.
 *
 * Never holds a secret (ADR 0004): DefaultGame.ini is committed and shipped, so options a
 * transport declares Secret are ignored here, with a warning. Tokens come from the secret store
 * or environment variables.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Open3DBroadcast"))
class OPEN3DSHARED_API UOpen3DBroadcastSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOpen3DBroadcastSettings();

	/** Default options for sender components, by transport name (for example "udp" or "webrtc"). */
	UPROPERTY(Config, EditAnywhere, Category = "Transport Defaults")
	TMap<FName, FO3DTransportDefaultOptions> SenderDefaults;

	/** Default options for receiver sources (LiveLink), by transport name. */
	UPROPERTY(Config, EditAnywhere, Category = "Transport Defaults")
	TMap<FName, FO3DTransportDefaultOptions> ReceiverDefaults;

	/**
	 * Fills every option InOutOptions leaves unset (absent, empty or whitespace) with this project's
	 * default for Transport and Role. Options the transport declares Secret are never filled. Does
	 * nothing for a transport that is not registered, because its secret options are unknown.
	 */
	static void ApplyTransportDefaults(FName Transport, EO3DTransportRole Role, TMap<FString, FString>& InOutOptions);

	/**
	 * Sets each non-secret field's Default in InOutSchema to this project's default, so an options
	 * panel shows the value that applies when the field is left empty.
	 */
	static void ApplyToSchemaDefaults(FName Transport, EO3DTransportRole Role, FO3DTransportOptionSchema& InOutSchema);
};
