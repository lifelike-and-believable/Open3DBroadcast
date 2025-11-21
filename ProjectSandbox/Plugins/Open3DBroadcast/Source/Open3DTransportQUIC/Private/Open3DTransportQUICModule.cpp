#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportQUIC, Log, All);

class FOpen3DTransportQUICModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("Open3D QUIC transport module initialized (stub)."));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("Open3D QUIC transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportQUICModule, Open3DTransportQUIC)
