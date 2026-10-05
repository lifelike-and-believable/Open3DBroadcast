// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DPerformanceMetrics.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace O3DS
{
	class Subject;
}

/**
 * One parsed subject converted for LiveLink. The arrays belong to the decoder and stay valid
 * until its next Decode, ForgetSubject or Reset.
 */
struct FO3DDecodedSubject
{
	FName SubjectName;
	const TArray<FName>* BoneNames = nullptr;
	const TArray<int32>* BoneParents = nullptr;
	/** O3DHelpers::HashNamesAndParents of the bone names and parents. */
	uint64 SkeletonHash = 0;
	const TArray<FTransform>* BoneTransforms = nullptr;
	const TArray<FName>* CurveNames = nullptr;
	const TArray<float>* CurveValues = nullptr;
	/** O3DHelpers::HashNames of the curve names. */
	uint64 CurveHash = 0;
};

/**
 * Converts parsed O3DS subjects into LiveLink bone names, parents, transforms and curves (WP-A3,
 * RCV-29). Per subject it caches what only changes with the topology (RCV-4, RCV-11, RCV-12): the
 * bone names, parents and their hash, rebuilt when the skeleton fingerprint changes or the packet
 * carried a full descriptor; the curve FNames and their hash, rebuilt when the curve name strings
 * change; and the subject FName. A frame converts only its values, into arrays reused across
 * frames. Game thread, like the receiver source that owns it.
 */
class FO3DReceiverFrameDecoder
{
public:
	/** Metrics: the owning receiver source's handle (ADR 0012 item 4). */
	explicit FO3DReceiverFrameDecoder(FO3DReceiverMetricsHandleRef InMetrics);
	/**
	 * Converts one subject. False when it has no usable pose: no transforms, a missing transform
	 * (RCV-14), a non-finite value or a zero rotation. The caller then skips the subject, as the
	 * receiver always has.
	 */
	bool Decode(const O3DS::Subject& Subject, bool bFullDescriptor, FO3DDecodedSubject& Out);

	/** Drops one subject's caches (the subject was removed). */
	void ForgetSubject(FName SubjectName);

	/** Drops every cache (the transport stopped). */
	void Reset();

	/** Times bone names were built from a subject (full descriptor or new skeleton); for tests. */
	uint64 GetSkeletonBuilds() const { return SkeletonBuilds; }
	/** Times curve FNames were built from a subject's curve name strings; for tests. */
	uint64 GetCurveNameBuilds() const { return CurveNameBuilds; }

private:
	FO3DReceiverMetricsHandleRef Metrics;

	struct FSubjectCache
	{
		TArray<FName> BoneNames;
		TArray<int32> BoneParents;
		uint64 SkeletonFingerprint = 0;
		uint64 SkeletonHash = 0;
		bool bHasSkeleton = false;

		std::vector<std::string> RawCurveNames;
		TArray<FName> CurveNames;
		uint64 CurveHash = 0;
		bool bHasCurveNames = false;
	};

	FName ResolveSubjectName(const std::string& RawName);
	bool ConvertTransforms(const O3DS::Subject& Subject);
	void ConvertCurves(const O3DS::Subject& Subject, FSubjectCache& Cache);

	TMap<FName, FSubjectCache> Caches;
	/** Subject name text to FName; cleared when it outgrows the subject limit (names come from the network). */
	std::unordered_map<std::string, FName> SubjectNames;

	// Scratch reused across frames (RCV-11).
	TArray<FTransform> Transforms;
	TArray<float> CurveValues;

	uint64 SkeletonBuilds = 0;
	uint64 CurveNameBuilds = 0;
};
