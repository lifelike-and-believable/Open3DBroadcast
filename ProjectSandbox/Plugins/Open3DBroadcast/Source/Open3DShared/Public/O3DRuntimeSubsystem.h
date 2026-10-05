// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DRuntimeContext.h"
#include "Subsystems/EngineSubsystem.h"
#include "O3DRuntimeSubsystem.generated.h"

/**
 * Owns the named runtime contexts (docs/adr/0012-runtime-services-and-global-state.md, item 5).
 * Receiver sources, sender components and the remote audio and control components select one by
 * their ContextName: the same name gives the same context, so their audio, control and metrics
 * meet, and two names never do. The empty name (NAME_None) is FO3DRuntimeContext::Default().
 *
 * Names are FNames, so they compare case-insensitively ("Stage" and "stage" are one context),
 * unlike control keys. A named context is created on first use and kept until the subsystem
 * deinitializes; owners hold a strong reference, so one that outlives the subsystem keeps its
 * context. Selection is by name, not by world: the LiveLink client is one per process in UE 5.7
 * (FLiveLinkModule owns a single FLiveLinkClient), so PIE worlds share it whatever the names.
 */
UCLASS()
class OPEN3DSHARED_API UO3DRuntimeSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	/** The context named Name, created on first use; NAME_None is the default context. Game thread. */
	FO3DRuntimeContextRef GetContext(FName Name);

	/** The named contexts created so far (the default context not included), sorted by name. Game thread. */
	TArray<FO3DRuntimeContextRef> GetNamedContexts() const;

	/**
	 * Resolves Name through the engine's subsystem. NAME_None, or no engine yet, gives the default
	 * context; the second case logs a warning naming the context, so a named owner never lands in
	 * the default context silently. Game thread.
	 */
	static FO3DRuntimeContextRef Resolve(FName Name);

private:
	TMap<FName, FO3DRuntimeContextRef> Contexts;
};
