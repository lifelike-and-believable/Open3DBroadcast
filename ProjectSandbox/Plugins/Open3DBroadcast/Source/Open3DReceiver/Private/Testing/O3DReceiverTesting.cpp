// Copyright Lifelike & Believable. All Rights Reserved.

#include "Testing/O3DReceiverTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DLiveLinkPublisher.h"
#include "O3DReceiverConcealment.h"
#include "O3DReceiverStreamScheduler.h"
#include "O3DReceiverFrameDecoder.h"
#include "O3DHelpers.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
THIRD_PARTY_INCLUDES_END

FO3DReceiverFrameDecoderProbe::FO3DReceiverFrameDecoderProbe()
	: Decoder(MakeUnique<FO3DReceiverFrameDecoder>())
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

	FO3DDecodedSubject Decoded;
	if (!Decoder->Decode(*List->mItems[0], bFullDescriptor, Decoded))
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
			[this](const FLiveLinkSubjectKey& Key, const TArray<FTransform>& Transforms, const TArray<float>&, double WorldTime)
			{
				Frames.Add({ Key.SubjectName.Name, Transforms.Num(), WorldTime });
			});
	}
}

FO3DLiveLinkPublisherProbe::~FO3DLiveLinkPublisherProbe() = default;

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
	Publisher->PublishFrame(Subject, Transforms, TArray<FName>(), TArray<float>(), WorldTime, WorldTime, 0);
}

void FO3DLiveLinkPublisherProbe::PublishSyntheticFrame(FName Subject, int32 NumTransforms, double Time)
{
	TArray<FTransform> Transforms;
	Transforms.SetNum(NumTransforms);
	Publisher->PublishSyntheticFrame(Subject, Transforms, TArray<float>(), Time);
}

TArray<FName> FO3DLiveLinkPublisherProbe::RemoveInactiveSubjects(double NowSeconds, double ThresholdSeconds)
{
	TArray<FName> Removed;
	Publisher->RemoveInactiveSubjects(NowSeconds, ThresholdSeconds, [&Removed](FName Subject) { Removed.Add(Subject); });
	return Removed;
}

int32 FO3DLiveLinkPublisherProbe::GetActiveSubjectCount() const
{
	return Publisher->GetActiveSubjectCount();
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
		}))
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
	: Concealment(MakeUnique<FO3DReceiverConcealment>())
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

#endif // WITH_DEV_AUTOMATION_TESTS
