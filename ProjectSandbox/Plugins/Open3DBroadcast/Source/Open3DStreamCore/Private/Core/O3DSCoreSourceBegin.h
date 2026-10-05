// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// Included first by every generated O3DSCore_*.cpp in this folder, before the
// mirrored o3ds core source it compiles; O3DSCoreSourceEnd.h closes what this
// opens. No #pragma once: each translation unit includes it exactly once.
//
// HAL/Platform.h defines DLLEXPORT and DLLIMPORT, which OPEN3DSTREAMCORE_API (and
// so O3DS_API, see Open3DStreamCore.Build.cs) expands to in modular builds. It
// brings in no UObject, logging or assertion macros.
#include "HAL/Platform.h"

// The core is compiled here from a generated copy of src/o3ds, which is
// third-party code as far as this plugin is concerned: it has its own warning
// checks (the GCC warning ratchet and the MSVC build in core-tests.yml), and it
// is not edited in the plugin tree. Its warnings are switched off so that the
// engine's warning settings, and -FailOnWarnings in CI, apply to plugin code
// only (docs/adr/0003-core-library-delivery-to-plugin.md).
THIRD_PARTY_INCLUDES_START
#if defined(_MSC_VER) && !defined(__clang__)
	#pragma warning(push, 0)
#elif defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wall"
	#pragma GCC diagnostic ignored "-Wextra"
#endif
