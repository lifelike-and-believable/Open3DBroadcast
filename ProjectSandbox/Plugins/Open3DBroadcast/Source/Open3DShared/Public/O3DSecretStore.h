// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

/**
 * Credentials for transports (ADR 0004).
 *
 * A transport declares which of its option keys are secret, and optionally an environment
 * variable per key, with a Secret entry in its option schema and the entry's SecretEnvVar
 * (FO3DTransportRegistry::GetSecretDeclaration lists them). The value of a secret key is never
 * written to TransportOptions, an asset, an ini file or a LiveLink connection string. It lives in
 * FO3DSecretStore, keyed by (transport, credential profile, option key). The credential profile
 * is an ordinary persisted option, "<transport>.credentialProfile" (default "default"), so an asset
 * names the secret it needs without containing it.
 *
 * Resolution order when a transport starts:
 *   1. Session (R1): the value set in this process. Lost on restart.
 *   2. Environment (R2): the declared variable with the profile suffix
 *      (e.g. O3DB_EXAMPLE_TOKEN__STAGE for profile "stage"), then the plain name.
 *   3. User settings (R3): the per-user EditorPerProjectUserSettings file under Saved/, only in
 *      editor builds and only for values the user chose to remember on this machine.
 *
 * No API returns more than one value, and none lists values.
 */

/** Where Set() keeps a value. There is deliberately no option to keep it in an asset or project ini. */
enum class EO3DSecretPersistence : uint8
{
	/** In this process only (R1). */
	Session,
	/** In this process and in the per-user editor settings file (R3). Editor builds only; acts as Session elsewhere. */
	RememberOnThisMachine,
};

/** Where a resolved secret came from. */
enum class EO3DSecretSource : uint8
{
	None,
	Session,
	Environment,
	UserSettings,
};

/** A resolved secret value and its source. Do not log Value. */
struct FO3DResolvedSecret
{
	FString Value;
	EO3DSecretSource Source = EO3DSecretSource::None;
	/** The environment variable the value came from, when Source is Environment. */
	FString EnvVarName;
};

/** Where a secret would resolve from, without the value (for status lines in the UI). */
struct FO3DSecretStatus
{
	EO3DSecretSource Source = EO3DSecretSource::None;
	/** The environment variable the value comes from, when Source is Environment. */
	FString EnvVarName;
	/** True when a remembered (R3) copy exists, even if a session or environment value wins. */
	bool bRemembered = false;
};

/**
 * Backing store for remembered secrets (R3). The editor implementation writes the per-user
 * EditorPerProjectUserSettings file; tests pass a fake. Keys are opaque composite strings.
 * Called on the game thread only.
 */
class IO3DSecretUserStore
{
public:
	virtual ~IO3DSecretUserStore() = default;
	virtual bool Load(const FString& CompositeKey, FString& OutValue) const = 0;
	virtual void Save(const FString& CompositeKey, const FString& Value) = 0;
	virtual void Remove(const FString& CompositeKey) = 0;
};

/**
 * Thread-safe secret store. Reads happen when a transport starts, not per frame.
 * The user-settings store (R3) is only consulted on the game thread.
 */
class OPEN3DSHARED_API FO3DSecretStore
{
public:
	/** Returns an environment variable's value, or an empty string when it is unset. */
	using FEnvironmentReader = TFunction<FString(const FString& /*Name*/)>;

	/**
	 * @param InEnvironmentReader Null reads the process environment (FPlatformMisc).
	 * @param InUserStore Null disables R3 (what a packaged game gets).
	 */
	explicit FO3DSecretStore(FEnvironmentReader InEnvironmentReader = nullptr, TSharedPtr<IO3DSecretUserStore> InUserStore = nullptr);
	~FO3DSecretStore();

	FO3DSecretStore(const FO3DSecretStore&) = delete;
	FO3DSecretStore& operator=(const FO3DSecretStore&) = delete;

	/** The process-wide store. Editor builds get the per-user settings store (R3); other builds do not. */
	static FO3DSecretStore& Get();

	/**
	 * Stores Value for (Transport, Profile, Key). An empty Value is the same as Clear().
	 * RememberOnThisMachine also writes the remembered (R3) copy. Session leaves any remembered
	 * copy alone; call SetPersistence(..., Session) to drop it.
	 */
	void Set(const FString& Transport, const FString& Profile, const FString& Key, const FString& Value,
		EO3DSecretPersistence Persistence = EO3DSecretPersistence::Session);

	/**
	 * Moves the current session value between Session and RememberOnThisMachine without the
	 * caller seeing it. Returns false when there is no session value to move (switching to
	 * Session still removes a remembered copy).
	 */
	bool SetPersistence(const FString& Transport, const FString& Profile, const FString& Key, EO3DSecretPersistence Persistence);

	/** Removes the session value and any remembered copy. An environment variable still applies. */
	void Clear(const FString& Transport, const FString& Profile, const FString& Key);

	/** True when a session value exists (R1 only). */
	bool HasSessionValue(const FString& Transport, const FString& Profile, const FString& Key) const;

	/**
	 * Resolves (Transport, Profile, Key): session, then environment (EnvVarBaseName with the
	 * profile suffix, then without), then remembered settings. EnvVarBaseName may be empty.
	 */
	TOptional<FO3DResolvedSecret> Resolve(const FString& Transport, const FString& Profile, const FString& Key,
		const FString& EnvVarBaseName = FString()) const;

	/** Same order as Resolve(), without the value. */
	FO3DSecretStatus Describe(const FString& Transport, const FString& Profile, const FString& Key,
		const FString& EnvVarBaseName = FString()) const;

	/**
	 * Resolves every key in SecretKeys into OutSecrets (key -> value). Keys that do not resolve
	 * are left out. EnvVarsByKey maps an option key to its environment variable base name.
	 */
	void ResolveAll(const FString& Transport, const FString& Profile, const TArray<FString>& SecretKeys,
		const TMap<FString, FString>& EnvVarsByKey, TMap<FString, FString>& OutSecrets) const;

	/** True when this store has a user-settings (R3) backend. False in packaged games. */
	bool SupportsRememberOnThisMachine() const { return UserStore.IsValid(); }

	/** "default" for an empty or blank profile, otherwise the trimmed profile. */
	static FString NormalizeProfile(const FString& Profile);

	/**
	 * Environment variable names tried for a profile, in order: "<Base>__<PROFILE>" for a
	 * non-default profile (upper case, characters other than A-Z, 0-9 and '_' become '_'),
	 * then "<Base>". Empty when Base is empty.
	 */
	static TArray<FString> GetEnvVarCandidates(const FString& EnvVarBaseName, const FString& Profile);

	/** Name of the persisted, non-secret option that selects the credential profile: "<transport>.credentialProfile" in lower case transport. */
	static FString MakeCredentialProfileOptionKey(const FString& Transport);

	/** Default credential profile name. */
	static const TCHAR* DefaultProfile() { return TEXT("default"); }

private:
	static FString MakeCompositeKey(const FString& Transport, const FString& Profile, const FString& Key);
	bool CanUseUserStore() const;
	bool LoadRemembered(const FString& CompositeKey, FString& OutValue) const;
	bool ResolveEnvironment(const FString& EnvVarBaseName, const FString& Profile, FString& OutValue, FString& OutName) const;

	mutable FCriticalSection Mutex;
	TMap<FString, FString> SessionValues;
	FEnvironmentReader EnvironmentReader;
	TSharedPtr<IO3DSecretUserStore> UserStore;
};
