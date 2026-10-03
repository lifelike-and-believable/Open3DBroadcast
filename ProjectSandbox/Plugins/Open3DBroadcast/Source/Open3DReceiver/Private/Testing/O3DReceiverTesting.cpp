// Copyright Lifelike & Believable. All Rights Reserved.

#include "Testing/O3DReceiverTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverFrameDecoder.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
