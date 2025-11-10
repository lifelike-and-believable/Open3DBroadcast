#include "Modules/ModuleManager.h"

class FOpen3DTransportNNGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FOpen3DTransportNNGModule, Open3DTransportNNG)
