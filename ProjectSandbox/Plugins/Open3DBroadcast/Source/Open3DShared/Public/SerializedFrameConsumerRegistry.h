// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

/** Interface implemented by modules that want to consume serialized O3DS frames. */
class OPEN3DSHARED_API ISerializedFrameConsumer : public TSharedFromThis<ISerializedFrameConsumer>
{
public:
	virtual ~ISerializedFrameConsumer() = default;

	/**
	 * Process a serialized frame payload.
	 *
	 * @param Subject            Subject identifier associated with the payload.
	 * @param Buffer             Serialized SubjectList payload.
	 * @param TimestampSeconds   Timestamp (seconds) associated with the captured frame.
	 */
	virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) = 0;
};

using FSerializedFrameConsumerFactory = TFunction<TSharedPtr<ISerializedFrameConsumer>()>;

/**
 * Registry used by transports to lazily create serialized frame consumers without
 * taking a static dependency on the consumer implementation module.
 */
class OPEN3DSHARED_API FSerializedFrameConsumerRegistry
{
public:
	/** Register a factory that will be used to create consumers on demand. */
	static void RegisterFactory(FSerializedFrameConsumerFactory InFactory);

	/** Clear the currently registered factory. */
	static void ClearFactory();

	/** Create a consumer using the currently registered factory if available. */
	static TSharedPtr<ISerializedFrameConsumer> Create();
};