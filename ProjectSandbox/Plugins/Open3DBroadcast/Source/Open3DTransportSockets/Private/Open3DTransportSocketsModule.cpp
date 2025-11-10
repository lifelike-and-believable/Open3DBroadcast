#include "Modules/ModuleManager.h"

class FOpen3DTransportSocketsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FOpen3DTransportSocketsModule, Open3DTransportSockets)
