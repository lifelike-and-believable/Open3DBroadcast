// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace O3DTests
{
	/**
	 * Registers the offline profiles for Fake, Loopback, TCP, UDP, NNG and MoQ (those compiled
	 * in), and records the deferral for the add-on WebRTC transport (WP-T2e). Called from StartupModule.
	 */
	void RegisterBuiltInConformanceProfiles();
	void UnregisterBuiltInConformanceProfiles();
}

#endif // WITH_DEV_AUTOMATION_TESTS
