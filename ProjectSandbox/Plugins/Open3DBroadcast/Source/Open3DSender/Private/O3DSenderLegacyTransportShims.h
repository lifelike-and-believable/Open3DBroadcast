// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

class UO3DSenderComponent;

// Module hooks for the deprecated O3DSender::FindTransportCustomization (WP-A1 PR 1). Removed with
// the shims in the next minor release.
namespace O3DSenderLegacyShims
{
	/** Subscribes the customization cache to FO3DTransportRegistry::OnTransportsChanged. Module startup. */
	void StartCustomizationCache();

	/** Unsubscribes and empties the cache, so no transport function outlives its module. Module shutdown. */
	void StopCustomizationCache();

	/**
	 * WP-A1 PR 5a: a configure function registered through the deprecated
	 * RegisterTransportCustomization still takes the component. The descriptor gets an adapter with
	 * the new signature, which passes it the component this scope names (null outside one). The
	 * component opens one around its call to the descriptor's ConfigureSender. Game thread.
	 */
	class FScopedConfiguringComponent
	{
	public:
		explicit FScopedConfiguringComponent(const UO3DSenderComponent* Component);
		~FScopedConfiguringComponent();

		FScopedConfiguringComponent(const FScopedConfiguringComponent&) = delete;
		FScopedConfiguringComponent& operator=(const FScopedConfiguringComponent&) = delete;

	private:
		const UO3DSenderComponent* Previous = nullptr;
	};
}
