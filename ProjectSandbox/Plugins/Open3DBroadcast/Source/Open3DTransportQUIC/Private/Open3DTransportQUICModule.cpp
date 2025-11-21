#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#include "Sender/QuicSender.h"
#include "O3DSenderRegistry.h"

#include "Open3DTransportQUICLog.h"

DEFINE_LOG_CATEGORY(LogOpen3DTransportQUIC);

class FOpen3DTransportQUICModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		O3DTransport::RegisterSender(TEXT("QUIC"), []() { return MakeShared<FO3DQuicSender>(); });
		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("Open3D QUIC transport module initialized."));
	}

	virtual void ShutdownModule() override
	{
		O3DTransport::UnregisterSender(TEXT("QUIC"));
		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("Open3D QUIC transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportQUICModule, Open3DTransportQUIC)
