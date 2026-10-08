// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Testing/O3DReceiverTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DLiveLinkPublisher.h"
#include "O3DReceiverConcealment.h"
#include "O3DReceiverControlRouter.h"
#include "O3DReceiverStreamScheduler.h"
#include "O3DReceiverFrameDecoder.h"
#include "O3DSceneTimeMapper.h"
#include "O3DHelpers.h"
#include "O3DRuntimeContext.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	/** The probes record into a handle of the default context, as a receiver source does today (ADR 0012). */
	FO3DReceiverMetricsHandleRef MakeProbeMetrics(const TCHAR* ProbeName)
	{
		return FO3DRuntimeContext::Default()->GetMetrics().AcquireReceiverMetrics(ProbeName);
	}
}

FO3DReceiverFrameDecoderProbe::FO3DReceiverFrameDecoderProbe()
	: Decoder(MakeUnique<FO3DReceiverFrameDecoder>(MakeProbeMetrics(TEXT("Frame decoder probe"))))
	, List(MakeUnique<O3DS::SubjectList>())
{
}

FO3DReceiverFrameDecoderProbe::~FO3DReceiverFrameDecoderProbe() = default;

bool FO3DReceiverFrameDecoderProbe::Decode(TConstArrayView<uint8> Buffer, bool bFullDescriptor)
{
	if (!List->Parse(reinterpret_cast<const char*>(Buffer.GetData()), static_cast<size_t>(Buffer.Num()), nullptr, true) || List->mItems.empty())
	{
		return false;
	}
	return DecodeSubject(*List->mItems[0], bFullDescriptor);
}

bool FO3DReceiverFrameDecoderProbe::DecodeSubject(const O3DS::Subject& Subject, bool bFullDescriptor)
{
	FO3DDecodedSubject Decoded;
	if (!Decoder->Decode(Subject, bFullDescriptor, Decoded))
	{
		return false;
	}
	SubjectName = Decoded.SubjectName;
	BoneNames = *Decoded.BoneNames;
	BoneParents = *Decoded.BoneParents;
	BoneTransforms = *Decoded.BoneTransforms;
	CurveNames = *Decoded.CurveNames;
	CurveValues = *Decoded.CurveValues;
	SkeletonHash = Decoded.SkeletonHash;
	CurveHash = Decoded.CurveHash;
	return true;
}

void FO3DReceiverFrameDecoderProbe::ForgetSubject(FName InSubjectName)
{
	Decoder->ForgetSubject(InSubjectName);
}

void FO3DReceiverFrameDecoderProbe::Reset()
{
	Decoder->Reset();
}

uint64 FO3DReceiverFrameDecoderProbe::GetSkeletonBuilds() const
{
	return Decoder->GetSkeletonBuilds();
}

uint64 FO3DReceiverFrameDecoderProbe::GetCurveNameBuilds() const
{
	return Decoder->GetCurveNameBuilds();
}

FO3DLiveLinkPublisherProbe::FO3DLiveLinkPublisherProbe(bool bBindHooks)
	: Publisher(MakeUnique<FO3DLiveLinkPublisher>())
{
	if (bBindHooks)
	{
		Publisher->SetTestHooks(
			[this](const FLiveLinkSubjectKey& Key, const TArray<FName>&, const TArray<int32>&, const TArray<FName>& CurveNames, bool bFirstPush)
			{
				Statics.Add({ Key.SubjectName.Name, CurveNames, bFirstPush });
			},
			[this](const FLiveLinkSubjectKey& Key, const TArray<FTransform>& Transforms, const TArray<float>&, double WorldTime,
				const TOptional<FQualifiedFrameTime>& SceneTime)
			{
				Frames.Add({ Key.SubjectName.Name, Transforms.Num(), WorldTime, SceneTime });
			});
	}
}

FO3DLiveLinkPublisherProbe::~FO3DLiveLinkPublisherProbe() = default;

ULiveLinkSubjectSettings* FO3DLiveLinkPublisherProbe::MakeSubjectSettings(FName Subject)
{
	return FO3DLiveLinkPublisher::MakeSubjectSettings(FLiveLinkSubjectKey(FGuid::NewGuid(), Subject));
}

bool FO3DLiveLinkPublisherProbe::CanPublish() const
{
	return Publisher->CanPublish();
}

bool FO3DLiveLinkPublisherProbe::PublishStatic(FName Subject, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames)
{
	FO3DDecodedSubject Decoded;
	Decoded.SubjectName = Subject;
	Decoded.BoneNames = &BoneNames;
	Decoded.BoneParents = &BoneParents;
	Decoded.SkeletonHash = O3DHelpers::HashNamesAndParents(BoneNames, BoneParents);
	Decoded.CurveNames = &CurveNames;
	Decoded.CurveHash = O3DHelpers::HashNames(CurveNames);
	return Publisher->PublishStatic(Decoded);
}

void FO3DLiveLinkPublisherProbe::PublishFrame(FName Subject, int32 NumTransforms, double WorldTime)
{
	TArray<FTransform> Transforms;
	Transforms.SetNum(NumTransforms);
	Publisher->PublishFrame(Subject, Transforms, TArray<FName>(), TArray<float>(), WorldTime, nullptr);
}

void FO3DLiveLinkPublisherProbe::PublishFrameWithSenderTime(FName Subject, int32 NumTransforms, double WorldTime, int32 Frame, float SubFrame,
	int32 RateNumerator, int32 RateDenominator)
{
	TArray<FTransform> Transforms;
	Transforms.SetNum(NumTransforms);
	O3DS::SceneTime SenderTime;
	SenderTime.frame = Frame;
	SenderTime.subframe = SubFrame;
	SenderTime.rate_numerator = RateNumerator;
	SenderTime.rate_denominator = RateDenominator;
	Publisher->PublishFrame(Subject, Transforms, TArray<FName>(), TArray<float>(), WorldTime, &SenderTime);
}

FO3DSceneTimeMapperProbe::FO3DSceneTimeMapperProbe()
	: Mapper(MakeUnique<FO3DSceneTimeMapper>())
{
}

FO3DSceneTimeMapperProbe::~FO3DSceneTimeMapperProbe() = default;

TOptional<FQualifiedFrameTime> FO3DSceneTimeMapperProbe::MapSender(FName Subject, int32 Frame, float SubFrame, int32 RateNumerator, int32 RateDenominator,
	double WorldTime, double NowSeconds, const TOptional<FQualifiedFrameTime>& EngineTime)
{
	O3DS::SceneTime SenderTime;
	SenderTime.frame = Frame;
	SenderTime.subframe = SubFrame;
	SenderTime.rate_numerator = RateNumerator;
	SenderTime.rate_denominator = RateDenominator;
	return Mapper->Map(Subject, &SenderTime, WorldTime, NowSeconds, EngineTime);
}

TOptional<FQualifiedFrameTime> FO3DSceneTimeMapperProbe::MapWithout(FName Subject, double WorldTime, double NowSeconds, const TOptional<FQualifiedFrameTime>& EngineTime)
{
	return Mapper->Map(Subject, nullptr, WorldTime, NowSeconds, EngineTime);
}

void FO3DSceneTimeMapperProbe::ForgetSubject(FName Subject)
{
	Mapper->ForgetSubject(Subject);
}

void FO3DSceneTimeMapperProbe::Reset()
{
	Mapper->Reset();
}

void FO3DLiveLinkPublisherProbe::PublishSyntheticFrame(FName Subject, int32 NumTransforms, double Time)
{
	TArray<FTransform> Transforms;
	Transforms.SetNum(NumTransforms);
	Publisher->PublishSyntheticFrame(Subject, Transforms, TArray<float>(), Time);
}

TArray<FName> FO3DLiveLinkPublisherProbe::ClearInactiveSubjects(double NowSeconds, double ThresholdSeconds)
{
	TArray<FName> Cleared;
	Publisher->ClearInactiveSubjects(NowSeconds, ThresholdSeconds, [&Cleared](FName Subject) { Cleared.Add(Subject); });
	return Cleared;
}

int32 FO3DLiveLinkPublisherProbe::GetActiveSubjectCount() const
{
	return Publisher->GetActiveSubjectCount();
}

void FO3DLiveLinkPublisherProbe::NoteSlowFramePush(FName Subject, double PushMs, double NowSeconds)
{
	Publisher->NoteSlowFramePush(Subject, PushMs, NowSeconds);
}

int32 FO3DLiveLinkPublisherProbe::GetSlowPushesNotLogged() const
{
	return Publisher->GetSlowPushesNotLogged();
}

void FO3DLiveLinkPublisherProbe::Reset()
{
	Publisher->Reset();
}

FO3DReceiverStreamSchedulerProbe::FO3DReceiverStreamSchedulerProbe()
	: Scheduler(MakeUnique<FO3DReceiverStreamScheduler>(
		[this](O3DS::ReceiverStream&, const FString& Label, const char*, size_t Len, double LegacyTimestampSeconds, const O3DS::Frame* GatedFrame)
		{
			FReleased Entry;
			Entry.Label = Label;
			Entry.NumBytes = static_cast<int32>(Len);
			Entry.bGated = GatedFrame != nullptr;
			Entry.Seq = GatedFrame ? GatedFrame->seq : 0;
			Entry.LegacyTimestampSeconds = LegacyTimestampSeconds;
			Released.Add(MoveTemp(Entry));
		}, MakeProbeMetrics(TEXT("Stream scheduler probe"))))
{
}

FO3DReceiverStreamSchedulerProbe::~FO3DReceiverStreamSchedulerProbe() = default;

bool FO3DReceiverStreamSchedulerProbe::Push(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, double NowSeconds)
{
	O3DS::PacketMeta Meta;
	if (!O3DS::PeekPacketMeta(reinterpret_cast<const char*>(Buffer.GetData()), static_cast<size_t>(Buffer.Num()), Meta))
	{
		return false;
	}
	Scheduler->Push(Subject, Buffer, TimestampSeconds, Meta, 1, NowSeconds, O3DS::LegacyOrderingConfig(), false);
	return true;
}

void FO3DReceiverStreamSchedulerProbe::Flush(double NowSeconds)
{
	Scheduler->Flush(NowSeconds);
}

void FO3DReceiverStreamSchedulerProbe::PruneIdle(double NowSeconds, double IdleSeconds)
{
	Scheduler->PruneIdle(NowSeconds, IdleSeconds);
}

void FO3DReceiverStreamSchedulerProbe::Reset()
{
	Scheduler->Reset();
}

int32 FO3DReceiverStreamSchedulerProbe::GetNumStreams() const
{
	return static_cast<int32>(Scheduler->GetNumStreams());
}

FO3DReceiverConcealmentProbe::FO3DReceiverConcealmentProbe()
	: Concealment(MakeUnique<FO3DReceiverConcealment>(MakeProbeMetrics(TEXT("Concealment probe"))))
{
}

FO3DReceiverConcealmentProbe::~FO3DReceiverConcealmentProbe() = default;

void FO3DReceiverConcealmentProbe::ObserveRealFrame(const UO3DReceiverSourceSettings* Settings, FName Subject, double PresentationTimeSeconds, const TArray<FTransform>& Transforms, bool bTopologyChanged)
{
	Concealment->ObserveRealFrame(Settings, Subject, PresentationTimeSeconds, Transforms, TArray<float>(), bTopologyChanged);
}

void FO3DReceiverConcealmentProbe::NoteClockOffset(int64 OffsetEstimateUs)
{
	Concealment->NoteClockOffset(OffsetEstimateUs);
}

void FO3DReceiverConcealmentProbe::Tick(const UO3DReceiverSourceSettings* Settings, bool bCanPublish, double NowSeconds)
{
	Concealment->Tick(Settings, bCanPublish, NowSeconds,
		[this](FName Subject, const TArray<FTransform>& Transforms, const TArray<float>&, double Time)
		{
			Synthetic.Add({ Subject, Transforms, Time });
		});
}

void FO3DReceiverConcealmentProbe::ForgetSubject(FName Subject)
{
	Concealment->ForgetSubject(Subject);
}

void FO3DReceiverConcealmentProbe::Reset()
{
	Concealment->Reset();
}

int32 FO3DReceiverConcealmentProbe::GetNumEngines() const
{
	return Concealment->GetNumEngines();
}

bool FO3DReceiverConcealmentProbe::HasClockOffsetEstimate() const
{
	return Concealment->HasClockOffsetEstimate();
}

FO3DReceiverControlRouterProbe::FO3DReceiverControlRouterProbe()
	: Router(MakeUnique<FO3DReceiverControlRouter>())
{
}

FO3DReceiverControlRouterProbe::~FO3DReceiverControlRouterProbe() = default;

void FO3DReceiverControlRouterProbe::ApplyConfig()
{
	Router->ApplyConfig();
}

void FO3DReceiverControlRouterProbe::HandlePayload(bool bEnabled, TConstArrayView<uint8> Payload, double NowSeconds, const FString& StreamId)
{
	Router->HandlePayload(bEnabled, Payload.GetData(), Payload.Num(), NowSeconds, StreamId);
}

void FO3DReceiverControlRouterProbe::Tick(bool bEnabled, double NowSeconds, const FString& StreamId, int64 PresentedSenderTimeUs)
{
	Router->Tick(bEnabled, NowSeconds, StreamId, [this, PresentedSenderTimeUs](const std::vector<std::string>& MocapSubjects, uint64_t& OutUs)
	{
		LastAskedSubjects.Reset();
		for (const std::string& Subject : MocapSubjects)
		{
			LastAskedSubjects.Add(UTF8_TO_TCHAR(Subject.c_str()));
		}
		if (PresentedSenderTimeUs < 0)
		{
			return false;
		}
		OutUs = static_cast<uint64_t>(PresentedSenderTimeUs);
		return true;
	});
}

void FO3DReceiverControlRouterProbe::FlushHeld(const FString& StreamId)
{
	Router->FlushHeld(StreamId);
}

uint64 FO3DReceiverControlRouterProbe::GetPayloadsDroppedDisabled() const
{
	return Router->GetPayloadsDroppedDisabled();
}

int32 FO3DReceiverControlRouterProbe::GetNumHeld() const
{
	return static_cast<int32>(Router->GetNumHeld());
}

#endif // WITH_DEV_AUTOMATION_TESTS
