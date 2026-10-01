// Copyright (c) Open3DStream Contributors

#include "O3DFfiLibrary.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "O3DSharedLogs.h"
#include "Transport/O3DTransportRegistry.h"

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
	Ops.CountLiveInstances = [](FName TransportName) { return FO3DTransportRegistry::Get().GetNumLiveInstances(TransportName); };
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

int32 FO3DFfiLibrary::GetNumLiveInstances() const
{
	if (!Ops.CountLiveInstances)
	{
		return 0;
	}
	int32 Count = 0;
	for (const FName& TransportName : Desc.TransportNames)
	{
		Count += Ops.CountLiveInstances(TransportName);
	}
	return Count;
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
