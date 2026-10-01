// Copyright Lifelike & Believable. All Rights Reserved.

// This file also includes (through the registry header) the OPEN3DSHARED_API interface classes
// IOpen3DSender, IOpen3DReceiver, their audio sinks and ISerializedFrameConsumer, which have only
// inline members; including them in an Open3DShared translation unit is what emits their exported
// virtual function tables for the modules that import them.
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"
#include "O3DSharedLogs.h"

namespace O3DTransportRegistryPrivate
{
	/** A legacy descriptor with nothing left in it is removed rather than kept as an empty entry. */
	static bool IsEmptyLegacyDescriptor(const FO3DTransportDescriptor& Descriptor)
	{
		return !Descriptor.CreateSender
			&& !Descriptor.CreateReceiver
			&& !Descriptor.ConfigureSender
			&& !Descriptor.ConfigureReceiver
			&& Descriptor.SenderOptions.IsEmpty()
			&& Descriptor.ReceiverOptions.IsEmpty();
	}

	static const TCHAR* RoleName(EO3DTransportRole Role)
	{
		return Role == EO3DTransportRole::Sender ? TEXT("sender") : TEXT("receiver");
	}
}

// ── FO3DTransportRegistry::FLiveList ─────────────────────────────────────────────────────

/**
 * Weak references to the instances created from one registry entry (ADR 0007 item 5). Shared
 * between the entry and every CreateSender/CreateReceiver call that pinned it, so a factory that
 * is still running when the entry leaves the registry finds the list closed instead of adding an
 * instance nobody will drain. Its own lock, never held while calling into an instance.
 */
struct FO3DTransportRegistry::FLiveList
{
	using FSenderRef = TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>;
	using FReceiverRef = TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>;

	/** Adds Instance unless the list is closed. Returns false when closed. Any thread. */
	bool TryAddSender(const FSenderRef& Instance)
	{
		FScopeLock ScopeLock(&Mutex);
		if (bClosed)
		{
			return false;
		}
		PruneLocked();
		Senders.Add(Instance);
		return true;
	}

	bool TryAddReceiver(const FReceiverRef& Instance)
	{
		FScopeLock ScopeLock(&Mutex);
		if (bClosed)
		{
			return false;
		}
		PruneLocked();
		Receivers.Add(Instance);
		return true;
	}

	/** No instance is added after this. */
	void Close()
	{
		FScopeLock ScopeLock(&Mutex);
		bClosed = true;
	}

	/** Strong references to every instance that is still alive. */
	void PinAlive(TArray<FSenderRef>& OutSenders, TArray<FReceiverRef>& OutReceivers)
	{
		FScopeLock ScopeLock(&Mutex);
		PruneLocked();
		for (const TWeakPtr<IOpen3DSender, ESPMode::ThreadSafe>& Weak : Senders)
		{
			if (FSenderRef Pinned = Weak.Pin())
			{
				OutSenders.Add(MoveTemp(Pinned));
			}
		}
		for (const TWeakPtr<IOpen3DReceiver, ESPMode::ThreadSafe>& Weak : Receivers)
		{
			if (FReceiverRef Pinned = Weak.Pin())
			{
				OutReceivers.Add(MoveTemp(Pinned));
			}
		}
	}

	/** Instances still referenced somewhere. */
	int32 CountAlive(int32* OutSenders = nullptr, int32* OutReceivers = nullptr)
	{
		FScopeLock ScopeLock(&Mutex);
		PruneLocked();
		if (OutSenders != nullptr)
		{
			*OutSenders = Senders.Num();
		}
		if (OutReceivers != nullptr)
		{
			*OutReceivers = Receivers.Num();
		}
		return Senders.Num() + Receivers.Num();
	}

private:
	/** Drops references whose instance is gone, so a long session does not grow the lists. Caller holds Mutex. */
	void PruneLocked()
	{
		Senders.RemoveAll([](const TWeakPtr<IOpen3DSender, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid(); });
		Receivers.RemoveAll([](const TWeakPtr<IOpen3DReceiver, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid(); });
	}

	FCriticalSection Mutex;
	bool bClosed = false;
	TArray<TWeakPtr<IOpen3DSender, ESPMode::ThreadSafe>> Senders;
	TArray<TWeakPtr<IOpen3DReceiver, ESPMode::ThreadSafe>> Receivers;
};

// ── FO3DTransportRegistry ────────────────────────────────────────────────────────────────

FO3DTransportRegistry& FO3DTransportRegistry::Get()
{
	// Function-local static: created on first use by any module, thread-safe initialization. The
	// registrations refer back to it weakly, so it must be owned by a shared reference.
	static const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe> Instance = MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	return Instance.Get();
}

FO3DTransportRegistry::FO3DTransportRegistry() = default;

FO3DTransportRegistry::~FO3DTransportRegistry() = default;

FO3DTransportRegistration FO3DTransportRegistry::Register(FO3DTransportDescriptor&& Descriptor)
{
	return Register(FO3DTransportDescriptorRef(MakeShared<FO3DTransportDescriptor, ESPMode::ThreadSafe>(MoveTemp(Descriptor))));
}

FO3DTransportRegistration FO3DTransportRegistry::Register(FO3DTransportDescriptorRef Descriptor)
{
	// Game thread only (ADR 0007 item 4): OnTransportsChanged is broadcast on this thread.
	check(IsInGameThread());

	const FName Name = Descriptor->Name;
	if (Name.IsNone())
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Transport registration refused: the descriptor has no name."));
		return FO3DTransportRegistration();
	}

	if (Descriptor->ApiVersion != O3D_TRANSPORT_API_VERSION)
	{
		UE_LOG(LogO3DShared, Error,
			TEXT("Transport '%s' registration refused: it was built for transport API version %d, this Open3DBroadcast provides version %d."),
			*Name.ToString(), Descriptor->ApiVersion, O3D_TRANSPORT_API_VERSION);
		return FO3DTransportRegistration();
	}

	if (!Descriptor->CreateSender && !Descriptor->CreateReceiver)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Transport '%s' registration refused: the descriptor has neither a sender nor a receiver factory."), *Name.ToString());
		return FO3DTransportRegistration();
	}

	uint64 RegistrationId = 0;
	{
		FWriteScopeLock WriteLock(Lock);
		if (Entries.Contains(Name))
		{
			// Deterministic: the first registration keeps the name. Two modules fighting over one
			// name is a packaging error, not something to resolve by load order.
			RegistrationId = 0;
		}
		else
		{
			RegistrationId = NextRegistrationId++;
			FEntry& Entry = Entries.Add(Name);
			Entry.Descriptor = Descriptor;
			Entry.Live = MakeShared<FLiveList, ESPMode::ThreadSafe>();
			Entry.RegistrationId = RegistrationId;
		}
	}

	if (RegistrationId == 0)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Transport '%s' registration refused: a transport with that name is already registered."), *Name.ToString());
		return FO3DTransportRegistration();
	}

	UE_LOG(LogO3DShared, Verbose, TEXT("Transport '%s' registered (sender=%d receiver=%d, module %s)."),
		*Name.ToString(), Descriptor->CreateSender ? 1 : 0, Descriptor->CreateReceiver ? 1 : 0, *Descriptor->OwningModule.ToString());
	TransportsChanged.Broadcast();
	return FO3DTransportRegistration(AsShared(), Name, RegistrationId);
}

void FO3DTransportRegistry::Unregister(FName Name, uint64 RegistrationId)
{
	// Game thread only (ADR 0007 items 4 and 5): the drain broadcasts and calls Stop() here.
	check(IsInGameThread());

	// Destroyed outside the lock, after the drain: the descriptor's functions live in the
	// transport's module.
	FO3DTransportDescriptorPtr Removed;
	FLiveListPtr Live;
	{
		FWriteScopeLock WriteLock(Lock);
		const FEntry* Entry = Entries.Find(Name);
		if (Entry == nullptr || Entry->RegistrationId != RegistrationId)
		{
			return;
		}
		Removed = Entry->Descriptor;
		Live = Entry->Live;
		Entries.Remove(Name);
	}

	Drain(Name, Removed, Live);

	UE_LOG(LogO3DShared, Verbose, TEXT("Transport '%s' unregistered."), *Name.ToString());
	TransportsChanged.Broadcast();
}

void FO3DTransportRegistry::Drain(FName Name, const FO3DTransportDescriptorPtr& Removed, const FLiveListPtr& Live)
{
	// 1. The name is already out of the map, so no new CreateSender/CreateReceiver can find it;
	//    closing the list also turns away a factory call that pinned the entry just before.
	if (Live.IsValid())
	{
		Live->Close();
	}

	// 2. Owners stop and release their instances (sender transport controller, receiver source).
	TransportUnregistering.Broadcast(Name);

	if (!Live.IsValid())
	{
		return;
	}

	// 3. Whatever is still referenced belongs to an owner that did not subscribe. Stop it, so it no
	//    longer runs transport code on its own threads, and release the sinks a receiver was given
	//    (a receiver also releases its control sink in Stop, ADR 0011). Stop and the setters are
	//    game-thread calls; no registry lock is held.
	TArray<FLiveList::FSenderRef> Senders;
	TArray<FLiveList::FReceiverRef> Receivers;
	Live->PinAlive(Senders, Receivers);
	for (const FLiveList::FSenderRef& Sender : Senders)
	{
		Sender->Stop();
	}
	for (const FLiveList::FReceiverRef& Receiver : Receivers)
	{
		Receiver->Stop();
		Receiver->SetConsumer(nullptr);
		if (Receiver->SupportsControl())
		{
			Receiver->SetControlSink(nullptr);
		}
	}
	const int32 NumStopped = Senders.Num() + Receivers.Num();
	Senders.Reset();
	Receivers.Reset();

	// 4. Anything still alive now is a leak: report it, and keep counting it in GetNumLiveInstances
	//    so an FFI library is not freed under it.
	int32 LeakedSenders = 0;
	int32 LeakedReceivers = 0;
	const int32 Leaked = Live->CountAlive(&LeakedSenders, &LeakedReceivers);
	if (Leaked > 0)
	{
		const FString OwningModule = (Removed.IsValid() && !Removed->OwningModule.IsNone()) ? Removed->OwningModule.ToString() : FString(TEXT("unknown"));
		UE_LOG(LogO3DShared, Error,
			TEXT("Transport '%s' from module %s unregistered with %d live instance(s) still referenced (%d sender(s), %d receiver(s)). ")
			TEXT("They were stopped, but their owner did not release them on OnTransportUnregistering; the transport's code stays in use until they are released."),
			*Name.ToString(), *OwningModule, Leaked, LeakedSenders, LeakedReceivers);
	}
	else if (NumStopped > 0)
	{
		UE_LOG(LogO3DShared, Log, TEXT("Transport '%s': stopped %d instance(s) on unregister; all were released."), *Name.ToString(), NumStopped);
	}

	FWriteScopeLock WriteLock(Lock);
	Retired.RemoveAll([](const TPair<FName, FLiveListPtr>& Pair) { return !Pair.Value.IsValid() || Pair.Value->CountAlive() == 0; });
	if (Leaked > 0)
	{
		Retired.Emplace(Name, Live);
	}
}

bool FO3DTransportRegistry::FindEntry(FName Name, FO3DTransportDescriptorPtr& OutDescriptor, FLiveListPtr& OutLive) const
{
	FReadScopeLock ReadLock(Lock);
	const FEntry* Entry = Entries.Find(Name);
	if (Entry == nullptr || !Entry->Descriptor.IsValid())
	{
		return false;
	}
	OutDescriptor = Entry->Descriptor;
	OutLive = Entry->Live;
	return true;
}

int32 FO3DTransportRegistry::GetNumLiveInstances(FName Name) const
{
	TArray<FLiveListPtr> Lists;
	{
		FReadScopeLock ReadLock(Lock);
		if (const FEntry* Entry = Entries.Find(Name))
		{
			Lists.Add(Entry->Live);
		}
		for (const TPair<FName, FLiveListPtr>& Pair : Retired)
		{
			if (Pair.Key == Name)
			{
				Lists.Add(Pair.Value);
			}
		}
	}

	int32 Count = 0;
	for (const FLiveListPtr& List : Lists)
	{
		if (List.IsValid())
		{
			Count += List->CountAlive();
		}
	}
	return Count;
}

void FO3DTransportRegistry::EditLegacyDescriptor(FName Name, TFunctionRef<void(FO3DTransportDescriptor&)> Edit)
{
	// Game thread only, like Register: removing an entry drains it.
	check(IsInGameThread());

	if (Name.IsNone())
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Attempted to register a transport part with None name."));
		return;
	}

	FO3DTransportDescriptorPtr Replaced;
	FLiveListPtr RemovedLive;
	bool bChanged = false;
	bool bRefused = false;
	{
		FWriteScopeLock WriteLock(Lock);
		FEntry* Entry = Entries.Find(Name);
		if (Entry != nullptr && Entry->RegistrationId != 0)
		{
			bRefused = true;
		}
		else
		{
			FO3DTransportDescriptor Copy;
			if (Entry != nullptr && Entry->Descriptor.IsValid())
			{
				Copy = *Entry->Descriptor;
			}
			Copy.Name = Name;
			Edit(Copy);

			if (O3DTransportRegistryPrivate::IsEmptyLegacyDescriptor(Copy))
			{
				if (Entry != nullptr)
				{
					Replaced = Entry->Descriptor;
					RemovedLive = Entry->Live;
					Entries.Remove(Name);
					bChanged = true;
				}
			}
			else
			{
				FEntry& Target = Entry != nullptr ? *Entry : Entries.Add(Name);
				Replaced = Target.Descriptor;
				Target.Descriptor = MakeShared<FO3DTransportDescriptor, ESPMode::ThreadSafe>(MoveTemp(Copy));
				if (!Target.Live.IsValid())
				{
					// Instances made from earlier versions of this legacy entry stay in the same list.
					Target.Live = MakeShared<FLiveList, ESPMode::ThreadSafe>();
				}
				Target.RegistrationId = 0;
				bChanged = true;
			}
		}
	}

	if (bRefused)
	{
		UE_LOG(LogO3DShared, Warning,
			TEXT("Deprecated transport registration call for '%s' ignored: that name is registered with a descriptor (FO3DTransportRegistry::Register)."),
			*Name.ToString());
		return;
	}

	if (RemovedLive.IsValid())
	{
		Drain(Name, Replaced, RemovedLive);
	}

	if (bChanged)
	{
		TransportsChanged.Broadcast();
	}
}

FO3DTransportDescriptorPtr FO3DTransportRegistry::Find(FName Name) const
{
	FReadScopeLock ReadLock(Lock);
	const FEntry* Entry = Entries.Find(Name);
	return Entry != nullptr ? Entry->Descriptor : FO3DTransportDescriptorPtr();
}

bool FO3DTransportRegistry::IsRegistered(FName Name, EO3DTransportRole Role) const
{
	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	return Descriptor.IsValid() && Descriptor->HasRole(Role);
}

TArray<FName> FO3DTransportRegistry::GetNames(EO3DTransportRole Role) const
{
	TArray<FName> Names;
	{
		FReadScopeLock ReadLock(Lock);
		Names.Reserve(Entries.Num());
		for (const TPair<FName, FEntry>& Pair : Entries)
		{
			if (Pair.Value.Descriptor.IsValid() && Pair.Value.Descriptor->HasRole(Role))
			{
				Names.Add(Pair.Key);
			}
		}
	}
	Names.Sort(FNameLexicalLess());
	return Names;
}

bool FO3DTransportRegistry::GetSecretDeclaration(FName Name, EO3DTransportRole Role, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars) const
{
	OutSecretKeys.Reset();
	OutSecretEnvVars.Reset();

	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	if (!Descriptor.IsValid())
	{
		return false;
	}

	const FO3DTransportRoleOptions& Options = Descriptor->GetRoleOptions(Role);
	OutSecretKeys = Options.SecretOptionKeys;
	OutSecretEnvVars = Options.SecretEnvVars;
	return true;
}

bool FO3DTransportRegistry::GetOptionSchema(FName Name, EO3DTransportRole Role, FO3DTransportOptionSchema& OutSchema) const
{
	OutSchema.Reset();

	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	if (!Descriptor.IsValid())
	{
		return false;
	}

	OutSchema = Descriptor->GetRoleOptions(Role).OptionSchema;
	return true;
}

bool FO3DTransportRegistry::GetCapabilities(FName Name, const FO3DTransportConfig& Config, FO3DTransportCapabilities& OutCapabilities) const
{
	OutCapabilities = FO3DTransportCapabilities();

	// The descriptor is pinned, so its function is called outside the lock (ADR 0007 item 4).
	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	if (!Descriptor.IsValid())
	{
		return false;
	}

	if (Descriptor->GetCapabilities)
	{
		OutCapabilities = Descriptor->GetCapabilities(Config);
	}
	OutCapabilities.bSend = Descriptor->HasRole(EO3DTransportRole::Sender);
	OutCapabilities.bReceive = Descriptor->HasRole(EO3DTransportRole::Receiver);
	return true;
}

TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> FO3DTransportRegistry::CreateSender(FName Name) const
{
	// The descriptor is pinned, so the factory is called outside the lock and cannot be freed
	// by a concurrent unregister while it runs.
	FO3DTransportDescriptorPtr Descriptor;
	FLiveListPtr Live;
	if (!FindEntry(Name, Descriptor, Live) || !Descriptor->CreateSender)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("No %s factory registered for transport '%s'."),
			O3DTransportRegistryPrivate::RoleName(EO3DTransportRole::Sender), *Name.ToString());
		return nullptr;
	}

	TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Instance = Descriptor->CreateSender();
	if (Instance.IsValid() && Live.IsValid() && !Live->TryAddSender(Instance))
	{
		// The transport unregistered (and drained) while the factory ran: nothing would drain this one.
		UE_LOG(LogO3DShared, Warning, TEXT("Transport '%s' unregistered while a sender was being created; the new sender was discarded."), *Name.ToString());
		Instance->Stop();
		return nullptr;
	}
	return Instance;
}

TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> FO3DTransportRegistry::CreateReceiver(FName Name) const
{
	FO3DTransportDescriptorPtr Descriptor;
	FLiveListPtr Live;
	if (!FindEntry(Name, Descriptor, Live) || !Descriptor->CreateReceiver)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("No %s factory registered for transport '%s'."),
			O3DTransportRegistryPrivate::RoleName(EO3DTransportRole::Receiver), *Name.ToString());
		return nullptr;
	}

	TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Instance = Descriptor->CreateReceiver();
	if (Instance.IsValid() && Live.IsValid() && !Live->TryAddReceiver(Instance))
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Transport '%s' unregistered while a receiver was being created; the new receiver was discarded."), *Name.ToString());
		Instance->Stop();
		return nullptr;
	}
	return Instance;
}

int32 FO3DTransportRegistry::Num() const
{
	FReadScopeLock ReadLock(Lock);
	return Entries.Num();
}

// ── FO3DTransportRegistration ────────────────────────────────────────────────────────────

FO3DTransportRegistration::FO3DTransportRegistration() = default;

FO3DTransportRegistration::FO3DTransportRegistration(const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe>& InRegistry, FName InName, uint64 InRegistrationId)
	: Registry(InRegistry)
	, Name(InName)
	, RegistrationId(InRegistrationId)
{
}

FO3DTransportRegistration::~FO3DTransportRegistration()
{
	Reset();
}

FO3DTransportRegistration::FO3DTransportRegistration(FO3DTransportRegistration&& Other)
	: Registry(MoveTemp(Other.Registry))
	, Name(Other.Name)
	, RegistrationId(Other.RegistrationId)
{
	Other.Registry.Reset();
	Other.Name = NAME_None;
	Other.RegistrationId = 0;
}

FO3DTransportRegistration& FO3DTransportRegistration::operator=(FO3DTransportRegistration&& Other)
{
	if (this != &Other)
	{
		Reset();
		Registry = MoveTemp(Other.Registry);
		Name = Other.Name;
		RegistrationId = Other.RegistrationId;
		Other.Registry.Reset();
		Other.Name = NAME_None;
		Other.RegistrationId = 0;
	}
	return *this;
}

bool FO3DTransportRegistration::IsValid() const
{
	return RegistrationId != 0 && Registry.IsValid();
}

void FO3DTransportRegistration::Reset()
{
	if (RegistrationId != 0)
	{
		if (const TSharedPtr<FO3DTransportRegistry, ESPMode::ThreadSafe> Pinned = Registry.Pin())
		{
			Pinned->Unregister(Name, RegistrationId);
		}
	}
	Registry.Reset();
	Name = NAME_None;
	RegistrationId = 0;
}
