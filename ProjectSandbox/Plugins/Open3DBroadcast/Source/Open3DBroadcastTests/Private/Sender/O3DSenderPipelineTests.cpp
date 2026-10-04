// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A2c (ADR 0008 implementation outline item 4, "Verification / acceptance"): the sender pose
// pipeline. A scripted transport that can hold the worker inside SendSerialized makes the queue
// back up deterministically (HANDOFF pitfall 14): the test waits on an event until the worker is
// inside the send, then fills the queue. Every wait for the worker is an event or a condition with
// a timeout, never a bare sleep. Pipelines are driven through FO3DSenderPipelineProbe and
// components through FO3DSenderComponentTestAccess (Open3DSender/Public/Testing/O3DSenderTesting.h).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DHelpers.h"
#include "O3DSenderCapture.h"
#include "O3DSenderComponent.h"
#include "O3DSenderPipelineStats.h"
#include "O3DSenderSerializer.h"
#include "O3DTestFakes.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DTransportTypes.h"

#include "CoreGlobals.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtrTemplates.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/capture.h"
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <fstream>

#include <atomic>
#include <string>
#include <vector>

namespace O3DSenderPipelineTests
{
	/** Generous: every test waits for an event or condition, so this only bounds a failure. */
	constexpr double WaitTimeoutSeconds = 10.0;

	TSharedPtr<const FO3DSSkeletonDescriptor> MakeThreeBoneDescriptor()
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		Descriptor->BoneNames = { FName(TEXT("Root")), FName(TEXT("Spine")), FName(TEXT("Head")) };
		Descriptor->ParentIndices = { -1, 0, 1 };
		Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
		return Descriptor;
	}

	/** A pose that changes a little every frame, so quantized updates carry data. */
	TArray<FTransform> PoseAt(double T)
	{
		TArray<FTransform> Bones;
		Bones.Add(FTransform(FQuat::Identity, FVector(10.0 + 0.3 * T, 0.0, 90.0), FVector::OneVector));
		Bones.Add(FTransform(FQuat::Identity, FVector(0.0, 0.0, 20.0 + 0.002 * T), FVector::OneVector));
		Bones.Add(FTransform(FQuat::Identity, FVector(0.0, 0.001 * T, 15.0), FVector::OneVector));
		return Bones;
	}

	FO3DSenderEncodingSettings QuantizedSettings()
	{
		FO3DSenderEncodingSettings Settings;
		Settings.Mode = EO3DSenderEncodingMode::Quantized;
		Settings.QuantizationDeltaThreshold = 1.0e-6f;
		Settings.FullSyncIntervalSeconds = 1.0f;
		return Settings;
	}

	/** Takes a frame from the probe's pool and fills it as sampling would. Null when none is free. */
	TUniquePtr<FO3DSPoseFrame> MakeProbeFrame(FO3DSenderPipelineProbe& Probe, const FString& Subject,
		const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, double CaptureTimeSec)
	{
		TUniquePtr<FO3DSPoseFrame> Frame = Probe.AcquireFrame();
		if (Frame.IsValid())
		{
			Frame->Subject = Subject;
			Frame->Descriptor = Descriptor;
			Frame->CaptureTimeSec = CaptureTimeSec;
			Frame->Encoding = QuantizedSettings();
			Frame->BoneLocalTransforms = PoseAt(CaptureTimeSec);
		}
		return Frame;
	}

	bool ParsePacket(O3DS::SubjectList& Receiver, const TArray<uint8>& Packet, std::vector<O3DS::ParsedSubjectInfo>* OutTouched = nullptr)
	{
		return Receiver.Parse(reinterpret_cast<const char*>(Packet.GetData()), (size_t)Packet.Num(), nullptr, true, OutTouched);
	}

	/** One payload as the transport received it. */
	struct FRecordedPayload
	{
		TArray<uint8> Bytes;
		FString Subject;
		double CaptureTimeSec = 0.0;
		bool bFullSync = false;
		bool bOnGameThread = false;
	};

	/**
	 * A transport whose SendSerialized can be held: BlockNextSend makes the next call signal
	 * Entered and wait for ReleaseBlockedSend (capped, so a failing test cannot hang). It can also
	 * refuse the next N payloads with DroppedBackpressure. Records everything it is given.
	 */
	class FScriptedSender final : public IOpen3DSender
	{
	public:
		virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override
		{
			(void)Config;
			return FO3DTransportResult::Ok();
		}
		virtual FO3DTransportResult Start() override { return FO3DTransportResult::Ok(); }
		virtual void Stop() override {}
		virtual void Tick(float DeltaSeconds) override { (void)DeltaSeconds; }
		virtual FO3DTransportStats GetStats() const override { return FO3DTransportStats(); }
		virtual FO3DTransportCapabilities GetCapabilities() const override { return FO3DTransportCapabilities(); }
		virtual EO3DConnectionState GetConnectionState() const override { return EO3DConnectionState::Connected; }
		virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { (void)Callback; }

		virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override
		{
			if (bBlockNext.exchange(false))
			{
				Entered->Trigger();
				ReleaseEvent->Wait(15000);
			}

			FRecordedPayload Record;
			Record.Bytes = MoveTemp(Payload.Bytes);
			Record.Subject = Payload.Subject;
			Record.CaptureTimeSec = Payload.CaptureTimeSec;
			Record.bFullSync = Payload.bFullSync;
			Record.bOnGameThread = IsInGameThread();

			FScopeLock Guard(&Mutex);
			Recorded.Add(MoveTemp(Record));
			if (RefuseNext > 0)
			{
				--RefuseNext;
				return EO3DSendResult::DroppedBackpressure;
			}
			return EO3DSendResult::Queued;
		}

		void BlockNextSend()
		{
			Entered->Reset();
			ReleaseEvent->Reset();
			bBlockNext.store(true);
		}

		/** Waits until a blocked send has started. */
		bool WaitUntilEntered(double TimeoutSeconds)
		{
			return Entered->Wait((uint32)(TimeoutSeconds * 1000.0));
		}

		void ReleaseBlockedSend()
		{
			bBlockNext.store(false);
			ReleaseEvent->Trigger();
		}

		void SetRefuseNext(int32 Count)
		{
			FScopeLock Guard(&Mutex);
			RefuseNext = Count;
		}

		virtual void SetPeerJoinedCallback(FO3DPeerJoinedCallback Callback) override
		{
			FScopeLock Guard(&Mutex);
			PeerJoined = MoveTemp(Callback);
		}

		/** What a transport does when a receiver connects (ADR 0005 (vi)). False when no callback is set. */
		bool FirePeerJoined()
		{
			FO3DPeerJoinedCallback Callback;
			{
				FScopeLock Guard(&Mutex);
				Callback = PeerJoined;
			}
			if (!Callback)
			{
				return false;
			}
			Callback();
			return true;
		}

		TArray<FRecordedPayload> GetRecorded() const
		{
			FScopeLock Guard(&Mutex);
			return Recorded;
		}

	private:
		mutable FCriticalSection Mutex;
		TArray<FRecordedPayload> Recorded;
		int32 RefuseNext = 0;
		FO3DPeerJoinedCallback PeerJoined;
		std::atomic<bool> bBlockNext{ false };
		FEventRef Entered{ EEventMode::ManualReset };
		FEventRef ReleaseEvent{ EEventMode::ManualReset };
	};

	/** Sets o3d.Sender.AsyncPipeline for the scope and restores the previous value. */
	class FAsyncPipelineCVarScope
	{
	public:
		explicit FAsyncPipelineCVarScope(bool bAsync)
			: Variable(IConsoleManager::Get().FindConsoleVariable(TEXT("o3d.Sender.AsyncPipeline")))
		{
			if (Variable != nullptr)
			{
				Previous = Variable->GetInt();
				Variable->Set(bAsync ? 1 : 0, ECVF_SetByCode);
			}
		}

		~FAsyncPipelineCVarScope()
		{
			if (Variable != nullptr)
			{
				Variable->Set(Previous, ECVF_SetByCode);
			}
		}

		FAsyncPipelineCVarScope(const FAsyncPipelineCVarScope&) = delete;
		FAsyncPipelineCVarScope& operator=(const FAsyncPipelineCVarScope&) = delete;

		bool IsValid() const { return Variable != nullptr; }

	private:
		IConsoleVariable* Variable = nullptr;
		int32 Previous = 1;
	};

	/** A capturing, control-only component on a fake transport (no mesh: frames come from SubmitSampledFrame). */
	UO3DSenderComponent* MakeCapturingComponent(FName TransportName, const FString& Subject)
	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		Component->bAutoCreateTransport = true;
		Component->bAllowControlOnly = true;
		Component->bEnableQuantization = true;
		Component->QuantizationDeltaThreshold = 1.0e-6f;
		Component->SetTransportName(TransportName);
		Component->SubjectName = Subject;
		return Component;
	}

	const TCHAR* ModeLabel(bool bAsync)
	{
		return bAsync ? TEXT("async") : TEXT("sync");
	}
}

// ADR 0008 Verification: a slow transport. The worker is held inside SendSerialized while ten frames
// arrive; the queue never holds more than its depth, the oldest frames are the ones dropped, and the
// frames that are sent are numbered without a gap and decode as one unbroken update chain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineSlowTransportTest, "Open3DBroadcast.Sender.Pipeline.SlowTransportDropsOldest", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineSlowTransportTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Subject = TEXT("Slow");
	const int32 Depth = 2;

	FO3DSenderPipelineProbe Probe;
	Probe.SetDepth(Depth);
	Probe.Start(true);
	const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
	Probe.AttachSender(Transport);

	// Frame 1 reaches the worker, which then stays inside SendSerialized.
	Transport->BlockNextSend();
	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 100.0));
	if (!TestTrue(TEXT("The worker entered the blocked send"), Transport->WaitUntilEntered(WaitTimeoutSeconds)))
	{
		Transport->ReleaseBlockedSend();
		Probe.WaitForIdle(WaitTimeoutSeconds);
		return false;
	}

	// Frames 2..10 arrive while the transport is slow.
	for (int32 Index = 2; Index <= 10; ++Index)
	{
		TUniquePtr<FO3DSPoseFrame> Frame = MakeProbeFrame(Probe, Subject, Descriptor, 100.0 + (Index - 1) / 60.0);
		TestTrue(*FString::Printf(TEXT("Frame %d: a pooled frame is free"), Index), Frame.IsValid());
		Probe.SubmitFrame(MoveTemp(Frame));
		TestTrue(*FString::Printf(TEXT("Frame %d: never more than the depth waiting"), Index), Probe.GetStats().QueuedFrames <= Depth);
	}

	const FO3DSenderPipelineStats Blocked = Probe.GetStats();
	TestEqual(TEXT("Depth frames wait"), Blocked.QueuedFrames, Depth);
	TestEqual(TEXT("The seven oldest waiting frames were dropped"), Blocked.FramesDropped, (uint64)7);
	TestTrue(TEXT("The queue never held more than its depth"), Blocked.MaxQueuedFrames <= Depth);

	// A2a deviation 6: the stats dump reads every serializer under its lock while the worker runs.
	FO3DSenderSerializer::DumpAllStats();

	Transport->ReleaseBlockedSend();
	if (!TestTrue(TEXT("The worker drained"), Probe.WaitForIdle(WaitTimeoutSeconds)))
	{
		return false;
	}

	const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
	if (!TestEqual(TEXT("Three payloads: the blocked frame and the two newest"), Sent.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("Payload 1 is frame 1"), Sent[0].CaptureTimeSec, 100.0);
	TestEqual(TEXT("Payload 2 is frame 9"), Sent[1].CaptureTimeSec, 100.0 + 8 / 60.0);
	TestEqual(TEXT("Payload 3 is frame 10"), Sent[2].CaptureTimeSec, 100.0 + 9 / 60.0);
	TestTrue(TEXT("Only the first payload is a full sync"), Sent[0].bFullSync && !Sent[1].bFullSync && !Sent[2].bFullSync);
	for (const FRecordedPayload& Payload : Sent)
	{
		TestFalse(TEXT("Sent from the worker, not the game thread"), Payload.bOnGameThread);
		TestEqual(TEXT("Payload subject"), Payload.Subject, Subject);
	}

	const FO3DSenderPipelineStats Done = Probe.GetStats();
	TestEqual(TEXT("Every submitted frame is counted"), Done.FramesSubmitted, (uint64)10);
	TestEqual(TEXT("Processed = submitted - dropped"), Done.FramesProcessed, (uint64)3);
	TestEqual(TEXT("Every processed frame went to the transport"), Done.PayloadsHandedToTransport, (uint64)3);
	TestEqual(TEXT("Send sequence has no gap"), Done.LastSendSequence, Done.PayloadsHandedToTransport);
	TestEqual(TEXT("All accepted"), Done.PayloadsAccepted, (uint64)3);
	TestEqual(TEXT("Nothing waits"), Done.QueuedFrames, 0);
	TestEqual(TEXT("The serializer serialized exactly the sent frames"), Probe.GetSerializer().GetSubjectStats(Subject).FramesSerialized, (uint64)3);

	// No serialized frame was lost, so a receiver applies every update and ends on frame 10's pose.
	O3DS::SubjectList Receiver;
	for (int32 Index = 0; Index < Sent.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("Payload %d parses"), Index), ParsePacket(Receiver, Sent[Index].Bytes));
	}
	O3DS::Subject* Parsed = Receiver.findSubject(std::string(TCHAR_TO_UTF8(*Subject)));
	if (TestNotNull(TEXT("Subject decoded"), Parsed) && TestEqual(TEXT("Three bones"), (int32)Parsed->mTransforms.size(), 3))
	{
		const TArray<FTransform> Want = PoseAt(100.0 + 9 / 60.0);
		for (int32 Bone = 0; Bone < 3; ++Bone)
		{
			const O3DS::Vector3d& Got = Parsed->mTransforms[Bone]->translation.value;
			const FVector WantT = Want[Bone].GetTranslation();
			TestEqual(*FString::Printf(TEXT("Bone %d X after the drops"), Bone), Got.v[0], WantT.X, 1.0e-3);
			TestEqual(*FString::Printf(TEXT("Bone %d Y after the drops"), Bone), Got.v[1], WantT.Y, 1.0e-3);
			TestEqual(*FString::Printf(TEXT("Bone %d Z after the drops"), Bone), Got.v[2], WantT.Z, 1.0e-3);
		}
	}

	Probe.DetachSender();
	return true;
}

// ADR 0008 item 2 / ADR 0007 item 3: a full sync the transport refuses with DroppedBackpressure is
// followed by another full sync, in both modes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineRefusedFullSyncTest, "Open3DBroadcast.Sender.Pipeline.RefusedFullSyncIsSentAgain", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineRefusedFullSyncTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Subject = TEXT("Refused");

	for (bool bAsync : { false, true })
	{
		FO3DSenderPipelineProbe Probe;
		Probe.Start(bAsync);
		const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
		Transport->SetRefuseNext(1);
		Probe.AttachSender(Transport);

		for (int32 Index = 0; Index < 3; ++Index)
		{
			Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 10.0 + Index / 60.0));
			// One at a time, so no frame is dropped from the queue.
			TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
		}

		const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
		if (TestEqual(*FString::Printf(TEXT("%s: three payloads"), ModeLabel(bAsync)), Sent.Num(), 3))
		{
			TestTrue(*FString::Printf(TEXT("%s: refused full sync, then a full sync, then an update"), ModeLabel(bAsync)),
				Sent[0].bFullSync && Sent[1].bFullSync && !Sent[2].bFullSync);
			TestTrue(*FString::Printf(TEXT("%s: payload sent on the expected thread"), ModeLabel(bAsync)), Sent[0].bOnGameThread == !bAsync);
		}
		TestEqual(*FString::Printf(TEXT("%s: two full syncs"), ModeLabel(bAsync)), Probe.GetSerializer().GetSubjectStats(Subject).FullSyncsSent, (uint64)2);
		TestEqual(*FString::Printf(TEXT("%s: one refusal"), ModeLabel(bAsync)), Probe.GetStats().PayloadsRefused, (uint64)1);
		Probe.DetachSender();
	}
	return true;
}

// ADR 0005 (ix): a residual update the transport refuses leaves a sequence gap, and a receiver
// holds the subject until the next full Subject, so the next frame is one. A refused quantized
// update stays applicable across the gap and changes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineRefusedResidualTest, "Open3DBroadcast.Sender.Pipeline.RefusedResidualUpdateForcesFullSync", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineRefusedResidualTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();

	for (const EO3DSenderEncodingMode Mode : { EO3DSenderEncodingMode::Residual, EO3DSenderEncodingMode::Quantized })
	{
		const bool bResidual = (Mode == EO3DSenderEncodingMode::Residual);
		const TCHAR* ModeName = bResidual ? TEXT("Residual") : TEXT("Quantized");
		const FString Subject = ModeName;
		FO3DSenderPipelineProbe Probe;
		Probe.Start(false);
		const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
		Probe.AttachSender(Transport);

		for (int32 Index = 0; Index < 4; ++Index)
		{
			// Frame 1, the first update, is refused.
			Transport->SetRefuseNext(Index == 1 ? 1 : 0);
			TUniquePtr<FO3DSPoseFrame> Frame = MakeProbeFrame(Probe, Subject, Descriptor, 10.0 + Index / 60.0);
			if (Frame.IsValid())
			{
				Frame->Encoding.Mode = Mode;
			}
			Probe.SubmitFrame(MoveTemp(Frame));
			TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
		}

		const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
		if (TestEqual(*FString::Printf(TEXT("%s: four payloads"), ModeName), Sent.Num(), 4))
		{
			TestTrue(*FString::Printf(TEXT("%s: a full sync, then the refused update"), ModeName), Sent[0].bFullSync && !Sent[1].bFullSync);
			TestEqual(*FString::Printf(TEXT("%s: the frame after the refused update is a full sync only in residual mode"), ModeName), Sent[2].bFullSync, bResidual);
			TestFalse(*FString::Printf(TEXT("%s: then updates again"), ModeName), Sent[3].bFullSync);
		}
		Probe.DetachSender();
	}
	return true;
}

// ADR 0005 (ix): a full sync serialized while no transport is attached named a tx_seq no receiver
// has, and later updates would carry it as ref_seq, so the first frame after attaching is a full
// sync, in quantized mode too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineUndeliveredFullSyncTest, "Open3DBroadcast.Sender.Pipeline.UndeliveredFullSyncIsSentAgain", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineUndeliveredFullSyncTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Subject = TEXT("Unattached");

	FO3DSenderPipelineProbe Probe;
	Probe.Start(false);
	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 10.0));
	TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
	TestEqual(TEXT("The first full sync had no transport"), Probe.GetStats().PayloadsWithoutTransport, (uint64)1);

	const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
	Probe.AttachSender(Transport);
	for (int32 Index = 1; Index < 3; ++Index)
	{
		Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 10.0 + Index / 60.0));
		TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
	}
	const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
	if (TestEqual(TEXT("Two payloads after attaching"), Sent.Num(), 2))
	{
		TestTrue(TEXT("The first one is a full sync, then an update"), Sent[0].bFullSync && !Sent[1].bFullSync);
	}
	Probe.DetachSender();
	return true;
}

// ADR 0005 (vi): when the transport reports a new peer, the next frame of every subject is a full
// sync, so the new receiver does not wait for the periodic one. Detaching clears the callback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelinePeerJoinedTest, "Open3DBroadcast.Sender.Pipeline.PeerJoinedForcesFullSync", O3DB_TEST_FLAGS)
bool FO3DSenderPipelinePeerJoinedTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();

	for (bool bAsync : { false, true })
	{
		FO3DSenderPipelineProbe Probe;
		Probe.Start(bAsync);
		const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
		Probe.AttachSender(Transport);

		auto Send = [this, &Probe, &Descriptor](const FString& Subject, int32 Index)
		{
			Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 10.0 + Index / 60.0));
			TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
		};
		// Two subjects, each a full sync and then an update.
		Send(TEXT("A"), 0);
		Send(TEXT("B"), 0);
		Send(TEXT("A"), 1);
		Send(TEXT("B"), 1);
		// A receiver joins (on any thread; here the test's).
		TestTrue(*FString::Printf(TEXT("%s: the pipeline set a callback"), ModeLabel(bAsync)), Transport->FirePeerJoined());
		Send(TEXT("A"), 2);
		Send(TEXT("B"), 2);
		Send(TEXT("A"), 3);
		Send(TEXT("B"), 3);

		const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
		if (TestEqual(*FString::Printf(TEXT("%s: eight payloads"), ModeLabel(bAsync)), Sent.Num(), 8))
		{
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const bool bExpectFull = (Index < 2) || (Index == 4 || Index == 5);
				TestEqual(*FString::Printf(TEXT("%s: payload %d (%s) full sync"), ModeLabel(bAsync), Index, *Sent[Index].Subject), Sent[Index].bFullSync, bExpectFull);
			}
		}
		Probe.DetachSender();
		TestFalse(*FString::Printf(TEXT("%s: detaching clears the callback"), ModeLabel(bAsync)), Transport->FirePeerJoined());
	}
	return true;
}

// CORE-12: o3d.Sender.Capture.Start records every serialized payload, verbatim, in the core
// capture format that apps/QuantEval reads; nothing is recorded once it stops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderCaptureTest, "Open3DBroadcast.Sender.Capture.RecordsEveryPayload", O3DB_TEST_FLAGS)
bool FO3DSenderCaptureTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("O3DCaptures"), FString::Printf(TEXT("Test-%s.o3dscap"), *FGuid::NewGuid().ToString()));

	FO3DSenderPipelineProbe Probe;
	Probe.Start(false);
	const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
	Probe.AttachSender(Transport);

	if (!TestTrue(TEXT("Capture starts"), FO3DSenderCapture::Start(Path)))
	{
		return false;
	}
	TestTrue(TEXT("Active"), FO3DSenderCapture::IsActive());
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Probe.SubmitFrame(MakeProbeFrame(Probe, TEXT("Captured"), Descriptor, 30.0 + Index / 60.0));
		TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
	}
	TestEqual(TEXT("Five payloads written"), FO3DSenderCapture::Stop(), (int64)5);
	TestFalse(TEXT("Stopped"), FO3DSenderCapture::IsActive());
	Probe.SubmitFrame(MakeProbeFrame(Probe, TEXT("Captured"), Descriptor, 31.0));
	TestTrue(TEXT("Drained"), Probe.WaitForIdle(WaitTimeoutSeconds));
	Probe.DetachSender();

	// The file holds exactly what the transport was given, in order.
	const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
	std::ifstream In(TCHAR_TO_UTF8(*Path), std::ios::binary);
	O3DS::CaptureHeaderInfo Header;
	TestTrue(TEXT("Header reads"), (bool)In && O3DS::ReadCaptureHeader(In, Header));
	int32 Index = 0;
	O3DS::CaptureRecord Record;
	while (O3DS::ReadCaptureRecord(In, Record))
	{
		if (Index < 5 && Index < Sent.Num())
		{
			TestTrue(*FString::Printf(TEXT("Record %d matches the payload"), Index),
				Record.wire_bytes.size() == (size_t)Sent[Index].Bytes.Num()
				&& FMemory::Memcmp(Record.wire_bytes.data(), Sent[Index].Bytes.GetData(), Sent[Index].Bytes.Num()) == 0);
		}
		++Index;
	}
	TestEqual(TEXT("Five records, none after Stop"), Index, 5);
	In.close();
	IFileManager::Get().Delete(*Path);
	return true;
}

// ADR 0008 item 10: Stop discards the frames still waiting (it does not wait for them or the
// network), and control items keep their place: the first frame after Stop and Start is a full sync.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineStopDiscardsTest, "Open3DBroadcast.Sender.Pipeline.StopDiscardsQueuedFrames", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineStopDiscardsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Subject = TEXT("Stopping");

	FO3DSenderPipelineProbe Probe;
	Probe.SetDepth(2);
	Probe.Start(true);
	const TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
	Probe.AttachSender(Transport);

	Transport->BlockNextSend();
	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 1.0));
	if (!TestTrue(TEXT("The worker entered the blocked send"), Transport->WaitUntilEntered(WaitTimeoutSeconds)))
	{
		Transport->ReleaseBlockedSend();
		Probe.WaitForIdle(WaitTimeoutSeconds);
		return false;
	}

	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 2.0));
	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 3.0));
	Probe.Stop();
	TestEqual(TEXT("Stop discarded both waiting frames"), Probe.GetStats().FramesDiscardedOnStop, (uint64)2);
	TestEqual(TEXT("Nothing waits after Stop"), Probe.GetStats().QueuedFrames, 0);
	TestFalse(TEXT("Stop returned while the worker is still inside the send"), Probe.IsIdle());

	Probe.Start(true);
	Probe.SubmitFrame(MakeProbeFrame(Probe, Subject, Descriptor, 4.0));

	Transport->ReleaseBlockedSend();
	if (!TestTrue(TEXT("The worker drained"), Probe.WaitForIdle(WaitTimeoutSeconds)))
	{
		return false;
	}

	const TArray<FRecordedPayload> Sent = Transport->GetRecorded();
	if (TestEqual(TEXT("The in-flight frame and the frame after Start"), Sent.Num(), 2))
	{
		TestEqual(TEXT("First payload: frame 1"), Sent[0].CaptureTimeSec, 1.0);
		TestEqual(TEXT("Second payload: frame 4"), Sent[1].CaptureTimeSec, 4.0);
		TestTrue(TEXT("Both are full syncs (Stop cleared the serializer before frame 4)"), Sent[0].bFullSync && Sent[1].bFullSync);

		O3DS::SubjectList Receiver;
		std::vector<O3DS::ParsedSubjectInfo> Touched;
		TestTrue(TEXT("A fresh receiver decodes the restarted stream"), ParsePacket(Receiver, Sent[1].Bytes, &Touched));
		TestTrue(TEXT("It carries the descriptor"), Touched.size() == 1 && Touched[0].fullDescriptor);
	}
	Probe.DetachSender();
	return true;
}

// ADR 0005 acceptance under the pipeline: Stop/Start and a rename while frames are flowing (no waits
// between frames, so the queue drops some) give a full sync with the right names and parents first
// for every subject and every session, in both modes. Also: OnSerializedFrame fires on the worker
// when asynchronous and on the game thread when not, and never after StopCapture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineStopStartRenameTest, "Open3DBroadcast.Sender.Pipeline.StopStartAndRenameUnderLoad", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineStopStartRenameTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	// Control-only start without a mesh (HANDOFF pitfall 22).
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 0);

	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();

	for (bool bAsync : { false, true })
	{
		FAsyncPipelineCVarScope CVar(bAsync);
		if (!TestTrue(TEXT("o3d.Sender.AsyncPipeline exists"), CVar.IsValid()))
		{
			return false;
		}
		const FString Mode = ModeLabel(bAsync);

		FO3DFakeTransportScope Scope;
		UO3DSenderComponent* Component = MakeCapturingComponent(Scope.GetName(), TEXT("Alpha"));

		std::atomic<int32> ListenerCalls{ 0 };
		std::atomic<int32> ListenerCallsOnGameThread{ 0 };
		const FDelegateHandle ListenerHandle = Component->OnSerializedFrame.AddLambda([&ListenerCalls, &ListenerCallsOnGameThread](const FString&, const TArray<uint8>&, double)
		{
			ListenerCalls.fetch_add(1);
			if (IsInGameThread())
			{
				ListenerCallsOnGameThread.fetch_add(1);
			}
		});

		double Time = 300.0;
		int32 TotalPayloads = 0;
		for (int32 Cycle = 0; Cycle < 3; ++Cycle)
		{
			const FString Context = FString::Printf(TEXT("%s cycle %d"), *Mode, Cycle);
			const FString RenamedSubject = FString::Printf(TEXT("Beta%d"), Cycle);
			Component->SubjectName = TEXT("Alpha");
			Component->StartCapture();
			if (!TestTrue(*FString::Printf(TEXT("%s: capturing"), *Context), Component->IsCapturing()))
			{
				break;
			}
			FO3DSenderComponentTestAccess::SetDescriptor(*Component, *Descriptor);
			const TSharedPtr<FO3DFakeSender> Fake = Scope.GetLastSender();
			if (!TestTrue(*FString::Printf(TEXT("%s: fake transport started"), *Context), Fake.IsValid()))
			{
				Component->StopCapture();
				break;
			}

			for (int32 Index = 0; Index < 30; ++Index)
			{
				if (Index == 15)
				{
					Component->SubjectName = RenamedSubject;
				}
				TestTrue(*FString::Printf(TEXT("%s frame %d submitted"), *Context, Index), FO3DSenderComponentTestAccess::SubmitSampledFrame(*Component, PoseAt(Time), Time));
				Time += 1.0 / 60.0;
			}
			TestTrue(*FString::Printf(TEXT("%s: drained"), *Context), FO3DSenderComponentTestAccess::WaitForPipelineIdle(*Component, WaitTimeoutSeconds));

			const TArray<TArray<uint8>> Payloads = Fake->GetRecordedPayloads();
			TotalPayloads += Payloads.Num();
			TestTrue(*FString::Printf(TEXT("%s: payloads for both names"), *Context), Payloads.Num() >= 2);

			// One receiver for the session; each name's first payload must be a full descriptor.
			O3DS::SubjectList Receiver;
			TSet<FString> SeenNames;
			bool bRenamedSeen = false;
			for (int32 Packet = 0; Packet < Payloads.Num(); ++Packet)
			{
				std::vector<O3DS::ParsedSubjectInfo> Touched;
				if (!TestTrue(*FString::Printf(TEXT("%s packet %d parses"), *Context, Packet), ParsePacket(Receiver, Payloads[Packet], &Touched)))
				{
					continue;
				}
				if (!TestEqual(*FString::Printf(TEXT("%s packet %d: one subject"), *Context, Packet), (int32)Touched.size(), 1))
				{
					continue;
				}
				const FString Name = UTF8_TO_TCHAR(Touched[0].name.c_str());
				if (!SeenNames.Contains(Name))
				{
					TestTrue(*FString::Printf(TEXT("%s: first payload of '%s' is a full descriptor"), *Context, *Name), Touched[0].fullDescriptor);
					SeenNames.Add(Name);
				}
				if (Name == RenamedSubject)
				{
					bRenamedSeen = true;
				}
				else
				{
					TestFalse(*FString::Printf(TEXT("%s packet %d: no old-name payload after the new name"), *Context, Packet), bRenamedSeen);
					TestEqual(*FString::Printf(TEXT("%s packet %d: old name"), *Context, Packet), Name, FString(TEXT("Alpha")));
				}

				O3DS::Subject* Parsed = Receiver.findSubject(Touched[0].name);
				if (TestNotNull(*FString::Printf(TEXT("%s packet %d: subject decoded"), *Context, Packet), Parsed)
					&& TestEqual(*FString::Printf(TEXT("%s packet %d: bone count"), *Context, Packet), (int32)Parsed->mTransforms.size(), Descriptor->BoneNames.Num()))
				{
					for (int32 Bone = 0; Bone < Descriptor->BoneNames.Num(); ++Bone)
					{
						TestEqual(*FString::Printf(TEXT("%s packet %d bone %d name"), *Context, Packet, Bone), FString(UTF8_TO_TCHAR(Parsed->mTransforms[Bone]->mName.c_str())), Descriptor->BoneNames[Bone].ToString());
						TestEqual(*FString::Printf(TEXT("%s packet %d bone %d parent"), *Context, Packet, Bone), Parsed->mTransforms[Bone]->mParentId, Descriptor->ParentIndices[Bone]);
					}
				}
			}
			TestTrue(*FString::Printf(TEXT("%s: the renamed subject was sent"), *Context), bRenamedSeen);

			const FO3DSenderPipelineStats Stats = Component->GetPipelineStats();
			TestTrue(*FString::Printf(TEXT("%s: mode"), *Context), Stats.bAsync == bAsync);
			TestTrue(*FString::Printf(TEXT("%s: never more than the depth waiting"), *Context), Stats.MaxQueuedFrames <= FO3DSenderPipelineProbe::GetMaxDepth());

			Component->StopCapture();
		}

		const int32 CallsAtStop = ListenerCalls.load();
		TestEqual(*FString::Printf(TEXT("%s: OnSerializedFrame once per payload"), *Mode), CallsAtStop, TotalPayloads);
		TestEqual(*FString::Printf(TEXT("%s: OnSerializedFrame thread"), *Mode), ListenerCallsOnGameThread.load(), bAsync ? 0 : CallsAtStop);
		TestTrue(*FString::Printf(TEXT("%s: nothing left after StopCapture"), *Mode), FO3DSenderComponentTestAccess::WaitForPipelineIdle(*Component, WaitTimeoutSeconds));
		TestEqual(*FString::Printf(TEXT("%s: no OnSerializedFrame after StopCapture"), *Mode), ListenerCalls.load(), CallsAtStop);
		Component->OnSerializedFrame.Remove(ListenerHandle);
	}
	return true;
}

// ADR 0008 Verification: changing the quantization ranges mid-capture (a Blueprint-style property
// write, no restart) forces exactly one full sync; a delta threshold change forces none. Both modes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineQuantizationChangeTest, "Open3DBroadcast.Sender.Pipeline.QuantizationChangeForcesOneFullSync", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineQuantizationChangeTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 0);
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const FString Subject = TEXT("Quant");

	for (bool bAsync : { false, true })
	{
		FAsyncPipelineCVarScope CVar(bAsync);
		const FString Mode = ModeLabel(bAsync);
		FO3DFakeTransportScope Scope;
		UO3DSenderComponent* Component = MakeCapturingComponent(Scope.GetName(), Subject);
		Component->StartCapture();
		if (!TestTrue(*FString::Printf(TEXT("%s: capturing"), *Mode), Component->IsCapturing()))
		{
			continue;
		}
		FO3DSenderComponentTestAccess::SetDescriptor(*Component, *Descriptor);

		// 30 frames in 0.5 s of capture time: no periodic full sync (1 s) falls inside.
		double Time = 500.0;
		auto SendFrames = [this, Component, &Time](int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				TestTrue(TEXT("Frame submitted"), FO3DSenderComponentTestAccess::SubmitSampledFrame(*Component, PoseAt(Time), Time));
				Time += 1.0 / 60.0;
			}
			TestTrue(TEXT("Drained"), FO3DSenderComponentTestAccess::WaitForPipelineIdle(*Component, WaitTimeoutSeconds));
			return Component->GetSerializer().GetSubjectStats(TEXT("Quant")).FullSyncsSent;
		};

		TestEqual(*FString::Printf(TEXT("%s: first full sync"), *Mode), SendFrames(10), (uint64)1);

		Component->QuantizationByteRange = 0.02f;
		TestEqual(*FString::Printf(TEXT("%s: range change forces exactly one full sync"), *Mode), SendFrames(10), (uint64)2);

		Component->QuantizationDeltaThreshold = 1.0e-4f;
		TestEqual(*FString::Printf(TEXT("%s: delta threshold change forces none"), *Mode), SendFrames(10), (uint64)2);

		Component->StopCapture();
	}
	return true;
}

// ADR 0005 (iii): residual coding is used only on a transport that delivers reliably and in order;
// on any other the sender sends what it would with residual coding off (full snapshots, or
// quantized updates only when quantization was enabled itself; CORE-12) and warns once. The details panel shows the same
// warning from the configured transport, before anything starts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderResidualFallbackTest, "Open3DBroadcast.Sender.Encoding.ResidualFallsBackOnUnreliableTransports", O3DB_TEST_FLAGS)
bool FO3DSenderResidualFallbackTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;

	// The decision and the text.
	using EMode = EO3DSenderEncodingMode;
	TestTrue(TEXT("Residual on ReliableOrdered"), UO3DSenderComponent::ResolveEncodingMode(true, false, EO3DDeliveryGuarantee::ReliableOrdered) == EMode::Residual);
	TestTrue(TEXT("Residual takes precedence over quantization"), UO3DSenderComponent::ResolveEncodingMode(true, true, EO3DDeliveryGuarantee::ReliableOrdered) == EMode::Residual);
	TestTrue(TEXT("Full snapshots on Unreliable"), UO3DSenderComponent::ResolveEncodingMode(true, false, EO3DDeliveryGuarantee::Unreliable) == EMode::Legacy);
	TestTrue(TEXT("Full snapshots on Unknown"), UO3DSenderComponent::ResolveEncodingMode(true, false, EO3DDeliveryGuarantee::Unknown) == EMode::Legacy);
	TestTrue(TEXT("Quantized on Unreliable only when quantization is enabled"), UO3DSenderComponent::ResolveEncodingMode(true, true, EO3DDeliveryGuarantee::Unreliable) == EMode::Quantized);
	TestTrue(TEXT("Quantization alone is unaffected"), UO3DSenderComponent::ResolveEncodingMode(false, true, EO3DDeliveryGuarantee::Unreliable) == EMode::Quantized);
	TestTrue(TEXT("Legacy is unaffected"), UO3DSenderComponent::ResolveEncodingMode(false, false, EO3DDeliveryGuarantee::Unreliable) == EMode::Legacy);
	TestTrue(TEXT("No warning on ReliableOrdered"), UO3DSenderComponent::GetResidualFallbackWarning(TEXT("TCP"), EO3DDeliveryGuarantee::ReliableOrdered).IsEmpty());
	TestTrue(TEXT("The warning names the transport"), UO3DSenderComponent::GetResidualFallbackWarning(TEXT("UDP"), EO3DDeliveryGuarantee::Unreliable).ToString().Contains(TEXT("'UDP'")));

	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedErrorFlags::Contains, 0);
	// Logged once per capture for each unreliable run, not once per frame.
	AddExpectedMessage(TEXT("Residual coding needs a transport that delivers reliably and in order"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2, false);

	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	struct FRun { bool bUnreliable; bool bQuantization; const TCHAR* Name; };
	for (const FRun& Run : { FRun{ false, false, TEXT("reliable") }, FRun{ true, false, TEXT("unreliable") }, FRun{ true, true, TEXT("unreliable, quantization on") } })
	{
		const bool bUnreliable = Run.bUnreliable;
		const FString Context = Run.Name;
		FO3DFakeTransportScope Scope;
		UO3DSenderComponent* Component = MakeCapturingComponent(Scope.GetName(), TEXT("Fallback"));
		Component->bEnableQuantization = Run.bQuantization;
		Component->bEnableResidualCoding = true;
		Component->ResidualDeltaThreshold = 0.0f;
		if (bUnreliable)
		{
			Component->SetTransportOption(TEXT("fake.delivery"), TEXT("unreliable"));
		}

		// The details panel's view, from the registry and the configured options.
		TestTrue(*FString::Printf(TEXT("%s: configured guarantee"), *Context),
			Component->GetConfiguredDeliveryGuarantee() == (bUnreliable ? EO3DDeliveryGuarantee::Unreliable : EO3DDeliveryGuarantee::ReliableOrdered));
		TestEqual(*FString::Printf(TEXT("%s: details panel warning"), *Context), !Component->GetConfiguredResidualFallbackWarning().IsEmpty(), bUnreliable);

		TArray<TArray<uint8>> Payloads;
		const FDelegateHandle Listener = Component->OnSerializedFrame.AddLambda([&Payloads](const FString&, const TArray<uint8>& Bytes, double)
		{
			Payloads.Add(Bytes);
		});
		Component->StartCapture();
		if (!TestTrue(*FString::Printf(TEXT("%s: capturing"), *Context), Component->IsCapturing()))
		{
			Component->OnSerializedFrame.Remove(Listener);
			continue;
		}
		FO3DSenderComponentTestAccess::SetDescriptor(*Component, *Descriptor);
		double Time = 700.0;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			TestTrue(TEXT("Frame submitted"), FO3DSenderComponentTestAccess::SubmitSampledFrame(*Component, PoseAt(Time), Time));
			TestTrue(TEXT("Drained"), FO3DSenderComponentTestAccess::WaitForPipelineIdle(*Component, WaitTimeoutSeconds));
			Time += 1.0 / 60.0;
		}
		Component->StopCapture();
		Component->OnSerializedFrame.Remove(Listener);

		// Reliable: residual updates (predictor_id != 0). Unreliable: full snapshots only, or
		// quantized (non-residual) updates when quantization was enabled itself.
		int32 Updates = 0;
		int32 ResidualUpdates = 0;
		for (const TArray<uint8>& Payload : Payloads)
		{
			const O3DS::Data::SubjectList* List = O3DS::Data::GetSubjectList(Payload.GetData() + 8);
			if (List->updates() != nullptr && List->updates()->size() > 0)
			{
				++Updates;
				ResidualUpdates += (List->updates()->Get(0)->predictor_id() != 0) ? 1 : 0;
			}
		}
		TestEqual(*FString::Printf(TEXT("%s: payloads"), *Context), Payloads.Num(), 8);
		TestEqual(*FString::Printf(TEXT("%s: updates were sent"), *Context), Updates > 0, !bUnreliable || Run.bQuantization);
		TestEqual(*FString::Printf(TEXT("%s: residual updates"), *Context), ResidualUpdates, bUnreliable ? 0 : Updates);
	}
	return true;
}

// ADR 0008 item 3 and Verification ("destroying the component with a task in flight is clean"),
// pipeline level: 1,000 owners let go of their pipeline right after handing it frames; every tenth
// does it while the worker is held inside SendSerialized. Each task finishes on its own, the last
// reference is dropped on the worker, and every transport is released afterwards. Run under ASan
// outside the editor to check memory as the ADR asks; here the test checks lifetimes and counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineOwnerReleasedTest, "Open3DBroadcast.Sender.Pipeline.OwnerReleasedWithTaskInFlight", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineOwnerReleasedTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	const int32 Cycles = 1000;
	TArray<TWeakPtr<FScriptedSender>> Transports;
	Transports.Reserve(Cycles);

	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		const bool bHoldWorker = (Cycle % 10) == 0;
		TSharedRef<FScriptedSender> Transport = MakeShared<FScriptedSender>();
		Transports.Add(Transport);

		FO3DSenderPipelineProbe Probe;
		Probe.Start(true);
		Probe.AttachSender(Transport);
		if (bHoldWorker)
		{
			Transport->BlockNextSend();
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Probe.SubmitFrame(MakeProbeFrame(Probe, TEXT("Owner"), Descriptor, Cycle + Index / 60.0));
		}
		if (bHoldWorker && !TestTrue(*FString::Printf(TEXT("Cycle %d: the worker entered the blocked send"), Cycle), Transport->WaitUntilEntered(WaitTimeoutSeconds)))
		{
			Transport->ReleaseBlockedSend();
			break;
		}

		// The owner goes away without detaching anything, as a destroyed component's reference does.
		Probe.Release();
		if (bHoldWorker)
		{
			TestTrue(*FString::Printf(TEXT("Cycle %d: the task keeps the pipeline alive"), Cycle), Probe.IsPipelineAlive());
			Transport->ReleaseBlockedSend();
		}
	}

	TestTrue(TEXT("Every drain task finished"), FO3DSenderPipelineProbe::WaitForAllIdle(WaitTimeoutSeconds));
	int32 StillAlive = 0;
	for (const TWeakPtr<FScriptedSender>& Weak : Transports)
	{
		StillAlive += Weak.IsValid() ? 1 : 0;
	}
	TestEqual(TEXT("Every pipeline released its transport"), StillAlive, 0);
	return true;
}

// The same at component level: 200 capturing components on a fake transport are dropped right after
// frames were handed to their pipelines (half without StopCapture) and destroyed by garbage collection
// every 50 cycles. The destructor detaches the pipeline (waiting for at most one frame); tasks still
// running end on their own; no transport instance is left alive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPipelineComponentDestroyedTest, "Open3DBroadcast.Sender.Pipeline.ComponentDestroyedWithTaskInFlight", O3DB_TEST_FLAGS)
bool FO3DSenderPipelineComponentDestroyedTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineTests;
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 0);
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeThreeBoneDescriptor();
	FAsyncPipelineCVarScope CVar(true);
	const int32 Cycles = 200;
	const int32 CollectEvery = 50;

	TUniquePtr<FO3DFakeTransportScope> Scope = MakeUnique<FO3DFakeTransportScope>();
	const FName TransportName = Scope->GetName();
	TArray<TWeakObjectPtr<UO3DSenderComponent>> Components;

	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		UO3DSenderComponent* Component = MakeCapturingComponent(TransportName, TEXT("Destroyed"));
		Component->StartCapture();
		FO3DSenderComponentTestAccess::SetDescriptor(*Component, *Descriptor);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FO3DSenderComponentTestAccess::SubmitSampledFrame(*Component, PoseAt(Index / 60.0), Cycle + Index / 60.0);
		}
		if ((Cycle % 2) == 0)
		{
			Component->StopCapture();
		}
		Component->MarkAsGarbage();
		Components.Add(Component);

		if (((Cycle + 1) % CollectEvery) == 0)
		{
			CollectGarbage(GIsEditor ? RF_Standalone : RF_NoFlags);
		}
	}

	TestTrue(TEXT("Every drain task finished"), FO3DSenderPipelineProbe::WaitForAllIdle(WaitTimeoutSeconds));
	int32 StillAlive = 0;
	for (const TWeakObjectPtr<UO3DSenderComponent>& Weak : Components)
	{
		// IsValid(true): an object marked as garbage but not yet destroyed still counts.
		StillAlive += Weak.IsValid(true) ? 1 : 0;
	}
	TestEqual(TEXT("Every component was destroyed"), StillAlive, 0);
	TestEqual(TEXT("No transport instance is left alive"), FO3DTransportRegistry::Get().GetNumLiveInstances(TransportName), 0);
	Scope.Reset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
