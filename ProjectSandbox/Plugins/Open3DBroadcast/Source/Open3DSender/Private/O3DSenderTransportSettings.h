// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSecretStore.h"
#include "Templates/Function.h"
#include "Transport/O3DTransportOptionSet.h"

/**
 * The sender's transport options and credentials (WP-A3 step 8, SND-22, ADR 0004): option reads
 * and writes that route declared secret keys to the secret store, the credential profile, migrating
 * secrets out of old saved data, switching options between transports (SND-35), the options and
 * secrets a transport config gets, and which property edits restart capture. The maps stay
 * properties of the sender component, which passes them in, with a callback that records the
 * change for undo (Modify) before a map changes. Game thread only.
 */
class FO3DSenderTransportSettings
{
public:
	using FOptions = TMap<FString, FString>;

	/** Whether the transport's schema declares Key a secret. */
	static bool IsSecretKey(FName Transport, const FString& Key);
	/** The option's value; empty for an unknown key and always for a secret (ADR 0004 item 4). */
	static FString GetOption(const FOptions& Options, FName Transport, const FString& Key);
	/** Sets (empty: removes) an option; a secret key goes to the session store without Modify. */
	static void SetOption(FOptions& Options, FName Transport, const FString& Key, const FString& Value, TFunctionRef<void()> Modify);

	/** The credential profile the options name for the transport, normalized. */
	static FString GetCredentialProfile(const FOptions& Options, FName Transport);
	/** Stores a secret and removes a copy older data left in the options. */
	static void SetSecret(FOptions& Options, FName Transport, const FString& Key, const FString& Value, EO3DSecretPersistence Persistence, TFunctionRef<void()> Modify);
	static bool SetSecretPersistence(const FOptions& Options, FName Transport, const FString& Key, EO3DSecretPersistence Persistence);
	static void ClearSecret(const FOptions& Options, FName Transport, const FString& Key);
	static FO3DSecretStatus GetSecretStatus(const FOptions& Options, FName Transport, const FString& Key);

	/**
	 * Moves declared secret keys out of the options into the session store (ADR 0004 item 4); a
	 * value already set this session wins. Returns the keys moved; the caller logs them.
	 */
	static TArray<FString> MigrateLegacySecrets(FOptions& Options, FName Transport);
	/** Puts From's options away in Inactive and restores To's (SND-35). The caller records the change first. */
	static void SwitchOptions(FOptions& Active, TMap<FName, FO3DTransportOptionSet>& Inactive, FName From, FName To);

	/** The options a transport config carries (secret keys left out) and its secrets, resolved from the store. */
	static void BuildConfigOptions(const FOptions& Options, FName Transport, FOptions& OutOptions, FOptions& OutSecrets);

	/** The options a transport config carries, secret keys left out; the secret store is not read. */
	static void BuildPublicOptions(const FOptions& Options, FName Transport, FOptions& OutOptions);

	/** A sender property whose edit stops capture and starts it again (editor). */
	static bool IsRestartProperty(FName Property);
};
