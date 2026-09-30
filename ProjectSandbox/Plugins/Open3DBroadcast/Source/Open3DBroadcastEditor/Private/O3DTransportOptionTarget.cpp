// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DTransportOptionTarget.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "ScopedTransaction.h"
#include "Transport/O3DTransportRegistry.h"
#include "UObject/Object.h"

#define LOCTEXT_NAMESPACE "O3DTransportOptionTarget"

FString IO3DOptionTarget::GetOption(const FString& Key) const
{
	const TMap<FString, FString> Options = GetOptions();
	const FString* Value = Options.Find(Key);
	return Value ? *Value : FString();
}

bool IO3DOptionTarget::CommitOption(const FString& Key, const FString& Value)
{
	UObject* Object = GetObject();
	if (!Object || Key.IsEmpty() || IsSecretKey(Key))
	{
		return false;
	}

	if (GetOption(Key).Equals(Value, ESearchCase::CaseSensitive))
	{
		return false;
	}

	// One commit, one undo step (SND-35, TRB-45). Modify() records the object's state before the
	// write, so Ctrl+Z restores the previous option map.
	const FScopedTransaction Transaction(LOCTEXT("EditTransportOption", "Edit Transport Option"));
	Object->Modify();
	WriteOption(Key, Value);
	return true;
}

// ---------------------------------------------------------------------------------------------
// FO3DSenderOptionTarget
// ---------------------------------------------------------------------------------------------

FO3DSenderOptionTarget::FO3DSenderOptionTarget(UO3DSenderComponent* InComponent)
	: WeakComponent(InComponent)
{
}

bool FO3DSenderOptionTarget::IsValid() const
{
	return WeakComponent.IsValid();
}

FName FO3DSenderOptionTarget::GetTransportName() const
{
	const UO3DSenderComponent* Component = WeakComponent.Get();
	return Component ? Component->GetTransportName() : NAME_None;
}

TMap<FString, FString> FO3DSenderOptionTarget::GetOptions() const
{
	const UO3DSenderComponent* Component = WeakComponent.Get();
	return Component ? Component->TransportOptions : TMap<FString, FString>();
}

bool FO3DSenderOptionTarget::IsSecretKey(const FString& Key) const
{
	const UO3DSenderComponent* Component = WeakComponent.Get();
	return Component && Component->IsTransportSecretKey(Key);
}

FO3DSecretStatus FO3DSenderOptionTarget::GetSecretStatus(const FString& Key) const
{
	const UO3DSenderComponent* Component = WeakComponent.Get();
	return Component ? Component->GetTransportSecretStatus(Key) : FO3DSecretStatus();
}

void FO3DSenderOptionTarget::SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence)
{
	if (UO3DSenderComponent* Component = WeakComponent.Get())
	{
		Component->SetTransportSecret(Key, Value, Persistence);
	}
}

bool FO3DSenderOptionTarget::SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence)
{
	UO3DSenderComponent* Component = WeakComponent.Get();
	return Component ? Component->SetTransportSecretPersistence(Key, Persistence) : false;
}

void FO3DSenderOptionTarget::ClearSecret(const FString& Key)
{
	if (UO3DSenderComponent* Component = WeakComponent.Get())
	{
		Component->ClearTransportSecret(Key);
	}
}

UObject* FO3DSenderOptionTarget::GetObject() const
{
	return WeakComponent.Get();
}

void FO3DSenderOptionTarget::WriteOption(const FString& Key, const FString& Value)
{
	if (UO3DSenderComponent* Component = WeakComponent.Get())
	{
		// The component's setter: an empty value removes the key, a secret key never lands here
		// (CommitOption refuses secret keys).
		Component->SetTransportOption(Key, Value);
	}
}

// ---------------------------------------------------------------------------------------------
// FO3DReceiverOptionTarget
// ---------------------------------------------------------------------------------------------

FO3DReceiverOptionTarget::FO3DReceiverOptionTarget(UO3DReceiverSettingsObject* InSettingsObject)
	: WeakSettingsObject(InSettingsObject)
{
}

bool FO3DReceiverOptionTarget::IsValid() const
{
	return WeakSettingsObject.IsValid();
}

FName FO3DReceiverOptionTarget::GetTransportName() const
{
	const UO3DReceiverSettingsObject* SettingsObject = WeakSettingsObject.Get();
	return SettingsObject ? SettingsObject->Settings.TransportName : NAME_None;
}

TMap<FString, FString> FO3DReceiverOptionTarget::GetOptions() const
{
	const UO3DReceiverSettingsObject* SettingsObject = WeakSettingsObject.Get();
	return SettingsObject ? SettingsObject->Settings.TransportOptions : TMap<FString, FString>();
}

bool FO3DReceiverOptionTarget::IsSecretKey(const FString& Key) const
{
	const UO3DReceiverSettingsObject* SettingsObject = WeakSettingsObject.Get();
	return SettingsObject && O3DReceiver::IsSecretOptionKey(SettingsObject->Settings.TransportName, Key);
}

bool FO3DReceiverOptionTarget::GetSecretScope(FString& OutTransport, FString& OutProfile) const
{
	const UO3DReceiverSettingsObject* SettingsObject = WeakSettingsObject.Get();
	if (!SettingsObject)
	{
		return false;
	}

	OutTransport = SettingsObject->Settings.TransportName.ToString();
	OutProfile = O3DReceiver::GetCredentialProfile(SettingsObject->Settings);
	return true;
}

FO3DSecretStatus FO3DReceiverOptionTarget::GetSecretStatus(const FString& Key) const
{
	FString Transport;
	FString Profile;
	if (!GetSecretScope(Transport, Profile))
	{
		return FO3DSecretStatus();
	}

	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	FO3DTransportRegistry::Get().GetSecretDeclaration(FName(*Transport), EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars);
	const FString* EnvVar = SecretEnvVars.Find(Key);
	return FO3DSecretStore::Get().Describe(Transport, Profile, Key, EnvVar ? *EnvVar : FString());
}

void FO3DReceiverOptionTarget::SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence)
{
	FString Transport;
	FString Profile;
	if (GetSecretScope(Transport, Profile))
	{
		FO3DSecretStore::Get().Set(Transport, Profile, Key, Value, Persistence);
	}
}

bool FO3DReceiverOptionTarget::SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence)
{
	FString Transport;
	FString Profile;
	return GetSecretScope(Transport, Profile) && FO3DSecretStore::Get().SetPersistence(Transport, Profile, Key, Persistence);
}

void FO3DReceiverOptionTarget::ClearSecret(const FString& Key)
{
	FString Transport;
	FString Profile;
	if (GetSecretScope(Transport, Profile))
	{
		FO3DSecretStore::Get().Clear(Transport, Profile, Key);
	}
}

UObject* FO3DReceiverOptionTarget::GetObject() const
{
	return WeakSettingsObject.Get();
}

void FO3DReceiverOptionTarget::WriteOption(const FString& Key, const FString& Value)
{
	UO3DReceiverSettingsObject* SettingsObject = WeakSettingsObject.Get();
	if (!SettingsObject)
	{
		return;
	}

	if (Value.IsEmpty())
	{
		SettingsObject->Settings.TransportOptions.Remove(Key);
	}
	else
	{
		SettingsObject->Settings.TransportOptions.Add(Key, Value);
	}
}

#undef LOCTEXT_NAMESPACE
