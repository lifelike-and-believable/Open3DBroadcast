#include "Modules/ModuleManager.h"

class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FOpen3DTransportWebRTCModule, Open3DTransportWebRTC)
