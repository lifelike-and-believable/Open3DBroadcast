#include "Shared/MoQFfiSupport.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
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

	bIsLoaded = true;

	// Get version string if available
	FString Version = GetVersion();
	if (!Version.IsEmpty())
	{
		StatusMessage = FString::Printf(TEXT("MoQ FFI library loaded successfully (version %s)"), *Version);
		UE_LOG(LogMoQFfiSupport, Log, TEXT("Successfully loaded MoQ FFI library from: %s"), *LibraryPath);
		UE_LOG(LogMoQFfiSupport, Log, TEXT("MoQ FFI version: %s"), *Version);
	}
	else
	{
		StatusMessage = TEXT("MoQ FFI library loaded successfully");
		UE_LOG(LogMoQFfiSupport, Log, TEXT("Successfully loaded MoQ FFI library from: %s"), *LibraryPath);
	}

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
	if (!bIsLoaded || LibraryHandle == nullptr)
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

	// Fallback: version not available from library
	return FString(TEXT("unknown"));
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

	// Check for essential exported functions
	// These are the core functions we expect from moq_ffi.h
	struct RequiredSymbol
	{
		const TCHAR* Name;
		bool bOptional;
	};

	const RequiredSymbol RequiredSymbols[] = {
		{ TEXT("moq_client_create"), false },
		{ TEXT("moq_client_destroy"), false },
		{ TEXT("moq_connect"), false },
		{ TEXT("moq_disconnect"), false },
		{ TEXT("moq_create_publisher"), false },
		{ TEXT("moq_publisher_destroy"), false },
		{ TEXT("moq_subscribe"), false },
		{ TEXT("moq_subscriber_destroy"), false },
		{ TEXT("moq_free_str"), false },
		{ TEXT("moq_version"), true }, // Version function is optional
	};

	bool bAllValid = true;
	for (const RequiredSymbol& Symbol : RequiredSymbols)
	{
		void* Proc = FPlatformProcess::GetDllExport(LibraryHandle, Symbol.Name);
		if (Proc == nullptr && !Symbol.bOptional)
		{
			UE_LOG(LogMoQFfiSupport, Error, TEXT("Required symbol '%s' not found in MoQ FFI library"), Symbol.Name);
			bAllValid = false;
		}
		else if (Proc != nullptr)
		{
			UE_LOG(LogMoQFfiSupport, Verbose, TEXT("Found symbol: %s"), Symbol.Name);
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
