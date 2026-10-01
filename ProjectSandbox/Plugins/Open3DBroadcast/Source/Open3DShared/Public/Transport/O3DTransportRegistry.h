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
 * and GetNumLiveInstances are safe on any thread (read lock). Register, unregister (resetting a
 * registration) and EditLegacyDescriptor are game-thread only and check() it: they broadcast
 * OnTransportsChanged and OnTransportUnregistering on the calling thread, and draining calls the
 * instances' Stop(), which is a game-thread call.
 *
 * Not yet here (later WP-A1 PRs): result types and capabilities (PR 3), and the typed options view
 * that replaces the component and source parameters of the configure functions (PR 5).
 */

class UO3DSenderComponent;
struct FO3DReceiverSourceConfig;
class FO3DTransportRegistration;

/**
 * Translates a sender component's settings into the transport config before Initialize. Called
 * on the game thread, outside the registry lock. The component type lives in Open3DSender and is
 * only forward-declared here; WP-A1 PR 5 replaces this parameter with FO3DTransportOptionsView.
 */
using FO3DSenderConfigureFunction = TFunction<void(const UO3DSenderComponent*, FO3DTransportConfig&)>;

/** Receiver counterpart of FO3DSenderConfigureFunction; FO3DReceiverSourceConfig lives in Open3DReceiver. */
using FO3DReceiverConfigureFunction = TFunction<void(const FO3DReceiverSourceConfig&, FO3DTransportConfig&)>;

/** The options one role of a transport reads from its namespaced option map. */
struct FO3DTransportRoleOptions
{
	/**
	 * Option keys whose values are credentials (ADR 0004). Never persisted, never logged; the
	 * component or source resolves them from FO3DSecretStore into FO3DTransportConfig::Secrets.
	 */
	TArray<FString> SecretOptionKeys;

	/**
	 * Optional environment variable per secret key, e.g. {"<transport>.token", "O3DB_<TRANSPORT>_TOKEN"}.
	 * A non-default credential profile first tries "<NAME>__<PROFILE>".
	 */
	TMap<FString, FString> SecretEnvVars;

	/**
	 * The declared options, as data (ADR 0010 §4). The Open3DBroadcastEditor module builds the
	 * Details and LiveLink panel rows from it. Every Secret entry's key must also be in
	 * SecretOptionKeys.
	 */
	FO3DTransportOptionSchema OptionSchema;

	bool IsEmpty() const
	{
		return SecretOptionKeys.Num() == 0 && SecretEnvVars.Num() == 0 && OptionSchema.Num() == 0;
	}
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

	/** Optional. Fills the config from the sender component before Initialize. */
	FO3DSenderConfigureFunction ConfigureSender;

	/** Optional. Fills the config from the receiver source settings before Initialize. */
	FO3DReceiverConfigureFunction ConfigureReceiver;

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
	 * Copies the secret declaration of Role for Name. Returns false, with empty outputs, when Name
	 * is not registered. Any thread.
	 */
	bool GetSecretDeclaration(FName Name, EO3DTransportRole Role, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars) const;

	/** Copies the option schema of Role for Name. Returns false, with an empty output, when Name is not registered. Any thread. */
	bool GetOptionSchema(FName Name, EO3DTransportRole Role, FO3DTransportOptionSchema& OutSchema) const;

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

	/**
	 * Support for the deprecated register functions (O3DTransport::RegisterSender,
	 * O3DSender::RegisterTransportCustomization and their receiver counterparts), which each set
	 * one part of a transport. Under the write lock, copies the legacy descriptor registered under
	 * Name (or starts an empty one), lets Edit change the copy, and publishes it; an edit that
	 * leaves no factory, configure function or declared option removes the entry, and drains it
	 * like an unregister. Instances created from the entry stay tracked across edits. Refused,
	 * with a Warning, when Name belongs to a Register() registration. Edit must not call the
	 * registry. Removed with the shims in the next minor release. Game thread.
	 */
	void EditLegacyDescriptor(FName Name, TFunctionRef<void(FO3DTransportDescriptor&)> Edit);

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
		/** Instances created from this entry; carried over when a legacy edit replaces the descriptor. */
		FLiveListPtr Live;
		/** Identifies the Register() call that created the entry; 0 for a legacy entry. */
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
