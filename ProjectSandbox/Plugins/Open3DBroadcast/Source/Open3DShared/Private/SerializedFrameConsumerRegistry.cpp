// Copyright (c) Open3DStream Contributors

#include "SerializedFrameConsumerRegistry.h"

static TUniqueFunction<TSharedPtr<ISerializedFrameConsumer>()> GSerializedFrameConsumerFactory;

void FSerializedFrameConsumerRegistry::RegisterFactory(FSerializedFrameConsumerFactory InFactory)
{
    GSerializedFrameConsumerFactory = TUniqueFunction<TSharedPtr<ISerializedFrameConsumer>()>(InFactory);
}

void FSerializedFrameConsumerRegistry::ClearFactory()
{
    GSerializedFrameConsumerFactory = nullptr;
}

TSharedPtr<ISerializedFrameConsumer> FSerializedFrameConsumerRegistry::Create()
{
    if (GSerializedFrameConsumerFactory)
    {
        return GSerializedFrameConsumerFactory();
    }
    return nullptr;
}