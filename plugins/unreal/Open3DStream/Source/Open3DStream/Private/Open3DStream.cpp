// Copyright Epic Games, Inc. All Rights Reserved.

#include "Open3DStream.h"
#include "O3DSStreamLogs.h"
#include "O3DSLoopback.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Open3DStreamSource.h"
#include "Open3DStreamSourceSettings.h"

#define LOCTEXT_NAMESPACE "FOpen3DStreamModule"

// Define receiver-specific log categories
DEFINE_LOG_CATEGORY(LogO3DSReceiver);
DEFINE_LOG_CATEGORY(LogO3DSReceiverWebRTC);
DEFINE_LOG_CATEGORY(LogO3DSReceiverAudio);

void FOpen3DStreamModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	UE_LOG(LogO3DSReceiver, Display, TEXT("Open3DStream module started"));

	// Register loopback consumer factory so Broadcast can forward serialized frames without a hard dependency
	FSerializedFrameConsumerRegistry::RegisterFactory([]() -> TSharedPtr<ISerializedFrameConsumer>
	{
		class FLiveLinkSerializedFrameConsumer final : public ISerializedFrameConsumer
		{
		public:
			FLiveLinkSerializedFrameConsumer()
			{
				// Create an in-memory source without sockets; we will feed OnPackage directly
				FOpen3DStreamSettings Settings = GetDefault<UOpen3DStreamSettingsObject>()->Settings;
				Settings.Protocol = FText::FromString(TEXT("InMemory"));
				Settings.Url = FText::FromString(TEXT("mem://loopback"));

				Source = MakeShared<FOpen3DStreamSource>(Settings);

				if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
				{
					ILiveLinkClient* LiveLinkClient = &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
					if (LiveLinkClient)
					{
						LiveLinkClient->AddSource(Source.ToSharedRef());
					}
				}
			}

			virtual ~FLiveLinkSerializedFrameConsumer()
			{
				if (Source.IsValid())
				{
					if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
					{
						ILiveLinkClient* LiveLinkClient = &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
						if (LiveLinkClient)
						{
							LiveLinkClient->RemoveSource(Source->SourceGuid);
						}
					}
				}
			}

			virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double /*TimestampSeconds*/) override
			{
				if (!Source.IsValid())
				{
					return;
				}
				Source->OnPackage(Buffer);
			}

		private:
			TSharedPtr<FOpen3DStreamSource> Source;
		};

		return MakeShared<FLiveLinkSerializedFrameConsumer>();
	});
}

void FOpen3DStreamModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
    FSerializedFrameConsumerRegistry::ClearFactory();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOpen3DStreamModule, Open3DStream)