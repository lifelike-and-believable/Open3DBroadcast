// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DReceiverFrameDecoder.h"

#include "O3DHelpers.h"
#include "O3DPerformanceMetrics.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
THIRD_PARTY_INCLUDES_END

namespace O3DReceiverFrameDecoderPrivate
{
	/** More distinct subject names than this clears the name cache (they come from the network). */
	constexpr size_t MaxCachedSubjectNames = 1024;

	/**
	 * One O3DS transform as an FTransform, or false when a value is not finite or the rotation is
	 * zero. The core's doubles are kept (RCV-13): FVector and FQuat are double, and a quantized
	 * update is rebuilt in double (a translation as anchor plus delta, a rotation dequantized), so a
	 * float cast would lose precision.
	 */
	bool TryConvertTransform(const O3DS::Transform& Transform, FTransform& Out)
	{
		const O3DS::Vector3d& Translation = Transform.translation.value;
		const O3DS::Vector4d& Rotation = Transform.rotation.value;
		const O3DS::Vector3d& Scale = Transform.scale.value;

		FQuat Quat(Rotation.v[0], Rotation.v[1], Rotation.v[2], Rotation.v[3]);
		const FVector Location(Translation.v[0], Translation.v[1], Translation.v[2]);
		const FVector ScaleVec(Scale.v[0], Scale.v[1], Scale.v[2]);

		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z) ||
			!FMath::IsFinite(ScaleVec.X) || !FMath::IsFinite(ScaleVec.Y) || !FMath::IsFinite(ScaleVec.Z) ||
			!FMath::IsFinite(Quat.X) || !FMath::IsFinite(Quat.Y) || !FMath::IsFinite(Quat.Z) || !FMath::IsFinite(Quat.W))
		{
			return false;
		}

		if (Quat.SizeSquared() <= KINDA_SMALL_NUMBER)
		{
			return false;
		}

		Quat.Normalize();
		if (Quat.ContainsNaN())
		{
			return false;
		}

		Out = FTransform(Quat, Location, ScaleVec);
		return true;
	}

	/** The bone name LiveLink sees: the text after the last ':' (a namespace prefix is dropped). */
	FName BoneNameFromTransform(const O3DS::Transform& Transform)
	{
		std::string BoneNameUtf8 = Transform.mName;
		const size_t ColonIndex = BoneNameUtf8.rfind(':');
		if (ColonIndex != std::string::npos)
		{
			BoneNameUtf8.erase(0, ColonIndex + 1);
		}
		return FName(UTF8_TO_TCHAR(BoneNameUtf8.c_str()));
	}
}

FName FO3DReceiverFrameDecoder::ResolveSubjectName(const std::string& RawName)
{
	const auto Found = SubjectNames.find(RawName);
	if (Found != SubjectNames.end())
	{
		return Found->second;
	}
	if (SubjectNames.size() >= O3DReceiverFrameDecoderPrivate::MaxCachedSubjectNames)
	{
		SubjectNames.clear();
	}
	const FName Name(*FString(UTF8_TO_TCHAR(RawName.c_str())));
	SubjectNames.emplace(RawName, Name);
	return Name;
}

bool FO3DReceiverFrameDecoder::ConvertTransforms(const O3DS::Subject& Subject)
{
	const std::vector<O3DS::Transform*>& Items = Subject.mTransforms.mItems;
	Transforms.Reset();
	if (Items.empty())
	{
		return false;
	}

	Transforms.SetNum(static_cast<int32>(Items.size()), EAllowShrinking::No);
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		// RCV-14: parent ids index this list, so skipping an entry would shift every later
		// parent. The core parser never leaves a null entry (a nameless node gets a placeholder
		// name), so treat one as a malformed frame. RCV-13: the dropped pose is counted.
		if (Items[Index] == nullptr
			|| !O3DReceiverFrameDecoderPrivate::TryConvertTransform(*Items[Index], Transforms[static_cast<int32>(Index)]))
		{
			Transforms.Reset();
			FO3DPerformanceMetrics::Get().RecordInvalidPoseDropped();
			return false;
		}
	}
	return true;
}

void FO3DReceiverFrameDecoder::ConvertCurves(const O3DS::Subject& Subject, FSubjectCache& Cache)
{
	if (!Cache.bHasCurveNames || Cache.RawCurveNames != Subject.mCurveNames)
	{
		Cache.RawCurveNames = Subject.mCurveNames;
		Cache.CurveNames.Reset(static_cast<int32>(Subject.mCurveNames.size()));
		for (const std::string& CurveNameUtf8 : Subject.mCurveNames)
		{
			Cache.CurveNames.Add(FName(UTF8_TO_TCHAR(CurveNameUtf8.c_str())));
		}
		Cache.CurveHash = O3DHelpers::HashNames(Cache.CurveNames);
		Cache.bHasCurveNames = true;
		++CurveNameBuilds;
	}

	const size_t CurveCount = Subject.mCurveNames.size();
	CurveValues.SetNumUninitialized(static_cast<int32>(CurveCount), EAllowShrinking::No);
	for (size_t CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
	{
		CurveValues[static_cast<int32>(CurveIndex)] = (CurveIndex < Subject.mCurveValues.size()) ? Subject.mCurveValues[CurveIndex] : 0.0f;
	}
}

bool FO3DReceiverFrameDecoder::Decode(const O3DS::Subject& Subject, bool bFullDescriptor, FO3DDecodedSubject& Out)
{
	Out = FO3DDecodedSubject();
	const FName SubjectName = ResolveSubjectName(Subject.mName);

	// RCV-4: the cached bone names and parents are reused only when the skeleton fingerprint
	// (bone count, names and parent ids) is unchanged and this packet did not carry a full
	// descriptor for the subject. Hashing the name bytes is cheap next to building FNames,
	// which is what the cache saves.
	const uint64 Fingerprint = O3DS::SkeletonFingerprint(Subject);
	const FSubjectCache* Existing = Caches.Find(SubjectName);
	const bool bReuseSkeleton = !bFullDescriptor && Existing != nullptr && Existing->bHasSkeleton && Existing->SkeletonFingerprint == Fingerprint;

	if (!ConvertTransforms(Subject))
	{
		return false;
	}

	FSubjectCache& Cache = Caches.FindOrAdd(SubjectName);
	if (!bReuseSkeleton)
	{
		const std::vector<O3DS::Transform*>& Items = Subject.mTransforms.mItems;
		Cache.BoneNames.Reset(static_cast<int32>(Items.size()));
		Cache.BoneParents.Reset(static_cast<int32>(Items.size()));
		for (const O3DS::Transform* Transform : Items)
		{
			Cache.BoneNames.Add(O3DReceiverFrameDecoderPrivate::BoneNameFromTransform(*Transform));
			Cache.BoneParents.Add(Transform->mParentId);
		}
		Cache.SkeletonFingerprint = Fingerprint;
		Cache.SkeletonHash = O3DHelpers::HashNamesAndParents(Cache.BoneNames, Cache.BoneParents);
		Cache.bHasSkeleton = true;
		++SkeletonBuilds;
	}

	ConvertCurves(Subject, Cache);

	Out.SubjectName = SubjectName;
	Out.BoneNames = &Cache.BoneNames;
	Out.BoneParents = &Cache.BoneParents;
	Out.SkeletonHash = Cache.SkeletonHash;
	Out.BoneTransforms = &Transforms;
	Out.CurveNames = &Cache.CurveNames;
	Out.CurveValues = &CurveValues;
	Out.CurveHash = Cache.CurveHash;
	return true;
}

void FO3DReceiverFrameDecoder::ForgetSubject(FName SubjectName)
{
	Caches.Remove(SubjectName);
}

void FO3DReceiverFrameDecoder::Reset()
{
	Caches.Empty();
	SubjectNames.clear();
	Transforms.Reset();
	CurveValues.Reset();
}
