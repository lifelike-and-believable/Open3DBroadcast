// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DControlTypes.h"
#include "Templates/UniquePtr.h"

class IOpen3DSender;

namespace O3DS
{
	namespace Control
	{
		class ControlPublisher;
	}
}

/**
 * The sending side of the control channel for one sender component
 * (docs/adr/0011-control-channel.md, item 8). Wraps the core O3DS::Control::ControlPublisher:
 * converts engine values, stamps times on the sender clock (FPlatformTime, the clock
 * SubjectList.time uses), wraps each message in a control envelope and hands it to the
 * transport, returning refused messages to the core for retry.
 *
 * The source id is a fresh GUID per instance and is never serialized, so a duplicated actor gets
 * its own id and two senders never merge state on a receiver.
 *
 * Game thread only.
 */
class OPEN3DSENDER_API FO3DControlPublisher
{
public:
	explicit FO3DControlPublisher(const FString& SourceName);
	~FO3DControlPublisher();

	FO3DControlPublisher(const FO3DControlPublisher&) = delete;
	FO3DControlPublisher& operator=(const FO3DControlPublisher&) = delete;

	const FString& GetSourceId() const { return SourceId; }

	/** Snapshot interval (0.25-10 s), event copies (1-5) and per-key value rate (1-120 Hz); clamped. */
	void SetConfig(double SnapshotIntervalSeconds, int32 EventRedundancy, double MaxValueRateHz);

	/**
	 * The subject names this sender streams mocap under, exactly as they go on the wire, so
	 * receivers can align control to that stream. Cheap to call every tick: unchanged names are
	 * ignored.
	 */
	void SetMocapSubjects(const TArray<FString>& Subjects);

	/** Starts a session (new epoch) and sends a snapshot of the current values at once. */
	void Start();
	/** Ends the session. Values are kept for the next Start; pending events are dropped. */
	void Stop();
	bool IsRunning() const;

	/** Sets a value; allowed while stopped (sent at the next Start). False with a reason when refused. */
	bool SetValue(const FString& Key, const FString& TargetSubject, const FO3DControlValue& Value, FString* OutError = nullptr);
	bool ClearValue(const FString& Key, const FString& TargetSubject);
	void ClearAll();
	/** Fires an event now (sender clock). Needs a running session. False with a reason when refused. */
	bool FireEvent(const FString& Name, const FString& TargetSubject, const FO3DControlValue& Value, FString* OutError = nullptr);
	bool FindValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const;

	/**
	 * Sends what is due through Sender (which must support control). Returns the number of
	 * messages the transport accepted. Messages it refuses are retried on later ticks.
	 */
	int32 Tick(IOpen3DSender& Sender);

private:
	FString SourceId;
	TArray<FString> LastMocapSubjects;
	TUniquePtr<O3DS::Control::ControlPublisher> Core;
	/** Sequence number of the next control envelope (envelope v2, ADR 0009 item 4). */
	uint32 NextEnvelopeSeq = 0;
};
