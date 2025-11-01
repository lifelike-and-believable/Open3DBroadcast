// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Interface for consuming serialized O3DS frames (subject name + flatbuffer bytes + timestamp)
class OPEN3DSHARED_API ISerializedFrameConsumer : public TSharedFromThis<ISerializedFrameConsumer>
{
public:
    virtual ~ISerializedFrameConsumer() = default;

    // Submit a serialized frame; implementation owns threading and delivery
    virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) = 0;
};

// Registry for a single factory that can create ISerializedFrameConsumer instances on demand.
// The receiver module registers a factory at startup; broadcast can request a consumer if available.
using FSerializedFrameConsumerFactory = TFunction<TSharedPtr<ISerializedFrameConsumer>()>;

class OPEN3DSHARED_API FSerializedFrameConsumerRegistry
{
public:
    // Register a factory; overwrites any existing factory
    static void RegisterFactory(FSerializedFrameConsumerFactory InFactory);

    // Clear the registered factory
    static void ClearFactory();

    // Create a consumer if a factory is registered; returns nullptr otherwise
    static TSharedPtr<ISerializedFrameConsumer> Create();
};
