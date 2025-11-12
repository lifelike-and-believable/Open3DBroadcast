#include "Modules/ModuleManager.h"
#include "O3DReceiverLogs.h"

DEFINE_LOG_CATEGORY(LogO3DReceiver);
DEFINE_LOG_CATEGORY(LogO3DReceiverSource);
DEFINE_LOG_CATEGORY(LogO3DReceiverAudio);

/** Module entry for the runtime LiveLink receiver integration. */
class FOpen3DReceiverModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogO3DReceiver, Display, TEXT("Open3DReceiver module started"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogO3DReceiver, Display, TEXT("Open3DReceiver module shut down"));
	}
};

IMPLEMENT_MODULE(FOpen3DReceiverModule, Open3DReceiver)
