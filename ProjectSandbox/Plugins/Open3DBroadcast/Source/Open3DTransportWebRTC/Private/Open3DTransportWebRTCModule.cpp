#include "Modules/ModuleManager.h"
#include "WebRTCSender.h"
#include "WebRTCReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"

DEFINE_LOG_CATEGORY(LogO3DWebRTCTransport);

#define LOCTEXT_NAMESPACE "Open3DTransportWebRTC"

class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Register WebRTC sender factory
		O3DTransport::RegisterSender(TEXT("WebRTC"), []() { return MakeShared<FO3DWebRTCSender>(); });

		// Register WebRTC receiver factory
		O3DTransport::RegisterReceiver(TEXT("WebRTC"), []() { return MakeShared<FO3DWebRTCReceiver>(); });

		UE_LOG(LogO3DWebRTCTransport, Log, TEXT("Open3D WebRTC transport module started (LiveKit FFI backend)"));
	}

	virtual void ShutdownModule() override
	{
		// Unregister transport factories
		O3DTransport::UnregisterSender(TEXT("WebRTC"));
		O3DTransport::UnregisterReceiver(TEXT("WebRTC"));

		UE_LOG(LogO3DWebRTCTransport, Log, TEXT("Open3D WebRTC transport module shut down"));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportWebRTCModule, Open3DTransportWebRTC)

#undef LOCTEXT_NAMESPACE
