// Copyright (c) Open3DStream Contributors

#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Conformance/O3DConformanceProfiles.h"
#endif

/**
 * Editor-only module that holds the plugin's automation tests, fakes and the transport
 * conformance suite (ADR 0006, WP-T2). It registers the built-in conformance profiles at startup;
 * the tests themselves self-register through the automation macros.
 */
class FOpen3DBroadcastTestsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_DEV_AUTOMATION_TESTS
		O3DTests::RegisterBuiltInConformanceProfiles();
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_DEV_AUTOMATION_TESTS
		O3DTests::UnregisterBuiltInConformanceProfiles();
#endif
	}
};

IMPLEMENT_MODULE(FOpen3DBroadcastTestsModule, Open3DBroadcastTests)
