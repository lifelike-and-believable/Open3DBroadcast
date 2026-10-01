// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQFfiSupport.h"
#include "MoQFfiApi.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoQFfiSupport, Log, All);

namespace MoQFfiSupportPrivate
{
	/** Plugin that ships moq_ffi. The path below is relative to its base directory. */
	static constexpr TCHAR OwningPluginName[] = TEXT("Open3DBroadcast");
}

FO3DFfiLibraryDesc FMoQFfiSupport::MakeLibraryDesc()
{
	FO3DFfiLibraryDesc Desc;
	Desc.DisplayName = TEXT("MoQ FFI");
	Desc.OwningPluginName = MoQFfiSupportPrivate::OwningPluginName;
	// Win64 only: O3D_WITH_TRANSPORT_MOQ is 0 on every other platform, so this file is then
	// compiled out (ADR 0001). The Linux and Mac paths named binaries that do not exist (BUILD-2).
	Desc.RelativePath = TEXT("Source/Open3DTransportMoQ/ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.dll");
	// Unload() keeps moq_ffi loaded while the registry still counts a live "MoQ" instance (WP-A1 PR 2).
	Desc.TransportNames.Add(FName(TEXT("MoQ")));
	return Desc;
}

FString FMoQFfiSupport::GetVersion(const FO3DFfiLibrary& Library)
{
	using FnGetVersion = const char* (*)();
	const FnGetVersion GetVersionFunc = reinterpret_cast<FnGetVersion>(Library.GetExport(TEXT("moq_version")));
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

bool FMoQFfiSupport::ValidateLibrary(const FO3DFfiLibrary& Library, FString& OutError)
{
	if (!Library.IsLoaded())
	{
		OutError = TEXT("MoQ FFI library is not loaded");
		return false;
	}

	// TRF-29: validate exactly the exports this module binds (FMoQFfiApi), all required.
	// A stale DLL missing any of them would otherwise pass here and fail later on delay-load.
	bool bAllValid = true;
	for (const TCHAR* SymbolName : FMoQFfiApi::GetRequiredSymbolNames())
	{
		if (Library.GetExport(SymbolName) == nullptr)
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
		OutError = FString::Printf(TEXT("MoQ FFI library at %s is missing required symbols. It may be outdated or corrupted; see ThirdParty/moq-ffi/README.md for refresh instructions."),
			*Library.GetLibraryPath());
		return false;
	}

	// TRF-29: moq_version is a required export (checked above), so an empty version here means
	// the library returned null, which is a mismatch too. moq-ffi exports no structured ABI
	// version; until it does, the Draft 07 marker in the version string is the only build check
	// available (the shipped DLL reports "moq_ffi 0.1.0 (IETF Draft 07)").
	const FString Version = GetVersion(Library);
	if (Version.IsEmpty() || !Version.Contains(TEXT("Draft 07")))
	{
		OutError = FString::Printf(TEXT("MoQ FFI build mismatch (reported '%s'). Rebuild moq-ffi with --features with_moq_draft07."), *Version);
		return false;
	}

	UE_LOG(LogMoQFfiSupport, Log, TEXT("MoQ FFI version: %s"), *Version);
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ
