#pragma once

#include "CoreMinimal.h"

/**
 * Static utility for loading and validating the moq_ffi.dll/.so/.dylib
 * at runtime. Provides version checking and diagnostic logging.
 *
 * Usage:
 *   FMoQFfiSupport::LoadLibrary();  // Call once at module startup
 *   FMoQFfiSupport::UnloadLibrary(); // Call once at module shutdown
 */
class FMoQFfiSupport
{
public:
	/**
	 * Load the MoQ FFI shared library (moq_ffi.dll on Windows).
	 * Must be called before any MoQ FFI functions are used.
	 *
	 * @return true if loaded successfully, false otherwise
	 */
	static bool LoadLibrary();

	/**
	 * Unload the MoQ FFI shared library.
	 * Should be called during module shutdown.
	 */
	static void UnloadLibrary();

	/**
	 * Check if the MoQ FFI library is currently loaded.
	 *
	 * @return true if loaded, false otherwise
	 */
	static bool IsLoaded();

	/**
	 * Get the version string of the loaded MoQ FFI library.
	 * Only valid if IsLoaded() returns true.
	 *
	 * @return Version string (e.g., "0.1.0") or empty if not loaded
	 */
	static FString GetVersion();

	/**
	 * Get the path to the loaded MoQ FFI library.
	 * Only valid if IsLoaded() returns true.
	 *
	 * @return Full path to the loaded library or empty if not loaded
	 */
	static FString GetLibraryPath();

	/**
	 * Get a human-readable status message describing library load state.
	 * Useful for diagnostic logging and error reporting.
	 *
	 * @return Status message
	 */
	static FString GetStatusMessage();

private:
	static void* LibraryHandle;
	static FString LibraryPath;
	static FString StatusMessage;
	static bool bIsLoaded;

	/** Construct the platform-specific path to the MoQ FFI library */
	static FString ConstructLibraryPath();

	/** Validate library after loading (check for required symbols, etc.) */
	static bool ValidateLibrary();
};
