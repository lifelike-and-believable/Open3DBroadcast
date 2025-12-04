#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQFfiSupport.h"
#include "Sender/MoQSender.h"
#include "Receiver/MoQReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"

#define LOCTEXT_NAMESPACE "Open3DTransportMoQ"

/**
 * Open3DTransportMoQ Module
 *
 * Phase 4 status:
 * - Loads and validates the moq-ffi runtime
 * - Wires the async dispatcher used by the session wrapper
 * - Registers sender and receiver factories with audio support
 */
class FOpen3DTransportMoQModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Load the MoQ FFI DLL using the centralized support class
		if (!FMoQFfiSupport::LoadLibrary())
		{
			UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to load MoQ FFI library: %s"), *FMoQFfiSupport::GetStatusMessage());
			return;
		}

		// Initialize the MoQ FFI crypto provider - must be called before any TLS operations
		// BUG-1 fix: Check return value of moq_init()
		if (!moq_init())
		{
			UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to initialize MoQ FFI crypto provider"));
			FMoQFfiSupport::UnloadLibrary();
			return;
		}

		// Ensure dispatcher is ready for wrapper usage
		FMoQAsyncDispatcher::Get().Initialize();
	
		RegisterTransports();

		UE_LOG(LogO3DMoQSender, Log, TEXT("Open3D MoQ transport module started (Phase 4 - sender/receiver with audio support)"));
	}

	virtual void ShutdownModule() override
	{
		FMoQAsyncDispatcher::Get().Shutdown();

		// Unregister transports
		UnregisterTransports();

		// Unload the MoQ FFI library
		FMoQFfiSupport::UnloadLibrary();

		UE_LOG(LogO3DMoQSender, Log, TEXT("Open3D MoQ transport module shut down"));
	}

private:

	void RegisterTransports()
	{
		// Register sender factory
		O3DTransport::RegisterSender(
			TEXT("MoQ"),
			[]() -> TSharedPtr<IOpen3DSender>
			{
				return MakeShared<FO3DMoQSender>();
			}
		);

		// Register receiver factory
		O3DTransport::RegisterReceiver(
			TEXT("MoQ"),
			[]() -> TSharedPtr<IOpen3DReceiver>
			{
				return MakeShared<FO3DMoQReceiver>();
			}
		);

		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport factories registered"));
	}

	void UnregisterTransports()
	{
		O3DTransport::UnregisterSender(TEXT("MoQ"));
		O3DTransport::UnregisterReceiver(TEXT("MoQ"));
		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport factories unregistered"));
	}
};

#undef LOCTEXT_NAMESPACE

#else // O3D_WITH_TRANSPORT_MOQ

/**
 * Stub module compiled when O3D_WITH_TRANSPORT_MOQ=0 so builds can exclude
 * the transport without pulling in third-party dependencies.
 */
class FOpen3DTransportMoQModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogTemp, Display, TEXT("Open3D MoQ transport disabled at build time (O3D_WITH_TRANSPORT_MOQ=0)."));
	}

	virtual void ShutdownModule() override {}
};

#endif // O3D_WITH_TRANSPORT_MOQ

IMPLEMENT_MODULE(FOpen3DTransportMoQModule, Open3DTransportMoQ)
