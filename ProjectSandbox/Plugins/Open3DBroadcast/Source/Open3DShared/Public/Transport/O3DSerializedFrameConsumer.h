// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * Interface implemented by whatever consumes the serialized O3DS frames a receiver delivers (the
 * LiveLink receiver source, test recorders). ADR 0007 items 3 and 5; SHR-16.
 *
 * Two forms (WP-A1 PR 5b):
 * - SubmitFrame, the view form: the bytes belong to the caller and are valid only until the call
 *   returns. A consumer that needs them later copies them. Receivers use it when the bytes are
 *   transient (a slice of a received message, a socket or FFI buffer), so nothing is copied to
 *   call it.
 * - SubmitFrameOwned, the owned form: the caller gives up a buffer it owns, and the consumer may
 *   keep it without a copy (move it into a game-thread task, a queue). Receivers use it when they
 *   already hold the frame in its own TArray (a hand-off queue item). The default forwards to
 *   SubmitFrame with a view of the buffer, so a consumer that only implements the view form still
 *   gets every frame.
 * A receiver calls exactly one of the two for each frame.
 *
 * Subject is the subject name exactly as it was sent, case-sensitive (an FString, not the FName
 * ADR 0007 item 3 sketched: FName compares case-insensitively and would merge "Hero" and "hero";
 * see the ADR 0007 addendum "implementation notes (WP-A1 PR 1)" on FO3DSendPayload::Subject).
 *
 * Threading: receivers call both forms from IOpen3DReceiver::Poll(), on the thread that calls
 * Poll() (the game thread today).
 *
 * The unused FSerializedFrameConsumerRegistry that used to sit next to this interface was never
 * populated and is gone (SHR-24).
 */
class OPEN3DSHARED_API ISerializedFrameConsumer : public TSharedFromThis<ISerializedFrameConsumer>
{
public:
	virtual ~ISerializedFrameConsumer() = default;

	/**
	 * View form: process one serialized frame whose bytes are valid only for this call.
	 *
	 * @param Subject            Subject name (case-sensitive), or the receiver's stream id.
	 * @param Bytes              Serialized SubjectList payload; do not keep the view.
	 * @param TimestampSeconds   When the frame should be shown (FPlatformTime clock).
	 */
	virtual void SubmitFrame(const FString& Subject, TConstArrayView<uint8> Bytes, double TimestampSeconds) = 0;

	/**
	 * Owned form: process one serialized frame whose buffer the caller hands over. Override it to
	 * keep the buffer without a copy; the default passes a view of it to SubmitFrame.
	 */
	virtual void SubmitFrameOwned(const FString& Subject, TArray<uint8>&& Bytes, double TimestampSeconds)
	{
		SubmitFrame(Subject, TConstArrayView<uint8>(Bytes), TimestampSeconds);
	}
};
