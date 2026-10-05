// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DFfiLibrary.h"

/**
 * moq-ffi specifics on top of the shared FO3DFfiLibrary loader (TRF-28): where moq_ffi lives in
 * the plugin, and the checks that the loaded build is the one this module was written for.
 *
 * Loading, unloading and live-instance tracking are FO3DFfiLibrary's job; the module owns the
 * library object (see Open3DTransportMoQModule.cpp).
 */
class FMoQFfiSupport
{
public:
	/** Location of moq_ffi relative to the Open3DBroadcast plugin. */
	static FO3DFfiLibraryDesc MakeLibraryDesc();

	/**
	 * Checks that every export this module binds is present and that the build is the Draft 07
	 * one (TRF-29). Returns false and fills OutError on the first problem found.
	 */
	static bool ValidateLibrary(const FO3DFfiLibrary& Library, FString& OutError);

	/** moq_version() of the loaded library, or empty when not loaded or missing. */
	static FString GetVersion(const FO3DFfiLibrary& Library);
};
