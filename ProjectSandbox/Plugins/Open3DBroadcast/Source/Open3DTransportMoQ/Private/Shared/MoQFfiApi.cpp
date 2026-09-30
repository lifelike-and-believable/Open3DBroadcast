// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "MoQFfiApi.h"

#include "Async/Async.h"

namespace
{
	FMoQFfiApiRef BuildProductionApi()
	{
		TSharedRef<FMoQFfiApi, ESPMode::ThreadSafe> Api = MakeShared<FMoQFfiApi, ESPMode::ThreadSafe>();

		// Taking the address of a delay-loaded import does not load the DLL; the first call does,
		// after FMoQFfiSupport has loaded and validated it.
		Api->Init = &moq_init;
		Api->ClientCreate = &moq_client_create;
		Api->ClientDestroy = &moq_client_destroy;
		Api->Connect = &moq_connect;
		Api->Disconnect = &moq_disconnect;
		Api->AnnounceNamespace = &moq_announce_namespace;
		Api->CreatePublisherEx = &moq_create_publisher_ex;
		Api->PublisherDestroy = &moq_publisher_destroy;
		Api->PublishData = &moq_publish_data;
		Api->Subscribe = &moq_subscribe;
		Api->SubscriberDestroy = &moq_subscriber_destroy;
		Api->FreeStr = &moq_free_str;
		Api->Version = &moq_version;
		Api->LastError = &moq_last_error;

		Api->LaunchBlocking = [](TUniqueFunction<void()>&& Work)
		{
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, MoveTemp(Work));
		};

		return Api;
	}
}

FMoQFfiApiRef FMoQFfiApi::GetProduction()
{
	static const FMoQFfiApiRef Production = BuildProductionApi();
	return Production;
}

TArray<const TCHAR*> FMoQFfiApi::GetRequiredSymbolNames()
{
	// Keep in step with BuildProductionApi(): one entry per bound export.
	return TArray<const TCHAR*>{
		TEXT("moq_init"),
		TEXT("moq_client_create"),
		TEXT("moq_client_destroy"),
		TEXT("moq_connect"),
		TEXT("moq_disconnect"),
		TEXT("moq_announce_namespace"),
		TEXT("moq_create_publisher_ex"),
		TEXT("moq_publisher_destroy"),
		TEXT("moq_publish_data"),
		TEXT("moq_subscribe"),
		TEXT("moq_subscriber_destroy"),
		TEXT("moq_free_str"),
		TEXT("moq_version"),
		TEXT("moq_last_error"),
	};
}

namespace MoQFfi
{
	FString CopyLastErrorOnThisThread(const FMoQFfiApi& Api)
	{
		if (!Api.LastError)
		{
			return FString();
		}

		// Copy at once: the buffer is reused by the next moq_last_error() call on this thread.
		const char* Raw = Api.LastError();
		return Raw != nullptr ? FString(UTF8_TO_TCHAR(Raw)) : FString();
	}

	FString CopyAndFreeString(const FMoQFfiApi& Api, const char* FfiString)
	{
		if (FfiString == nullptr)
		{
			return FString();
		}

		const FString Result = UTF8_TO_TCHAR(FfiString);
		if (Api.FreeStr)
		{
			Api.FreeStr(FfiString);
		}
		return Result;
	}
}

#endif // O3D_WITH_TRANSPORT_MOQ
