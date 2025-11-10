#include "O3DSenderRegistry.h"

#include "Algo/Sort.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogO3DSenderRegistry, Log, All);

namespace
{
    FCriticalSection GSenderRegistryMutex;
    TMap<FName, FO3DSenderFactory> GSenderFactories;

    template<typename FactoryType>
    bool IsFactoryValid(const FactoryType& Factory)
    {
        return static_cast<bool>(Factory); // TFunction has operator bool
    }

    void RemoveFactoryInternal(TMap<FName, FO3DSenderFactory>& FactoryMap, FName TransportName)
    {
        FactoryMap.Remove(TransportName);
    }

    void SnapshotKeys(const TMap<FName, FO3DSenderFactory>& FactoryMap, TArray<FName>& OutNames)
    {
        OutNames.Reserve(FactoryMap.Num());
        for (const TPair<FName, FO3DSenderFactory>& Pair : FactoryMap)
        {
            OutNames.Add(Pair.Key);
        }
    }
}

namespace O3DTransport
{
    void RegisterSender(FName TransportName, FO3DSenderFactory&& Factory)
    {
        if (!TransportName.IsNone() && !IsFactoryValid(Factory))
        {
            UE_LOG(LogO3DSenderRegistry, Warning, TEXT("Attempted to register sender factory for '%s' with no callable."), *TransportName.ToString());
            return;
        }

        FScopeLock Lock(&GSenderRegistryMutex);
        if (TransportName.IsNone())
        {
            UE_LOG(LogO3DSenderRegistry, Warning, TEXT("Attempted to register sender factory with None name."));
            return;
        }

        if (GSenderFactories.Contains(TransportName))
        {
            UE_LOG(LogO3DSenderRegistry, Verbose, TEXT("Overriding existing sender factory for transport '%s'."), *TransportName.ToString());
        }

        if (IsFactoryValid(Factory))
        {
            GSenderFactories.Add(TransportName, MoveTemp(Factory));
        }
        else
        {
            RemoveFactoryInternal(GSenderFactories, TransportName);
        }
    }

    void UnregisterSender(FName TransportName)
    {
        FScopeLock Lock(&GSenderRegistryMutex);
        RemoveFactoryInternal(GSenderFactories, TransportName);
    }

    TSharedPtr<IOpen3DSender> CreateSender(FName TransportName)
    {
        FO3DSenderFactory FactoryCopy;
        {
            FScopeLock Lock(&GSenderRegistryMutex);
            if (FO3DSenderFactory* Existing = GSenderFactories.Find(TransportName))
            {
                FactoryCopy = *Existing;
            }
        }

        if (!IsFactoryValid(FactoryCopy))
        {
            UE_LOG(LogO3DSenderRegistry, Warning, TEXT("No sender factory registered for transport '%s'."), *TransportName.ToString());
            return nullptr;
        }

        return FactoryCopy();
    }

    TArray<FName> GetRegisteredSenders()
    {
        TArray<FName> Names;
        {
            FScopeLock Lock(&GSenderRegistryMutex);
            SnapshotKeys(GSenderFactories, Names);
        }
        Names.Sort(FNameLexicalLess());
        return Names;
    }
}
