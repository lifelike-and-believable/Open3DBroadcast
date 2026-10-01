// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

/**
 * Where a transport's FFI shared library lives (ADR 0007 item 7, TRF-28).
 *
 * The path is relative to the base directory of the plugin that owns the transport module, so
 * a transport in an add-on plugin (WP-F11) names its own plugin rather than Open3DBroadcast.
 */
struct FO3DFfiLibraryDesc
{
	/** Name used in log messages, for example "MoQ FFI". */
	FString DisplayName;
	/** Plugin whose base directory RelativePath starts from, for example "Open3DBroadcast". */
	FString OwningPluginName;
	/** Library file relative to the plugin base directory, with forward slashes. */
	FString RelativePath;
	/**
	 * Registered transport names whose instances run code from this library, for example
	 * {"MoQ"}. Unload() refuses to free the library while FO3DTransportRegistry reports a live
	 * instance of any of them (ADR 0007 item 5, WP-A1 PR 2).
	 */
	TArray<FName> TransportNames;
};

/**
 * Platform calls FO3DFfiLibrary makes. Production code uses MakePlatform(). Tests pass fakes so
 * load and unload decisions can be checked without a real DLL (ADR 0006 option F2).
 */
struct OPEN3DSHARED_API FO3DFfiLibraryOps
{
	/** Absolute base directory of the named plugin, or empty when the plugin is not found. */
	TFunction<FString(const FString& PluginName)> FindPluginBaseDir;
	TFunction<bool(const FString& Path)> FileExists;
	/** Returns the library handle, or null on failure. */
	TFunction<void*(const FString& Path)> LoadDll;
	TFunction<void(void* Handle)> FreeDll;
	/** Returns the exported symbol, or null when it is missing. */
	TFunction<void*(void* Handle, const TCHAR* SymbolName)> GetExport;
	/**
	 * Live instances of one transport name. Unset counts as none. Tests point it at their own
	 * FO3DTransportRegistry.
	 */
	TFunction<int32(FName TransportName)> CountLiveInstances;

	/**
	 * IPluginManager, FPaths::FileExists, FPlatformProcess Get/Free/GetDllExport and
	 * FO3DTransportRegistry::Get().GetNumLiveInstances.
	 */
	static FO3DFfiLibraryOps MakePlatform();
};

/** What FO3DFfiLibrary::Unload did. */
enum class EO3DFfiUnloadResult : uint8
{
	/** Nothing was loaded. */
	NotLoaded,
	/** The library handle was freed. */
	Unloaded,
	/** Transport instances were still referenced, so the library stays loaded until process exit. */
	KeptLoaded,
};

/**
 * One FFI shared library used by a transport module (TRF-14, TRF-28; ADR 0007 items 4, 5 and 7).
 *
 * Every FFI transport loads its library through this class, so path lookup, load-status
 * reporting and the unload rule are written once. Module use:
 *
 *   StartupModule:  construct with Desc.TransportNames set, Load(); register the transport only
 *                   if Load() (and any library-specific validation) succeeded.
 *   ShutdownModule: reset the FO3DTransportRegistration first. That drains the transport: its
 *                   owners stop and release their instances and the registry stops the rest
 *                   (FO3DTransportRegistry, OnTransportUnregistering). Then Unload().
 *
 * The registry tracks the instances (WP-A1 PR 2); this class only asks it. Unload() refuses to
 * free the library while the registry reports a live instance of one of Desc.TransportNames (for
 * example one a component that did not release it still holds during editor exit). The library
 * then stays loaded until process exit, so code in the library that such an instance may still
 * run (its destructor, a late callback) stays mapped. The destructor never frees the library;
 * only Unload() does.
 *
 * Threading: Load and Unload run on the game thread (module startup and shutdown).
 * GetNumLiveInstances may be called from any thread.
 */
class OPEN3DSHARED_API FO3DFfiLibrary
{
public:
	explicit FO3DFfiLibrary(const FO3DFfiLibraryDesc& InDesc);
	FO3DFfiLibrary(const FO3DFfiLibraryDesc& InDesc, FO3DFfiLibraryOps InOps);
	~FO3DFfiLibrary();

	FO3DFfiLibrary(const FO3DFfiLibrary&) = delete;
	FO3DFfiLibrary& operator=(const FO3DFfiLibrary&) = delete;

	/**
	 * Resolves the path from the owning plugin and loads the library. Logs an Error and returns
	 * false when the plugin, the file or the load fails. Returns true if already loaded.
	 */
	bool Load();

	bool IsLoaded() const { return Handle != nullptr; }

	/** Exported symbol, or null when not loaded or missing. */
	void* GetExport(const TCHAR* SymbolName) const;

	/** Full path of the library (set by Load(), also on failure once the plugin was found). */
	const FString& GetLibraryPath() const { return LibraryPath; }

	/** Human-readable load state for logs and diagnostics. */
	const FString& GetStatusMessage() const { return StatusMessage; }

	const FO3DFfiLibraryDesc& GetDesc() const { return Desc; }

	/**
	 * Instances of Desc.TransportNames that are still referenced somewhere, as the transport
	 * registry counts them (including leaks left by an earlier unregister).
	 */
	int32 GetNumLiveInstances() const;

	/**
	 * Frees the library unless an instance of one of its transports is still referenced, in which
	 * case it logs a Warning and keeps the library loaded until process exit. Call it after the
	 * transport's registration is reset, so the drain has run.
	 */
	EO3DFfiUnloadResult Unload();

private:
	FO3DFfiLibraryDesc Desc;
	FO3DFfiLibraryOps Ops;
	void* Handle = nullptr;
	FString LibraryPath;
	FString StatusMessage;
};
