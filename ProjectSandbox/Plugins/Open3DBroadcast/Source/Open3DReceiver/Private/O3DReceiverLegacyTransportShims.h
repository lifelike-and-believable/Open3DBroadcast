// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

struct FO3DReceiverSourceConfig;

// Module hooks for the deprecated O3DReceiver::FindTransportCustomization (WP-A1 PR 1). Removed
// with the shims in the next minor release.
namespace O3DReceiverLegacyShims
{
	/** Subscribes the customization cache to FO3DTransportRegistry::OnTransportsChanged. Module startup. */
	void StartCustomizationCache();

	/** Unsubscribes and empties the cache, so no transport function outlives its module. Module shutdown. */
	void StopCustomizationCache();

	/**
	 * WP-A1 PR 5a: a configure function registered through the deprecated
	 * RegisterTransportCustomization still takes the source settings. The descriptor gets an
	 * adapter with the new signature, which passes it the settings this scope names (defaults
	 * outside one). The receiver source opens one around its call to ConfigureReceiver. Game thread.
	 */
	class FScopedConfiguringSettings
	{
	public:
		explicit FScopedConfiguringSettings(const FO3DReceiverSourceConfig& Settings);
		~FScopedConfiguringSettings();

		FScopedConfiguringSettings(const FScopedConfiguringSettings&) = delete;
		FScopedConfiguringSettings& operator=(const FScopedConfiguringSettings&) = delete;

	private:
		const FO3DReceiverSourceConfig* Previous = nullptr;
	};
}
