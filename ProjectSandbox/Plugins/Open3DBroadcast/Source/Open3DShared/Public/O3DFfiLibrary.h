// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
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

	/** IPluginManager, FPaths::FileExists and FPlatformProcess Get/Free/GetDllExport. */
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
 * One FFI shared library used by a transport module, and the transport instances created from it
 * (TRF-14, TRF-28; ADR 0007 items 4, 5 and 7).
 *
 * Every FFI transport loads its library through this class, so path lookup, load-status
 * reporting and the unload rule are written once. Module use:
 *
 *   StartupModule:  construct, Load(); register factories only if Load() (and any
 *                   library-specific validation) succeeded. Factories wrap each new instance
 *                   in TrackInstance().
 *   ShutdownModule: unregister factories, StopLiveInstances(), then Unload().
 *
 * Unload() refuses to free the library while any tracked instance is still referenced (for
 * example by a component or LiveLink source that outlives the module during editor exit). The
 * library then stays loaded until process exit, so code in the library that such an instance
 * may still run (its destructor, a late callback) stays mapped. The destructor never frees the
 * library; only Unload() does.
 *
 * Threading: Load, Unload and StopLiveInstances run on the game thread (module startup and
 * shutdown). TrackInstance and GetNumLiveInstances may be called from any thread.
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
	 * Records Instance so StopLiveInstances() can stop it and Unload() can see it is still
	 * referenced. Holds only a weak reference. InstanceType must have a Stop() method.
	 */
	template <typename InstanceType>
	TSharedRef<InstanceType, ESPMode::ThreadSafe> TrackInstance(const TSharedRef<InstanceType, ESPMode::ThreadSafe>& Instance)
	{
		const TWeakPtr<InstanceType, ESPMode::ThreadSafe> Weak = Instance;
		FLiveInstance Entry;
		Entry.IsAlive = [Weak]() { return Weak.IsValid(); };
		Entry.Stop = [Weak]()
		{
			if (const TSharedPtr<InstanceType, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Stop();
			}
		};
		AddLiveInstance(MoveTemp(Entry));
		return Instance;
	}

	/** Calls Stop() on every tracked instance that is still alive. Returns how many were stopped. */
	int32 StopLiveInstances();

	/** Tracked instances that are still referenced somewhere. Drops the ones that are gone. */
	int32 GetNumLiveInstances() const;

	/**
	 * Frees the library unless a tracked instance is still referenced, in which case it logs a
	 * Warning and keeps the library loaded until process exit.
	 */
	EO3DFfiUnloadResult Unload();

private:
	struct FLiveInstance
	{
		TFunction<bool()> IsAlive;
		TFunction<void()> Stop;
	};

	void AddLiveInstance(FLiveInstance&& Entry);
	/** Removes dead entries and returns the live count. Caller holds InstancesMutex. */
	int32 PruneLocked() const;

	FO3DFfiLibraryDesc Desc;
	FO3DFfiLibraryOps Ops;
	void* Handle = nullptr;
	FString LibraryPath;
	FString StatusMessage;

	mutable FCriticalSection InstancesMutex;
	mutable TArray<FLiveInstance> Instances;
};
