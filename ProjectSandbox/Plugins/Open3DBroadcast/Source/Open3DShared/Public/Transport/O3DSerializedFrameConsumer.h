// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

/**
 * Interface implemented by whatever consumes the serialized O3DS frames a receiver delivers (the
 * LiveLink receiver source, test recorders). ADR 0007 item 3.
 *
 * Threading: receivers call SubmitFrame from IOpen3DReceiver::Poll(), on the thread that calls
 * Poll() (the game thread today).
 *
 * The unused FSerializedFrameConsumerRegistry that used to sit next to this interface was never
 * populated and is gone (SHR-24). The old header "SerializedFrameConsumerRegistry.h" forwards here
 * for one release.
 */
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
