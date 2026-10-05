// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DRuntimeContext.h"

FO3DRuntimeContext::FO3DRuntimeContext(FName InName)
	: Name(InName)
{
}

FO3DRuntimeContext::~FO3DRuntimeContext() = default;

const FO3DRuntimeContextRef& FO3DRuntimeContext::Default()
{
	// A function-local static: initialised once, thread-safely, on the first call, and destroyed
	// at static destruction, the same lifetime as the metrics singleton it replaces.
	static const FO3DRuntimeContextRef Instance = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(NAME_None);
	return Instance;
}
