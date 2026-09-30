// Copyright (c) Open3DStream Contributors

#include "O3DFfiLibrary.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "O3DSharedLogs.h"

FO3DFfiLibraryOps FO3DFfiLibraryOps::MakePlatform()
{
	FO3DFfiLibraryOps Ops;
	Ops.FindPluginBaseDir = [](const FString& PluginName) -> FString
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
	};
	Ops.FileExists = [](const FString& Path) { return FPaths::FileExists(Path); };
	Ops.LoadDll = [](const FString& Path) { return FPlatformProcess::GetDllHandle(*Path); };
	Ops.FreeDll = [](void* InHandle) { FPlatformProcess::FreeDllHandle(InHandle); };
	Ops.GetExport = [](void* InHandle, const TCHAR* SymbolName) { return FPlatformProcess::GetDllExport(InHandle, SymbolName); };
	return Ops;
}

FO3DFfiLibrary::FO3DFfiLibrary(const FO3DFfiLibraryDesc& InDesc)
	: FO3DFfiLibrary(InDesc, FO3DFfiLibraryOps::MakePlatform())
{
}

FO3DFfiLibrary::FO3DFfiLibrary(const FO3DFfiLibraryDesc& InDesc, FO3DFfiLibraryOps InOps)
	: Desc(InDesc)
	, Ops(MoveTemp(InOps))
	, StatusMessage(TEXT("Not loaded"))
{
}

FO3DFfiLibrary::~FO3DFfiLibrary()
{
	// Deliberately no FreeDll here: only Unload() decides to free (see the class comment).
}

bool FO3DFfiLibrary::Load()
{
	if (Handle != nullptr)
	{
		return true;
	}

	const FString BaseDir = Ops.FindPluginBaseDir ? Ops.FindPluginBaseDir(Desc.OwningPluginName) : FString();
	if (BaseDir.IsEmpty())
	{
		StatusMessage = FString::Printf(TEXT("%s: plugin '%s' not found, cannot locate %s"),
			*Desc.DisplayName, *Desc.OwningPluginName, *Desc.RelativePath);
		UE_LOG(LogO3DShared, Error, TEXT("%s"), *StatusMessage);
		return false;
	}

	LibraryPath = FPaths::Combine(BaseDir, Desc.RelativePath);
	if (!Ops.FileExists || !Ops.FileExists(LibraryPath))
	{
		StatusMessage = FString::Printf(TEXT("%s: library not found at %s"), *Desc.DisplayName, *LibraryPath);
		UE_LOG(LogO3DShared, Error, TEXT("%s"), *StatusMessage);
		return false;
	}

	Handle = Ops.LoadDll ? Ops.LoadDll(LibraryPath) : nullptr;
	if (Handle == nullptr)
	{
		StatusMessage = FString::Printf(TEXT("%s: failed to load %s"), *Desc.DisplayName, *LibraryPath);
		UE_LOG(LogO3DShared, Error, TEXT("%s"), *StatusMessage);
		return false;
	}

	StatusMessage = FString::Printf(TEXT("%s: loaded from %s"), *Desc.DisplayName, *LibraryPath);
	UE_LOG(LogO3DShared, Log, TEXT("%s"), *StatusMessage);
	return true;
}

void* FO3DFfiLibrary::GetExport(const TCHAR* SymbolName) const
{
	if (Handle == nullptr || SymbolName == nullptr || !Ops.GetExport)
	{
		return nullptr;
	}
	return Ops.GetExport(Handle, SymbolName);
}

void FO3DFfiLibrary::AddLiveInstance(FLiveInstance&& Entry)
{
	FScopeLock Lock(&InstancesMutex);
	// Prune here too, so a long session that creates many short-lived instances does not grow
	// the list without bound.
	PruneLocked();
	Instances.Add(MoveTemp(Entry));
}

int32 FO3DFfiLibrary::PruneLocked() const
{
	Instances.RemoveAll([](const FLiveInstance& Entry) { return !Entry.IsAlive(); });
	return Instances.Num();
}

int32 FO3DFfiLibrary::GetNumLiveInstances() const
{
	FScopeLock Lock(&InstancesMutex);
	return PruneLocked();
}

int32 FO3DFfiLibrary::StopLiveInstances()
{
	// Copy the entries and call Stop() outside the lock: Stop() may take the instance's own
	// locks, wait for its worker, or drop the last reference (running its destructor).
	TArray<FLiveInstance> Snapshot;
	{
		FScopeLock Lock(&InstancesMutex);
		PruneLocked();
		Snapshot = Instances;
	}

	int32 Stopped = 0;
	for (const FLiveInstance& Entry : Snapshot)
	{
		if (Entry.IsAlive())
		{
			Entry.Stop();
			++Stopped;
		}
	}

	if (Stopped > 0)
	{
		UE_LOG(LogO3DShared, Log, TEXT("%s: stopped %d live transport instance(s) before unload"), *Desc.DisplayName, Stopped);
	}
	return Stopped;
}

EO3DFfiUnloadResult FO3DFfiLibrary::Unload()
{
	if (Handle == nullptr)
	{
		return EO3DFfiUnloadResult::NotLoaded;
	}

	const int32 StillReferenced = GetNumLiveInstances();
	if (StillReferenced > 0)
	{
		StatusMessage = FString::Printf(TEXT("%s: %d transport instance(s) still referenced; library left loaded until process exit"),
			*Desc.DisplayName, StillReferenced);
		UE_LOG(LogO3DShared, Warning, TEXT("%s"), *StatusMessage);
		return EO3DFfiUnloadResult::KeptLoaded;
	}

	if (Ops.FreeDll)
	{
		Ops.FreeDll(Handle);
	}
	Handle = nullptr;
	StatusMessage = FString::Printf(TEXT("%s: unloaded"), *Desc.DisplayName);
	UE_LOG(LogO3DShared, Log, TEXT("%s"), *StatusMessage);
	return EO3DFfiUnloadResult::Unloaded;
}
