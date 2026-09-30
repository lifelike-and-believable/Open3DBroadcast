// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Module hooks for the deprecated O3DSender::FindTransportCustomization (WP-A1 PR 1). Removed with
// the shims in the next minor release.
namespace O3DSenderLegacyShims
{
	/** Subscribes the customization cache to FO3DTransportRegistry::OnTransportsChanged. Module startup. */
	void StartCustomizationCache();

	/** Unsubscribes and empties the cache, so no transport function outlives its module. Module shutdown. */
	void StopCustomizationCache();
}
