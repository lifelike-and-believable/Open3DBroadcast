// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DRuntimeSubsystem.h"

#include "Engine/Engine.h"
#include "O3DSharedLogs.h"

void UO3DRuntimeSubsystem::Deinitialize()
{
	Contexts.Empty();
	Super::Deinitialize();
}

FO3DRuntimeContextRef UO3DRuntimeSubsystem::GetContext(FName Name)
{
	check(IsInGameThread());
	if (Name.IsNone())
	{
		return FO3DRuntimeContext::Default();
	}
	if (const FO3DRuntimeContextRef* Existing = Contexts.Find(Name))
	{
		return *Existing;
	}
	FO3DRuntimeContextRef Created = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(Name);
	Contexts.Add(Name, Created);
	UE_LOG(LogO3DShared, Log, TEXT("Created runtime context '%s'"), *Name.ToString());
	return Created;
}

TArray<FO3DRuntimeContextRef> UO3DRuntimeSubsystem::GetNamedContexts() const
{
	check(IsInGameThread());
	TArray<FName> Names;
	Contexts.GetKeys(Names);
	Names.Sort(FNameLexicalLess());
	TArray<FO3DRuntimeContextRef> Result;
	Result.Reserve(Names.Num());
	for (const FName& Name : Names)
	{
		Result.Add(Contexts.FindChecked(Name));
	}
	return Result;
}

FO3DRuntimeContextRef UO3DRuntimeSubsystem::Resolve(FName Name)
{
	if (Name.IsNone())
	{
		return FO3DRuntimeContext::Default();
	}
	if (GEngine != nullptr)
	{
		if (UO3DRuntimeSubsystem* Subsystem = GEngine->GetEngineSubsystem<UO3DRuntimeSubsystem>())
		{
			return Subsystem->GetContext(Name);
		}
	}
	UE_LOG(LogO3DShared, Warning, TEXT("Runtime context '%s' requested before the engine started; using the default context."), *Name.ToString());
	return FO3DRuntimeContext::Default();
}
