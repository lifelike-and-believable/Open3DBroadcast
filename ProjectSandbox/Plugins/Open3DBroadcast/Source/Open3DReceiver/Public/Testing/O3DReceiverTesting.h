// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only white-box access to the receiver module (ADR 0006, WP-T2). Each accessor below is
// befriended by the class it opens up. They are header-only: FO3DReceiverSource and
// UO3DRemoteAudioComponent are exported, so these inline functions link from any module that
// depends on Open3DReceiver. Compiled out without dev automation tests.

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverSource.h"
#include "O3DRemoteAudioComponent.h"
#include "O3DRemoteControlComponent.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Sound/SoundWaveProcedural.h"

struct FO3DRemoteAudioComponentTestAccessor
{
	static void CallEnsureSoundWave(UO3DRemoteAudioComponent* Component, int32 NumChannels, int32 SampleRate)
	{
		Component->EnsureSoundWave(NumChannels, SampleRate);
	}

	static void CallOnAudioPcm16(UO3DRemoteAudioComponent* Component, const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8> PCM16Bytes)
	{
		Component->OnAudioPcm16(Meta, PCM16Bytes);
	}

	static bool CallMatchesFilter(const UO3DRemoteAudioComponent* Component, const FString& Subject, const FString& Stream)
	{
		return Component->MatchesFilter(Subject, Stream);
	}

	static USoundWaveProcedural* GetSoundWave(const UO3DRemoteAudioComponent* Component)
	{
		return Component->SoundWave;
	}

	static int32 GetCurrentChannels(const UO3DRemoteAudioComponent* Component)
	{
		return Component->CurrentChannels;
	}

	static int32 GetCurrentSampleRate(const UO3DRemoteAudioComponent* Component)
	{
		return Component->CurrentSampleRate;
	}
};

struct FO3DReceiverSourceTestAccessor
{
	static void SetActiveConfig(FO3DReceiverSource& Source, const FO3DTransportConfig& Config)
	{
		Source.ActiveConfig = Config;
	}

	static void CallFinalizeAudioMeta(const FO3DReceiverSource& Source, O3DS::FAudioFrameMeta& Meta)
	{
		Source.FinalizeAudioMeta(Meta);
	}

	static void SetSourceGuid(FO3DReceiverSource& Source, const FGuid& Guid)
	{
		Source.SourceGuid = Guid;
	}

	static TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> MakeAudioSink(const FO3DReceiverSource& Source)
	{
		return Source.MakeAudioSink();
	}

	// Control channel (ADR 0011, CTL-4).
	static bool StartTransport(FO3DReceiverSource& Source) { return Source.StartTransport(); }
	static void StopTransport(FO3DReceiverSource& Source) { Source.StopTransport(); }
	static uint64 GetControlPayloadsDroppedDisabled(const FO3DReceiverSource& Source) { return Source.ControlPayloadsDroppedDisabled; }
	static const O3DS::Control::AlignerStats& GetAlignerStats(const FO3DReceiverSource& Source) { return Source.ControlAligner.GetStats(); }
	static size_t GetHeldControlChanges(const FO3DReceiverSource& Source) { return Source.ControlAligner.NumHeld(); }

	// Typed config (WP-A1 PR 5a): the config the source would start its transport with.
	static FO3DTransportConfig BuildTransportConfig(const FO3DReceiverSource& Source) { return Source.BuildTransportConfig(); }

	// Schema validation (WP-A1 PR 5c): what the last StartTransport() refused, and the status line.
	static FO3DTransportResult GetLastTransportResult(const FO3DReceiverSource& Source) { return Source.LastTransportResult; }
	static FText GetSourceStatus(const FO3DReceiverSource& Source) { return Source.SourceStatus; }
};

/** Binds a remote control component to the bus without a world (BeginPlay needs one). */
struct FO3DRemoteControlComponentTestAccessor
{
	static void Bind(UO3DRemoteControlComponent& Component) { Component.Bind(); }
	static void Unbind(UO3DRemoteControlComponent& Component) { Component.Unbind(); }
};

/** Records the LiveLink static and frame pushes of one FO3DReceiverSource (WP-S4 correctness tests). */
struct FO3DReceiverCorrectnessTestAccessor
{
	struct FStaticPush
	{
		FName Subject;
		TArray<FName> BoneNames;
		TArray<int32> BoneParents;
		TArray<FName> CurveNames;
		bool bFirstPushThisSession = false;
	};

	struct FFramePush
	{
		FName Subject;
		TArray<FTransform> Transforms;
		TArray<float> Curves;
	};

	struct FRecorder
	{
		TArray<FStaticPush> Statics;
		TArray<FFramePush> Frames;

		int32 CountFrames(FName Subject) const
		{
			int32 Count = 0;
			for (const FFramePush& Frame : Frames)
			{
				Count += (Frame.Subject == Subject) ? 1 : 0;
			}
			return Count;
		}
	};

	static void BindRecorder(FO3DReceiverSource& Source, const TSharedRef<FRecorder>& Recorder)
	{
		Source.TestStaticPushHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPush)
		{
			FStaticPush Push;
			Push.Subject = Key.SubjectName.Name;
			Push.BoneNames = BoneNames;
			Push.BoneParents = BoneParents;
			Push.CurveNames = CurveNames;
			Push.bFirstPushThisSession = bFirstPush;
			Recorder->Statics.Add(MoveTemp(Push));
		};
		Source.TestFramePushHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FTransform>& Transforms, const TArray<float>& Curves, double)
		{
			FFramePush Push;
			Push.Subject = Key.SubjectName.Name;
			Push.Transforms = Transforms;
			Push.Curves = Curves;
			Recorder->Frames.Add(MoveTemp(Push));
		};
	}

	/** The same consumer object StartTransport() hands to a real transport. */
	static TSharedRef<ISerializedFrameConsumer> MakeConsumer(const TSharedRef<FO3DReceiverSource>& Source)
	{
		return FO3DReceiverSource::MakeSerializedConsumer(TWeakPtr<FO3DReceiverSource>(Source));
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
