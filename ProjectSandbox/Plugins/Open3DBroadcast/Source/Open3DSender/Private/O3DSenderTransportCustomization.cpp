// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSenderTransportCustomization.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
    FCriticalSection GSenderCustomizationMutex;
    TMap<FName, FO3DSenderTransportCustomization> GSenderCustomizations;
}

void O3DSender::RegisterTransportCustomization(FName TransportName, FO3DSenderTransportCustomization&& Customization)
{
    FScopeLock Lock(&GSenderCustomizationMutex);
    GSenderCustomizations.Add(TransportName, MoveTemp(Customization));
}

void O3DSender::UnregisterTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GSenderCustomizationMutex);
    GSenderCustomizations.Remove(TransportName);
}

const FO3DSenderTransportCustomization* O3DSender::FindTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GSenderCustomizationMutex);
    return GSenderCustomizations.Find(TransportName);
}

void O3DSender::GetRegisteredTransportNames(TArray<FName>& OutNames)
{
    TArray<FName> LocalNames;
    {
        FScopeLock Lock(&GSenderCustomizationMutex);
        GSenderCustomizations.GetKeys(LocalNames);
    }

    LocalNames.Sort(FNameLexicalLess());
    OutNames = MoveTemp(LocalNames);
}

bool O3DSender::GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars)
{
    OutSecretKeys.Reset();
    OutSecretEnvVars.Reset();

    FScopeLock Lock(&GSenderCustomizationMutex);
    const FO3DSenderTransportCustomization* Customization = GSenderCustomizations.Find(TransportName);
    if (!Customization)
    {
        return false;
    }

    OutSecretKeys = Customization->SecretOptionKeys;
    OutSecretEnvVars = Customization->SecretEnvVars;
    return true;
}
