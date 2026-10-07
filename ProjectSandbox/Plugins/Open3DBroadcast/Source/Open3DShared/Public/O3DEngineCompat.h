// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

// Engine-version differences the plugins build against (ADR 0014: UE 5.7 and 5.8). Every guard
// names the engine change it covers; prefer code that compiles on both engines to a guard here.

#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"

namespace O3DEngineCompat
{
	/** UE 5.8 deprecates FCoreDelegates::OnPostEngineInit for GetOnPostEngineInit(), which 5.7 lacks. */
	inline FSimpleMulticastDelegate& OnPostEngineInit()
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		return FCoreDelegates::GetOnPostEngineInit();
#else
		return FCoreDelegates::OnPostEngineInit;
#endif
	}
}
