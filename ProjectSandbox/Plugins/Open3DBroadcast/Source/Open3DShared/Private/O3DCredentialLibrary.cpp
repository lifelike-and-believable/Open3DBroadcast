// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DCredentialLibrary.h"

#include "O3DSecretStore.h"

void UO3DCredentialLibrary::SetTransportSecret(FName TransportName, const FString& Profile, const FString& Key, const FString& Value)
{
	// Session only: runtime callers never persist a secret (ADR 0004 items 3 and 8).
	FO3DSecretStore::Get().Set(TransportName.ToString(), Profile, Key, Value, EO3DSecretPersistence::Session);
}

void UO3DCredentialLibrary::ClearTransportSecret(FName TransportName, const FString& Profile, const FString& Key)
{
	FO3DSecretStore::Get().Clear(TransportName.ToString(), Profile, Key);
}
