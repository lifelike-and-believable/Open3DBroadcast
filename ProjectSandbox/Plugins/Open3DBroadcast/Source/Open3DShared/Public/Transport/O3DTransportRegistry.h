// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Delegates/DelegateCombinations.h"
#include "HAL/CriticalSection.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportApiVersion.h"
#include "Transport/O3DTransportOptionsView.h"
#include "Transport/O3DTransportTypes.h"

/*
 * The one transport registry (ADR 0007 item 4, WP-A1 PR 1; SHR-12, SND-23, RCV-27, RCV-28).
 *
 * A transport registers ONE immutable FO3DTransportDescriptor per transport name: its sender and
 * receiver factories, the functions that turn component or source settings into an
 * FO3DTransportConfig, and the declared options (secret keys and the option schema the editor
 * builds its panels from). It gets an FO3DTransportRegistration back, whose destructor unregisters.
 *
 * Pickers (GetNames) and instance creation (CreateSender, CreateReceiver) read the same entries,
 * so a transport listed for a role can always be created for that role, and nothing else is
 * listed. Lookups hand out shared pointers to the immutable descriptor, never pointers into the
 * registry's map, so a descriptor stays valid for as long as the caller holds it, also after the
 * transport unregisters. Factories and configure functions are always called outside the lock.
 *
 * Lifetime (ADR 0007 item 5, WP-A1 PR 2; SHR-13, TRF-14): CreateSender and CreateReceiver keep a
 * weak reference to every instance they hand out, in a live list per transport name. Unregistering
 * a transport drains it: the name leaves the registry (no new instances), OnTransportUnregistering
 * is broadcast so the owners (the sender transport controller, the receiver source) stop and drop
 * their instances, then the registry stops whatever is still referenced and, if anything still
 * is, logs one Error naming the transport and the count. GetNumLiveInstances counts what is
 * still referenced, also after the unregister, so FO3DFfiLibrary can refuse to free a DLL whose
 * code a leaked instance still runs.
 * A transport module's ShutdownModule therefore resets its registration first and frees its FFI
 * handles after.
 *
 * Threading: Find, GetNames, GetSecretDeclaration, GetOptionSchema, CreateSender, CreateReceiver
 * and GetNumLiveInstances are safe on any thread (read lock). Register and unregister (resetting a
 * registration) are game-thread only and check() it: they broadcast
 * OnTransportsChanged and OnTransportUnregistering on the calling thread, and draining calls the
 * instances' Stop(), which is a game-thread call.
 *
 * Capabilities (ADR 0007 item 4, WP-A1 PR 3): a descriptor's GetCapabilities reports what the
 * transport can do with a config (delivery guarantee, audio, control, payload limit); GetCapabilities
 * on the registry calls it outside the lock.
 *
 * Configure functions (ADR 0007 item 4, WP-A1 PR 5a): both roles take the role's options as an
 * FO3DTransportOptionsView, not the sender component or the receiver source settings, so a
 * transport module needs neither Open3DSender nor Open3DReceiver.
 */

class FO3DTransportRegistration;

/**
 * Fills the transport config from the role's options before Initialize (WP-A1 PR 5a). Called on
 * the game thread, outside the registry lock, after the host has filled Transport, Role,
 * SubjectName (sender), Audio, Secrets, AdvancedParams (a copy of the options) and OptionSchema.
 * Options views the host's own copy of the option values with the role's schema; it stays valid
 * while the function runs, whatever the function adds to Config.AdvancedParams.
 */
using FO3DTransportConfigureFunction = TFunction<void(const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& /*Config*/)>;

/** The sender's configure function; same signature as the receiver's since WP-A1 PR 5a. */
using FO3DSenderConfigureFunction = FO3DTransportConfigureFunction;

/** The receiver's configure function; same signature as the sender's since WP-A1 PR 5a. */
using FO3DReceiverConfigureFunction = FO3DTransportConfigureFunction;

/** Capability query of a descriptor (ADR 0007 item 4). Any thread, outside the registry lock. */
using FO3DCapabilitiesFunction = TFunction<FO3DTransportCapabilities(const FO3DTransportConfig&)>;

/**
 * The options one role of a transport reads from its namespaced option map.
 *
 * Secrets (ADR 0004): a key is secret when the schema has a Secret entry for it, and its
 * environment variable is the entry's SecretEnvVar (ADR 0007 item 8). GetSecretDeclaration lists
 * them; it is what the hosts, the editor and the secret store use. A secret key is never
 * persisted, never logged, and reaches the transport only through FO3DTransportConfig::Secrets.
 */
struct FO3DTransportRoleOptions
{
	/**
	 * The declared options, as data (ADR 0010 §4). The Open3DBroadcastEditor module builds the
	 * Details and LiveLink panel rows from it. Its Secret entries declare the role's secrets.
	 */
	FO3DTransportOptionSchema OptionSchema;

	bool IsEmpty() const
	{
		return OptionSchema.Num() == 0;
	}

	/**
	 * The secret keys and their environment variables: the schema's Secret entries, in schema
	 * order, each key once (compared case-insensitively, as the option maps do), with its
	 * SecretEnvVar when it names one.
	 */
	OPEN3DSHARED_API void GetSecretDeclaration(TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars) const;
};

/**
 * Everything the host needs to know about one transport. Immutable once registered: the registry
 * only ever hands out TSharedPtr<const FO3DTransportDescriptor>.
 */
struct FO3DTransportDescriptor
{
	/** Registered transport name, e.g. "TCP" or "WebRTC". Pickers show it; assets store it. */
	FName Name;

	/** Optional human-readable name. Empty means "use Name". */
	FText DisplayName;

	/**
	 * O3D_TRANSPORT_API_VERSION as the code that filled this descriptor was compiled. The default
	 * initializer is compiled into the registering module, so an add-on built against another
	 * version is refused by Register (the second check after the add-on's own StartupModule check).
	 */
	int32 ApiVersion = O3D_TRANSPORT_API_VERSION;

	/** Module that registered the descriptor, for logs. Optional. */
	FName OwningModule;

	/** Sender factory. Optional; without it the transport is not listed for the Sender role. */
	FO3DSenderFactory CreateSender;

	/** Receiver factory. Optional; without it the transport is not listed for the Receiver role. */
	FO3DReceiverFactory CreateReceiver;

	/** Optional. Fills the config from the sender's options before Initialize. */
	FO3DSenderConfigureFunction ConfigureSender;

	/** Optional. Fills the config from the receiver's options before Initialize. */
	FO3DReceiverConfigureFunction ConfigureReceiver;

	/**
	 * What the transport can do with a config (ADR 0007 item 4, WP-A1 PR 3), e.g. the delivery
	 * guarantee of the NNG mode or WebRTC channel the config selects. Must return the values its
	 * sender's and receiver's GetCapabilities() report for the same config. Called outside the
	 * registry lock, on any thread; must not block. Optional: without it the registry reports
	 * only bSend and bReceive (from the factories) and Delivery Unknown.
	 */
	FO3DCapabilitiesFunction GetCapabilities;

	/** Declared sender options. */
	FO3DTransportRoleOptions SenderOptions;

	/** Declared receiver options. */
	FO3DTransportRoleOptions ReceiverOptions;

	/** True when the descriptor has a factory for Role. */
	bool HasRole(EO3DTransportRole Role) const
	{
		return Role == EO3DTransportRole::Sender ? static_cast<bool>(CreateSender) : static_cast<bool>(CreateReceiver);
	}

	/** The declared options of Role. */
	const FO3DTransportRoleOptions& GetRoleOptions(EO3DTransportRole Role) const
	{
		return Role == EO3DTransportRole::Sender ? SenderOptions : ReceiverOptions;
	}

	/** DisplayName, or Name when DisplayName is empty. */
	FText GetDisplayName() const
	{
		return DisplayName.IsEmpty() ? FText::FromName(Name) : DisplayName;
	}
};

/**
 * Broadcast with the transport name while that transport unregisters (ADR 0007 item 5). Game
 * thread. Owners of an instance of that transport stop it and drop every reference to it, and to
 * the sinks they gave it, before returning. Handlers may call the registry (Find, Create*), but
 * the name is already gone, so no new instance of it can be created.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FO3DTransportUnregisteringDelegate, FName /*TransportName*/);

using FO3DTransportDescriptorPtr = TSharedPtr<const FO3DTransportDescriptor, ESPMode::ThreadSafe>;
using FO3DTransportDescriptorRef = TSharedRef<const FO3DTransportDescriptor, ESPMode::ThreadSafe>;

/**
 * The registry. FO3DTransportRegistry::Get() is the process-wide instance every module uses. Tests
 * may create their own with MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>(); a registry must
 * always be owned by a shared pointer, because registrations refer back to it weakly.
 */
class OPEN3DSHARED_API FO3DTransportRegistry : public TSharedFromThis<FO3DTransportRegistry, ESPMode::ThreadSafe>
{
public:
	/** The process-wide registry. Created on first use; any thread. */
	static FO3DTransportRegistry& Get();

	FO3DTransportRegistry();
	~FO3DTransportRegistry();

	FO3DTransportRegistry(const FO3DTransportRegistry&) = delete;
	FO3DTransportRegistry& operator=(const FO3DTransportRegistry&) = delete;

	/**
	 * Registers Descriptor under Descriptor->Name. Refused, with a log line and an invalid
	 * registration, when the name is None, the descriptor has neither factory, its ApiVersion is
	 * not this build's O3D_TRANSPORT_API_VERSION, or the name is already registered (the first
	 * registration keeps the name; nothing is overridden). Game thread.
	 */
	[[nodiscard]] FO3DTransportRegistration Register(FO3DTransportDescriptorRef Descriptor);

	/** Convenience overload that takes the descriptor by value. */
	[[nodiscard]] FO3DTransportRegistration Register(FO3DTransportDescriptor&& Descriptor);

	/** The descriptor registered under Name, or null. The result stays valid after an unregister. Any thread. */
	FO3DTransportDescriptorPtr Find(FName Name) const;

	/** True when Name is registered with a factory for Role. Any thread. */
	bool IsRegistered(FName Name, EO3DTransportRole Role) const;

	/**
	 * Names that have a factory for Role, sorted lexically. This is the list pickers show and
	 * exactly the set CreateSender or CreateReceiver can instantiate (RCV-28). Any thread.
	 */
	TArray<FName> GetNames(EO3DTransportRole Role) const;

	/**
	 * Copies the secret declaration of Role for Name (FO3DTransportRoleOptions::
	 * GetSecretDeclaration: the schema's Secret entries and their environment variables). Returns
	 * false, with empty outputs, when Name is not registered. Any thread.
	 */
	bool GetSecretDeclaration(FName Name, EO3DTransportRole Role, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars) const;

	/** Copies the option schema of Role for Name. Returns false, with an empty output, when Name is not registered. Any thread. */
	bool GetOptionSchema(FName Name, EO3DTransportRole Role, FO3DTransportOptionSchema& OutSchema) const;

	/**
	 * The capabilities of Name for Config (FO3DTransportDescriptor::GetCapabilities, called
	 * outside the lock). bSend and bReceive always reflect the registered factories. Returns
	 * false, with default (empty) capabilities, when Name is not registered. Any thread.
	 */
	bool GetCapabilities(FName Name, const FO3DTransportConfig& Config, FO3DTransportCapabilities& OutCapabilities) const;

	/**
	 * A new sender from Name's factory (called outside the lock), or null with a Warning. The
	 * registry keeps a weak reference to it in Name's live list. If Name unregisters while the
	 * factory runs, the new instance is stopped and null is returned.
	 */
	TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> CreateSender(FName Name) const;

	/** Receiver counterpart of CreateSender, with the same tracking. */
	TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> CreateReceiver(FName Name) const;

	/**
	 * Instances of Name created through CreateSender or CreateReceiver that are still referenced
	 * somewhere, including those left over from an earlier registration of Name that were still
	 * referenced when it unregistered (leaks). Any thread.
	 */
	int32 GetNumLiveInstances(FName Name) const;

	/**
	 * Broadcast after every register and unregister, on the thread that made the change (the game
	 * thread by contract), outside the lock, so pickers and caches can refresh.
	 */
	FSimpleMulticastDelegate& OnTransportsChanged() { return TransportsChanged; }

	/**
	 * Broadcast while a transport unregisters, after its name has left the registry and before the
	 * registry stops what is still alive (see FO3DTransportUnregisteringDelegate). Owners of
	 * instances subscribe; the subscription must be removed before the subscriber is destroyed.
	 * Game thread.
	 */
	FO3DTransportUnregisteringDelegate& OnTransportUnregistering() { return TransportUnregistering; }

	/** Number of registered names. Any thread. */
	int32 Num() const;

private:
	friend class FO3DTransportRegistration;

	/** Weak references to the instances created from one entry. Defined in the .cpp. */
	struct FLiveList;
	using FLiveListPtr = TSharedPtr<FLiveList, ESPMode::ThreadSafe>;

	struct FEntry
	{
		FO3DTransportDescriptorPtr Descriptor;
		/** Instances created from this entry. */
		FLiveListPtr Live;
		/** Identifies the Register() call that created the entry. */
		uint64 RegistrationId = 0;
	};

	/** Removes Name when it is still owned by RegistrationId, then drains it. Called by FO3DTransportRegistration. */
	void Unregister(FName Name, uint64 RegistrationId);

	/** Pins the descriptor and live list registered under Name (read lock). */
	bool FindEntry(FName Name, FO3DTransportDescriptorPtr& OutDescriptor, FLiveListPtr& OutLive) const;

	/**
	 * Drain after Name left the map: closes Live, broadcasts OnTransportUnregistering, stops what is
	 * still alive and logs one Error if any instance is still referenced; those stay counted by
	 * GetNumLiveInstances. Game thread, no lock held.
	 */
	void Drain(FName Name, const FO3DTransportDescriptorPtr& Removed, const FLiveListPtr& Live);

	mutable FRWLock Lock;
	TMap<FName, FEntry> Entries;
	/**
	 * Live lists of unregistered entries that still had referenced instances (leaks), so
	 * GetNumLiveInstances keeps counting them. Lists with nothing alive are dropped on the next drain.
	 */
	TArray<TPair<FName, FLiveListPtr>> Retired;
	uint64 NextRegistrationId = 1;
	FSimpleMulticastDelegate TransportsChanged;
	FO3DTransportUnregisteringDelegate TransportUnregistering;
};

/**
 * Move-only handle for one Register() call. Destroying or resetting it unregisters that
 * descriptor, and only that one: a later registration under the same name is left alone. Holds
 * the registry weakly, so it is safe to outlive it. Keep it in the transport's module object and
 * reset it in ShutdownModule.
 */
class OPEN3DSHARED_API FO3DTransportRegistration
{
public:
	FO3DTransportRegistration();
	~FO3DTransportRegistration();

	FO3DTransportRegistration(FO3DTransportRegistration&& Other);
	FO3DTransportRegistration& operator=(FO3DTransportRegistration&& Other);

	FO3DTransportRegistration(const FO3DTransportRegistration&) = delete;
	FO3DTransportRegistration& operator=(const FO3DTransportRegistration&) = delete;

	/** True while this handle owns a registration. */
	bool IsValid() const;

	/** The registered name, or None. */
	FName GetName() const { return Name; }

	/** Unregisters now. Idempotent. */
	void Reset();

private:
	friend class FO3DTransportRegistry;

	FO3DTransportRegistration(const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe>& InRegistry, FName InName, uint64 InRegistrationId);

	TWeakPtr<FO3DTransportRegistry, ESPMode::ThreadSafe> Registry;
	FName Name;
	uint64 RegistrationId = 0;
};
