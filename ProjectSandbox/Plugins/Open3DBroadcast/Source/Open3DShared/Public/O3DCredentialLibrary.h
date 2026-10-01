// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "O3DCredentialLibrary.generated.h"

/**
 * Write-only runtime access to transport credentials (ADR 0004 item 8), for packaged games and
 * Blueprints. Values go to the in-process session store (FO3DSecretStore) and are never written
 * to disk. There is deliberately no getter.
 *
 * A transport reads a secret only for the option keys its customization declares as secret, and
 * only for the credential profile its "<transport>.credentialProfile" option selects ("default"
 * when unset).
 */
UCLASS()
class OPEN3DSHARED_API UO3DCredentialLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Sets a transport secret for this session. An empty Value clears it.
	 * @param TransportName Registered transport name, as selected on the sender or receiver.
	 * @param Profile Credential profile; empty means "default".
	 * @param Key Secret option key declared by the transport.
	 * @param Value The secret. Not stored on disk and not readable back.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Credentials")
	static void SetTransportSecret(FName TransportName, const FString& Profile, const FString& Key, const FString& Value);

	/** Clears a transport secret (session value and any copy remembered on this machine). An environment variable still applies. */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Credentials")
	static void ClearTransportSecret(FName TransportName, const FString& Profile, const FString& Key);
};
