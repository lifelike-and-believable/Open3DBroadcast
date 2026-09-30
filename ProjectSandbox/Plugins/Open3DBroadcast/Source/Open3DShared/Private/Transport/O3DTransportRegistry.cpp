// Copyright Lifelike & Believable. All Rights Reserved.

// This file also includes (through the registry header) the OPEN3DSHARED_API interface classes
// IOpen3DSender, IOpen3DReceiver, their audio sinks and ISerializedFrameConsumer, which have only
// inline members; including them in an Open3DShared translation unit is what emits their exported
// virtual function tables for the modules that import them.
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"

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
	// Destroyed outside the lock: the descriptor's functions live in the transport's module.
	FO3DTransportDescriptorPtr Removed;
	{
		FWriteScopeLock WriteLock(Lock);
		const FEntry* Entry = Entries.Find(Name);
		if (Entry == nullptr || Entry->RegistrationId != RegistrationId)
		{
			return;
		}
		Removed = Entry->Descriptor;
		Entries.Remove(Name);
	}

	UE_LOG(LogO3DShared, Verbose, TEXT("Transport '%s' unregistered."), *Name.ToString());
	TransportsChanged.Broadcast();
}

void FO3DTransportRegistry::EditLegacyDescriptor(FName Name, TFunctionRef<void(FO3DTransportDescriptor&)> Edit)
{
	if (Name.IsNone())
	{
		UE_LOG(LogO3DShared, Warning, TEXT("Attempted to register a transport part with None name."));
		return;
	}

	FO3DTransportDescriptorPtr Replaced;
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
					Entries.Remove(Name);
					bChanged = true;
				}
			}
			else
			{
				FEntry& Target = Entry != nullptr ? *Entry : Entries.Add(Name);
				Replaced = Target.Descriptor;
				Target.Descriptor = MakeShared<FO3DTransportDescriptor, ESPMode::ThreadSafe>(MoveTemp(Copy));
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

TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> FO3DTransportRegistry::CreateSender(FName Name) const
{
	// The descriptor is pinned, so the factory is called outside the lock and cannot be freed
	// by a concurrent unregister while it runs.
	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	if (!Descriptor.IsValid() || !Descriptor->CreateSender)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("No %s factory registered for transport '%s'."),
			O3DTransportRegistryPrivate::RoleName(EO3DTransportRole::Sender), *Name.ToString());
		return nullptr;
	}
	return Descriptor->CreateSender();
}

TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> FO3DTransportRegistry::CreateReceiver(FName Name) const
{
	const FO3DTransportDescriptorPtr Descriptor = Find(Name);
	if (!Descriptor.IsValid() || !Descriptor->CreateReceiver)
	{
		UE_LOG(LogO3DShared, Warning, TEXT("No %s factory registered for transport '%s'."),
			O3DTransportRegistryPrivate::RoleName(EO3DTransportRole::Receiver), *Name.ToString());
		return nullptr;
	}
	return Descriptor->CreateReceiver();
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
