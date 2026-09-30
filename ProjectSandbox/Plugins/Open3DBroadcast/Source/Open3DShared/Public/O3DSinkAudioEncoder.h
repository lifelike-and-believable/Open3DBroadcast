// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/UniquePtr.h"

#include "O3DAudioFrameCodec.h"
#include "Transport/O3DTransportTypes.h"

/**
 * Thread-safe "last observed subject" slot shared between a sender (writer, game or
 * pipeline thread) and its audio sinks (readers, audio thread). Holds no reference to
 * the sender.
 */
class OPEN3DSHARED_API FO3DAudioSubjectSlot
{
public:
	void Set(const FString& InSubject);
	void Reset();
	FString Get() const;

private:
	mutable FCriticalSection Mutex;
	FString Subject;
};

/**
 * Per-sink audio encoders (ADR 0007 addendum, WP-S5: TRB-11, TRF-10).
 *
 * Built from an immutable snapshot taken on the game thread when the sink is created.
 * Keeps one O3DAudio::FFrameEncoder per stream label, so interleaved labels never share
 * a stateful Opus encoder, and nothing reconfigures an encoder while it is in use.
 * Encode() may be called from any thread; it takes only this object's own lock, which is
 * never shared with a socket, the game thread or a network worker.
 */
class OPEN3DSHARED_API FO3DSinkAudioEncoder
{
public:
	struct FSettings
	{
		FO3DTransportAudioConfig Config;
		FString DefaultStreamLabel;
		FString DefaultSubject;
		FGuid SourceGuid;
		/** Upper bound on distinct stream labels; the least recently used encoder is evicted. */
		int32 MaxStreams = 16;
	};

	explicit FO3DSinkAudioEncoder(FSettings InSettings);
	~FO3DSinkAudioEncoder();

	FO3DSinkAudioEncoder(const FO3DSinkAudioEncoder&) = delete;
	FO3DSinkAudioEncoder& operator=(const FO3DSinkAudioEncoder&) = delete;

	/**
	 * Encode one capture buffer into zero or more frames (OutFrames is reset first). Opus
	 * buffers samples until a whole Opus frame is available (SHR-2), so a call may return no
	 * frame and still succeed. SubjectOverride may be empty (the snapshot default is used).
	 * Every frame's Meta.SourceGuid is the snapshot's SourceGuid. Returns false only when the
	 * input is rejected.
	 */
	bool Encode(const FString& StreamLabel,
		const FString& SubjectOverride,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec,
		TArray<O3DAudio::FEncodedFrame>& OutFrames);

	/**
	 * Encode and wrap each resulting frame in the unified envelope (TCP, UDP, NNG wire format).
	 * OutMessages is reset first; each message carries its own frame's timestamp.
	 */
	bool EncodeUnified(const FString& StreamLabel,
		const FString& SubjectOverride,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec,
		TArray<TArray<uint8>>& OutMessages);

	const FSettings& GetSettings() const { return Settings; }

private:
	struct FStreamEncoder
	{
		O3DAudio::FFrameEncoder Encoder;
		uint64 LastUse = 0;
	};

	FStreamEncoder* FindOrCreate(const FString& StreamLabel);

	const FSettings Settings;
	FCriticalSection Mutex;
	TMap<FString, TUniquePtr<FStreamEncoder>> Encoders;
	uint64 UseCounter = 0;
};
