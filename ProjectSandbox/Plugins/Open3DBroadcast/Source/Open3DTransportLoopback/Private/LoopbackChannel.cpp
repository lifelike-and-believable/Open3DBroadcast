#include "LoopbackChannel.h"

#include "HAL/CriticalSection.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY(LogO3DLoopbackTransport);

namespace
{
    FCriticalSection GLoopbackChannelMutex;
    TMap<FString, TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe>> GLoopbackChannels;

    FString NormaliseChannelKey(const FString& InKey)
    {
        FString Key = InKey;
        Key.TrimStartAndEndInline();
        if (Key.IsEmpty())
        {
            Key = TEXT("default");
        }
        return Key.ToLower();
    }
}

namespace O3DLoopback
{
    int32 ResolveQueueCapacity(const FO3DTransportConfig& Config)
    {
        int32 Capacity = 64;
        for (const TPair<FString, FString>& Pair : Config.AdvancedParams)
        {
            if (Pair.Key.Equals(TEXT("loopback.maxqueue"), ESearchCase::IgnoreCase) ||
                Pair.Key.Equals(TEXT("maxqueue"), ESearchCase::IgnoreCase))
            {
                const int32 Parsed = FCString::Atoi(*Pair.Value);
                if (Parsed > 0)
                {
                    Capacity = Parsed;
                }
            }
        }
        return Capacity;
    }

    FString ResolveChannelKey(const FO3DTransportConfig& Config)
    {
        if (!Config.StreamId.IsEmpty())
        {
            return NormaliseChannelKey(Config.StreamId);
        }
        if (!Config.Uri.IsEmpty())
        {
            return NormaliseChannelKey(Config.Uri);
        }
        return NormaliseChannelKey(TEXT("loopback"));
    }

    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> AcquireChannel(const FString& ChannelKey, int32 Capacity)
    {
        const FString NormalisedKey = NormaliseChannelKey(ChannelKey);
        FScopeLock Guard(&GLoopbackChannelMutex);

        if (TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe>* ExistingPtr = GLoopbackChannels.Find(NormalisedKey))
        {
            if (TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Existing = ExistingPtr->Pin())
            {
                Existing->Capacity = FMath::Max(1, Capacity);
                return Existing;
            }
        }

        TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> NewChannel = MakeShared<FO3DLoopbackChannel, ESPMode::ThreadSafe>(NormalisedKey, Capacity);
        GLoopbackChannels.Add(NormalisedKey, NewChannel);
        return NewChannel;
    }
}
