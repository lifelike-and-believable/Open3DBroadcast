// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace O3DTests
{
	/**
	 * Registers the offline profiles for Fake, Loopback, TCP, UDP, NNG and MoQ (those compiled
	 * in), and records the WebRTC deferral to WP-F11. Called from StartupModule.
	 */
	void RegisterBuiltInConformanceProfiles();
	void UnregisterBuiltInConformanceProfiles();
}

#endif // WITH_DEV_AUTOMATION_TESTS
