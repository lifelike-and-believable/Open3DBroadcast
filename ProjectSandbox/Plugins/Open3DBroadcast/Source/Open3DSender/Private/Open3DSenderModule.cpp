// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "O3DSenderLogs.h"

DEFINE_LOG_CATEGORY(LogO3DSender);
DEFINE_LOG_CATEGORY(LogO3DSenderComponent);
DEFINE_LOG_CATEGORY(LogO3DSenderSerializer);
DEFINE_LOG_CATEGORY(LogO3DSenderAudio);

/**
 * Module entry point for the Open3DStream sender runtime. The Details panel customization lives in
 * the Open3DBroadcastEditor module (ADR 0010).
 */
class FOpen3DSenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogO3DSender, Verbose, TEXT("Open3DSender module started"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogO3DSender, Verbose, TEXT("Open3DSender module shutdown"));
	}
};

IMPLEMENT_MODULE(FOpen3DSenderModule, Open3DSender)
