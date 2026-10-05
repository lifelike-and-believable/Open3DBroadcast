// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DTransportOptionSet.generated.h"

/**
 * The saved options of one transport that is not the selected one (ADR 0007 item 8, WP-A1 PR 5a;
 * SND-35). The sender component and the receiver source settings keep one per transport the user
 * has configured and switched away from, so switching back restores them instead of starting from
 * an empty map. Never holds a secret: the transport's declared secret keys are dropped when the
 * options are put away (ADR 0004 item 4).
 */
USTRUCT()
struct OPEN3DSHARED_API FO3DTransportOptionSet
{
	GENERATED_BODY()

	/** The option map exactly as it was when the transport was deselected, secrets excluded. */
	UPROPERTY()
	TMap<FString, FString> Options;
};

namespace O3DTransportOptions
{
	/**
	 * Switches the selected transport's options from From to To (SND-35).
	 *
	 * Active (the selected transport's map) is stored in Inactive under From, without the keys in
	 * FromSecretKeys and only when it is not empty; Active then becomes what Inactive held for To,
	 * which is removed from Inactive, or an empty map. Nothing happens when From equals To.
	 *
	 * bFromRegistered says whether From is a registered transport, so FromSecretKeys is its real
	 * secret declaration. When it is false (for example its plugin is not loaded), which keys are
	 * secrets is unknown, so From's options are dropped instead of put away, as before SND-35:
	 * a credential must never reach Inactive, which is saved with the asset (ADR 0004).
	 *
	 * Option keys are not renamed. TCP, UDP and NNG all read "host" and "port", so the options of
	 * two transports cannot share one map; keeping one map per transport keeps every documented
	 * key, saved asset and Blueprint call as it is.
	 *
	 * Callers record the change for undo first (Modify, or a transaction), because both maps change.
	 * Game thread.
	 */
	OPEN3DSHARED_API void SwitchTransportOptions(TMap<FString, FString>& Active, TMap<FName, FO3DTransportOptionSet>& Inactive,
		FName From, FName To, const TArray<FString>& FromSecretKeys, bool bFromRegistered);
}
