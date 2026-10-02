// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DTransportOptionSet.h"

void O3DTransportOptions::SwitchTransportOptions(TMap<FString, FString>& Active, TMap<FName, FO3DTransportOptionSet>& Inactive,
	FName From, FName To, const TArray<FString>& FromSecretKeys)
{
	if (From == To)
	{
		return;
	}

	// Put the outgoing transport's options away, never with a credential in them (ADR 0004).
	for (const FString& SecretKey : FromSecretKeys)
	{
		Active.Remove(SecretKey);
	}
	if (!From.IsNone())
	{
		if (Active.Num() > 0)
		{
			Inactive.FindOrAdd(From).Options = MoveTemp(Active);
		}
		else
		{
			Inactive.Remove(From);
		}
	}

	// Bring back what the incoming transport had, or start empty.
	FO3DTransportOptionSet Restored;
	if (!To.IsNone() && Inactive.RemoveAndCopyValue(To, Restored))
	{
		Active = MoveTemp(Restored.Options);
	}
	else
	{
		Active.Reset();
	}
}
