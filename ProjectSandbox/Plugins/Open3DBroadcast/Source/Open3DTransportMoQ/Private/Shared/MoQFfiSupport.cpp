#include "Shared/MoQFfiSupport.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "MoQFfiApi.h"
#include "moq_ffi.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoQFfiSupport, Log, All);

// Static member initialization
void* FMoQFfiSupport::LibraryHandle = nullptr;
FString FMoQFfiSupport::LibraryPath = TEXT("");
FString FMoQFfiSupport::StatusMessage = TEXT("Not loaded");
bool FMoQFfiSupport::bIsLoaded = false;

bool FMoQFfiSupport::LoadLibrary()
{
	if (bIsLoaded)
	{
		UE_LOG(LogMoQFfiSupport, Warning, TEXT("MoQ FFI library already loaded"));
		return true;
	}

	// Construct path to the library
	LibraryPath = ConstructLibraryPath();
	if (LibraryPath.IsEmpty())
	{
		StatusMessage = TEXT("Failed to construct library path");
		UE_LOG(LogMoQFfiSupport, Error, TEXT("%s"), *StatusMessage);
		return false;
	}

	// Check if file exists
	if (!FPaths::FileExists(LibraryPath))
	{
		StatusMessage = FString::Printf(TEXT("MoQ FFI library not found at: %s"), *LibraryPath);
		UE_LOG(LogMoQFfiSupport, Error, TEXT("%s"), *StatusMessage);
		UE_LOG(LogMoQFfiSupport, Error, TEXT("Please ensure moq-ffi binaries are built for your platform."));
		UE_LOG(LogMoQFfiSupport, Error, TEXT("See ThirdParty/moq-ffi/README.md for build instructions."));
		return false;
	}

	// Load the library
	LibraryHandle = FPlatformProcess::GetDllHandle(*LibraryPath);
	if (LibraryHandle == nullptr)
	{
		StatusMessage = FString::Printf(TEXT("Failed to load MoQ FFI library from: %s"), *LibraryPath);
		UE_LOG(LogMoQFfiSupport, Error, TEXT("%s"), *StatusMessage);
		return false;
	}

	// Validate the library
	if (!ValidateLibrary())
	{
		StatusMessage = TEXT("MoQ FFI library loaded but validation failed");
		UE_LOG(LogMoQFfiSupport, Error, TEXT("%s"), *StatusMessage);
		FPlatformProcess::FreeDllHandle(LibraryHandle);
		LibraryHandle = nullptr;
		return false;
	}

	// TRF-29: moq_version is a required export (ValidateLibrary checked it), so an empty
	// version here means the library returned null, which is a mismatch too. moq-ffi exports no
	// structured ABI version; until it does, the Draft 07 marker in the version string is the
	// only build check available (the shipped DLL reports "moq_ffi 0.1.0 (IETF Draft 07)").
	const FString Version = GetVersion();
	if (Version.IsEmpty() || !Version.Contains(TEXT("Draft 07")))
	{
		StatusMessage = FString::Printf(TEXT("MoQ FFI build mismatch (reported '%s'). Rebuild moq-ffi with --features with_moq_draft07."), *Version);
		UE_LOG(LogMoQFfiSupport, Error, TEXT("%s"), *StatusMessage);
		FPlatformProcess::FreeDllHandle(LibraryHandle);
		LibraryHandle = nullptr;
		return false;
	}

	bIsLoaded = true;

	StatusMessage = FString::Printf(TEXT("MoQ FFI library loaded successfully (version %s)"), *Version);
	UE_LOG(LogMoQFfiSupport, Log, TEXT("Successfully loaded MoQ FFI library from: %s"), *LibraryPath);
	UE_LOG(LogMoQFfiSupport, Log, TEXT("MoQ FFI version: %s"), *Version);

	return true;
}

void FMoQFfiSupport::UnloadLibrary()
{
	if (!bIsLoaded || LibraryHandle == nullptr)
	{
		return;
	}

	FPlatformProcess::FreeDllHandle(LibraryHandle);
	LibraryHandle = nullptr;
	bIsLoaded = false;
	StatusMessage = TEXT("Library unloaded");

	UE_LOG(LogMoQFfiSupport, Log, TEXT("MoQ FFI library unloaded"));
}

bool FMoQFfiSupport::IsLoaded()
{
	return bIsLoaded;
}

FString FMoQFfiSupport::GetVersion()
{
	if (LibraryHandle == nullptr)
	{
		return FString();
	}

	// Try to get version from FFI library
	// Note: This assumes moq_ffi exports a version function
	// If not available, we'll need to track version in our vendored README
	typedef const char* (*FnGetVersion)();
	FnGetVersion GetVersionFunc = (FnGetVersion)FPlatformProcess::GetDllExport(LibraryHandle, TEXT("moq_version"));
	
	if (GetVersionFunc != nullptr)
	{
		const char* VersionCStr = GetVersionFunc();
		if (VersionCStr != nullptr)
		{
			return FString(UTF8_TO_TCHAR(VersionCStr));
		}
	}

	// Missing export or null result: report nothing rather than a placeholder that could be
	// mistaken for a real version (TRF-29).
	return FString();
}

FString FMoQFfiSupport::GetLibraryPath()
{
	return LibraryPath;
}

FString FMoQFfiSupport::GetStatusMessage()
{
	return StatusMessage;
}

FString FMoQFfiSupport::ConstructLibraryPath()
{
	// Get the plugin base directory
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Open3DBroadcast"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogMoQFfiSupport, Error, TEXT("Failed to find Open3DBroadcast plugin"));
		return FString();
	}

	// Construct path to the library in ThirdParty/moq-ffi/bin/<Platform>/Release/
	FString Path = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Source"),
		TEXT("Open3DTransportMoQ"),
		TEXT("ThirdParty"),
		TEXT("moq-ffi"),
		TEXT("bin")
	);

#if PLATFORM_WINDOWS
	Path = FPaths::Combine(Path, TEXT("Win64"), TEXT("Release"), TEXT("moq_ffi.dll"));
#elif PLATFORM_LINUX
	Path = FPaths::Combine(Path, TEXT("Linux"), TEXT("Release"), TEXT("libmoq_ffi.so"));
#elif PLATFORM_MAC
	Path = FPaths::Combine(Path, TEXT("Mac"), TEXT("Release"), TEXT("libmoq_ffi.dylib"));
#else
	#error "Unsupported platform for MoQ FFI"
#endif

	return FPaths::ConvertRelativePathToFull(Path);
}

bool FMoQFfiSupport::ValidateLibrary()
{
	if (LibraryHandle == nullptr)
	{
		return false;
	}

	// TRF-29: validate exactly the exports this module binds (FMoQFfiApi), all required.
	// A stale DLL missing any of them would otherwise pass here and fail later on delay-load.
	bool bAllValid = true;
	for (const TCHAR* SymbolName : FMoQFfiApi::GetRequiredSymbolNames())
	{
		void* Proc = FPlatformProcess::GetDllExport(LibraryHandle, SymbolName);
		if (Proc == nullptr)
		{
			UE_LOG(LogMoQFfiSupport, Error, TEXT("Required symbol '%s' not found in MoQ FFI library"), SymbolName);
			bAllValid = false;
		}
		else
		{
			UE_LOG(LogMoQFfiSupport, Verbose, TEXT("Found symbol: %s"), SymbolName);
		}
	}

	if (!bAllValid)
	{
		UE_LOG(LogMoQFfiSupport, Error, TEXT("MoQ FFI library validation failed - missing required symbols"));
		UE_LOG(LogMoQFfiSupport, Error, TEXT("Library may be outdated or corrupted. Try refreshing from upstream."));
		UE_LOG(LogMoQFfiSupport, Error, TEXT("See ThirdParty/moq-ffi/README.md for refresh instructions."));
	}

	return bAllValid;
}
