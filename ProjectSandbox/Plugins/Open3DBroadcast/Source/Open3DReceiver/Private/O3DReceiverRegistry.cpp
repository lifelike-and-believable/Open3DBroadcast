#include "O3DReceiverRegistry.h"

#include "Algo/Sort.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogO3DReceiverRegistry, Log, All);

namespace
{
    FCriticalSection GReceiverRegistryMutex;
    TMap<FName, FO3DReceiverFactory> GReceiverFactories;

    template<typename FactoryType>
    bool IsFactoryValid(const FactoryType& Factory)
    {
        return static_cast<bool>(Factory);
    }

    void RemoveFactoryInternal(TMap<FName, FO3DReceiverFactory>& FactoryMap, FName TransportName)
    {
        FactoryMap.Remove(TransportName);
    }

    void SnapshotKeys(const TMap<FName, FO3DReceiverFactory>& FactoryMap, TArray<FName>& OutNames)
    {
        OutNames.Reserve(FactoryMap.Num());
        for (const TPair<FName, FO3DReceiverFactory>& Pair : FactoryMap)
        {
            OutNames.Add(Pair.Key);
        }
    }
}

namespace O3DTransport
{
    void RegisterReceiver(FName TransportName, FO3DReceiverFactory&& Factory)
    {
        if (!TransportName.IsNone() && !IsFactoryValid(Factory))
        {
            UE_LOG(LogO3DReceiverRegistry, Warning, TEXT("Attempted to register receiver factory for '%s' with no callable."), *TransportName.ToString());
            return;
        }

        FScopeLock Lock(&GReceiverRegistryMutex);
        if (TransportName.IsNone())
        {
            UE_LOG(LogO3DReceiverRegistry, Warning, TEXT("Attempted to register receiver factory with None name."));
            return;
        }

        if (GReceiverFactories.Contains(TransportName))
        {
            UE_LOG(LogO3DReceiverRegistry, Verbose, TEXT("Overriding existing receiver factory for transport '%s'."), *TransportName.ToString());
        }

        if (IsFactoryValid(Factory))
        {
            GReceiverFactories.Add(TransportName, MoveTemp(Factory));
        }
        else
        {
            RemoveFactoryInternal(GReceiverFactories, TransportName);
        }
    }

    void UnregisterReceiver(FName TransportName)
    {
        FScopeLock Lock(&GReceiverRegistryMutex);
        RemoveFactoryInternal(GReceiverFactories, TransportName);
    }

    TSharedPtr<IOpen3DReceiver> CreateReceiver(FName TransportName)
    {
        FO3DReceiverFactory FactoryCopy;
        {
            FScopeLock Lock(&GReceiverRegistryMutex);
            if (FO3DReceiverFactory* Existing = GReceiverFactories.Find(TransportName))
            {
                FactoryCopy = *Existing;
            }
        }

        if (!IsFactoryValid(FactoryCopy))
        {
            UE_LOG(LogO3DReceiverRegistry, Warning, TEXT("No receiver factory registered for transport '%s'."), *TransportName.ToString());
            return nullptr;
        }

        return FactoryCopy();
    }

    TArray<FName> GetRegisteredReceivers()
    {
        TArray<FName> Names;
        {
            FScopeLock Lock(&GReceiverRegistryMutex);
            SnapshotKeys(GReceiverFactories, Names);
        }
        Names.Sort(FNameLexicalLess());
        return Names;
    }
}
