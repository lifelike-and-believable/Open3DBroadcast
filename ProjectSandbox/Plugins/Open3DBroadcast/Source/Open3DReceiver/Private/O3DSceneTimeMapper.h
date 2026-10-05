// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"
#include "Misc/Optional.h"
#include "Misc/QualifiedFrameTime.h"

namespace O3DS
{
	struct SceneTime;
}

/**
 * The LiveLink SceneTime of each pushed frame (RCV-8, docs/adr/0013-livelink-timecode.md, decision
 * item 3), per subject:
 * - A frame with the sender's timecode (SubjectList.scene_time) gets it, converted to the
 *   subject's rate. The sender's values are never adjusted.
 * - A frame without one, for a subject that had sender timecode, continues the sender's timeline:
 *   the last sender timecode plus the WorldTime elapsed since. Concealed and render-ahead frames
 *   take this path too.
 * - Otherwise (option A'), the frame's WorldTime is converted to the engine's timecode: WorldTime
 *   (platform clock) plus one offset between the engine timecode and the platform clock, taken
 *   once for all subjects and taken again only when it drifts by more than a frame (the engine
 *   timecode is sampled at the start of a tick and may step in whole frames). A derived value is
 *   kept strictly increasing per subject and never negative. Without an engine timecode the
 *   frame gets none.
 * - A subject keeps the rate of its first frame with any timecode; later ones are converted to
 *   it, because a rate change flushes LiveLink's buffer for the subject.
 * Clocks are passed in, so the rules can be tested. Game thread.
 */
class FO3DSceneTimeMapper
{
public:
	/**
	 * SceneTime for one frame of Subject. SenderTime: the frame's scene_time, or null. WorldTime: the
	 * WorldTime pushed with it (platform clock). NowSeconds: FPlatformTime::Seconds(). EngineTime:
	 * FApp::GetCurrentFrameTime(). Unset: leave the frame's SceneTime at its default.
	 */
	TOptional<FQualifiedFrameTime> Map(FName Subject, const O3DS::SceneTime* SenderTime, double WorldTime, double NowSeconds,
		const TOptional<FQualifiedFrameTime>& EngineTime);

	/** Forgets one subject (it was removed). */
	void ForgetSubject(FName Subject) { Subjects.Remove(Subject); }

	/** Forgets every subject and the engine offset (the transport stopped). */
	void Reset();

private:
	struct FSubjectState
	{
		TOptional<FFrameRate> Rate;
		bool bHasSenderAnchor = false;
		double AnchorSceneSeconds = 0.0;
		double AnchorWorldTime = 0.0;
		bool bHasLast = false;
		double LastSeconds = 0.0;
	};

	TMap<FName, FSubjectState> Subjects;
	bool bHasEngineOffset = false;
	double EngineOffsetSeconds = 0.0;
};
