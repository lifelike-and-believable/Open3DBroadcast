// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

class OPEN3DSHAREDNEXT_API ISerializedFrameConsumer : public TSharedFromThis<ISerializedFrameConsumer>
{
public:
    virtual ~ISerializedFrameConsumer() = default;
    virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) = 0;
};

using FSerializedFrameConsumerFactory = TFunction<TSharedPtr<ISerializedFrameConsumer>()>;

class OPEN3DSHAREDNEXT_API FSerializedFrameConsumerRegistry
{
public:
    static void RegisterFactory(FSerializedFrameConsumerFactory InFactory);
    static void ClearFactory();
    static TSharedPtr<ISerializedFrameConsumer> Create();
};