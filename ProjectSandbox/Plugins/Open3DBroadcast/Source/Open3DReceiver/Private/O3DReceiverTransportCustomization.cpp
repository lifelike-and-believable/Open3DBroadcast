// Copyright (c) Open3DStream Contributors

#include "O3DReceiverTransportCustomization.h"

#include "O3DReceiverSourceSettings.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
    FCriticalSection GReceiverCustomizationMutex;
    TMap<FName, FO3DReceiverTransportCustomization> GReceiverCustomizations;
}

void O3DReceiver::RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    GReceiverCustomizations.Add(TransportName, MoveTemp(Customization));
}

void O3DReceiver::UnregisterTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    GReceiverCustomizations.Remove(TransportName);
}

const FO3DReceiverTransportCustomization* O3DReceiver::FindTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    return GReceiverCustomizations.Find(TransportName);
}

void O3DReceiver::GetRegisteredTransportNames(TArray<FName>& OutNames)
{
    TArray<FName> LocalNames;
    {
        FScopeLock Lock(&GReceiverCustomizationMutex);
        GReceiverCustomizations.GetKeys(LocalNames);
    }

    LocalNames.Sort(FNameLexicalLess());
    OutNames = MoveTemp(LocalNames);
}
