// Copyright Lifelike & Believable. All Rights Reserved.

#include "SerializedFrameConsumerRegistry.h"

#include "HAL/CriticalSection.h"

static TUniqueFunction<TSharedPtr<ISerializedFrameConsumer>()> GSerializedFrameConsumerFactory;
static FCriticalSection GFactoryLock;

void FSerializedFrameConsumerRegistry::RegisterFactory(FSerializedFrameConsumerFactory InFactory)
{
    FScopeLock Lock(&GFactoryLock);
    GSerializedFrameConsumerFactory = TUniqueFunction<TSharedPtr<ISerializedFrameConsumer>()>(InFactory);
}

void FSerializedFrameConsumerRegistry::ClearFactory()
{
    FScopeLock Lock(&GFactoryLock);
    GSerializedFrameConsumerFactory = nullptr;
}

TSharedPtr<ISerializedFrameConsumer> FSerializedFrameConsumerRegistry::Create()
{
    FScopeLock Lock(&GFactoryLock);
    if (GSerializedFrameConsumerFactory)
    {
        return GSerializedFrameConsumerFactory();
    }
    return nullptr;
}