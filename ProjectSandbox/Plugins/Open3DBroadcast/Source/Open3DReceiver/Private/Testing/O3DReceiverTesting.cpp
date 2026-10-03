// Copyright Lifelike & Believable. All Rights Reserved.

#include "Testing/O3DReceiverTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DLiveLinkPublisher.h"
#include "O3DReceiverFrameDecoder.h"
#include "O3DHelpers.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
