// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "Sender/LoopbackSender.h"
#include "Receiver/LoopbackReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DTransportOptionSchema.h"
#include "O3DTransportTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportLoopbackModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportLoopback"

namespace LoopbackReceiver
{
	static constexpr TCHAR ChannelOptionKey[] = TEXT("channel");
}

namespace LoopbackSender
{
	static constexpr TCHAR ChannelOptionKey[] = TEXT("channel");
	static constexpr TCHAR QueueOptionKey[] = TEXT("loopback.maxqueue");
	static constexpr int32 DefaultQueue = 64;
}

namespace LoopbackSchema
{
	static constexpr TCHAR DefaultChannel[] = TEXT("default");

	/** Channel name row, shared by both roles (ADR 0010 §4: the editor module renders it). */
	static FO3DTransportOptionField MakeChannelField(const TCHAR* Key)
	{
		FO3DTransportOptionField Field;
		Field.Key = Key;
		Field.DisplayName = LOCTEXT("LoopbackChannelLabel", "Channel Name");
		Field.Tooltip = LOCTEXT("LoopbackChannelTooltip", "In-process channel the sender publishes to and the receiver reads from. Empty uses 'default'.");
		Field.Type = EO3DTransportOptionType::String;
		Field.Default = DefaultChannel;
		return Field;
	}

	static FO3DTransportOptionSchema MakeReceiverSchema()
	{
		return { MakeChannelField(LoopbackReceiver::ChannelOptionKey) };
	}

	static FO3DTransportOptionSchema MakeSenderSchema()
	{
		FO3DTransportOptionField Queue;
		Queue.Key = LoopbackSender::QueueOptionKey;
		Queue.DisplayName = LOCTEXT("LoopbackSenderQueueLabel", "Queue Capacity");
		Queue.Tooltip = LOCTEXT("LoopbackSenderQueueTooltip", "Frames the loopback channel buffers before it drops the oldest.");
		Queue.Type = EO3DTransportOptionType::Int;
		Queue.Default = FString::FromInt(LoopbackSender::DefaultQueue);
		Queue.Min = 1;
		Queue.Max = 4096;

		return { MakeChannelField(LoopbackSender::ChannelOptionKey), MoveTemp(Queue) };
	}
}

class FOpen3DTransportLoopbackModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		O3DTransport::RegisterSender(TEXT("Loopback"), []() { return MakeShared<FO3DLoopbackSender>(); });
		O3DTransport::RegisterReceiver(TEXT("Loopback"), []() { return MakeShared<FO3DLoopbackReceiver>(); });

		FO3DReceiverTransportCustomization LoopbackCustomization;
		LoopbackCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			FString ChannelName;
			if (const FString* Option = Settings.TransportOptions.Find(LoopbackReceiver::ChannelOptionKey))
			{
				ChannelName = *Option;
			}
			if (ChannelName.IsEmpty())
			{
				ChannelName = LoopbackSchema::DefaultChannel;
			}

			Config.Transport = TEXT("Loopback");
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=sub"), *ChannelName);
			Config.AdvancedParams.Add(LoopbackReceiver::ChannelOptionKey, ChannelName);
		};
		LoopbackCustomization.OptionSchema = LoopbackSchema::MakeReceiverSchema();
		O3DReceiver::RegisterTransportCustomization(TEXT("Loopback"), MoveTemp(LoopbackCustomization));

		FO3DSenderTransportCustomization LoopbackSenderCustomization;
		LoopbackSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			FString ChannelName = SenderComponent ? SenderComponent->GetTransportOption(LoopbackSender::ChannelOptionKey) : FString();
			if (ChannelName.IsEmpty())
			{
				ChannelName = LoopbackSchema::DefaultChannel;
			}

			Config.Transport = TEXT("Loopback");
			Config.Role = TEXT("sender");
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=pub"), *ChannelName);
			Config.AdvancedParams.Add(LoopbackSender::ChannelOptionKey, ChannelName);

			FString QueueString = SenderComponent ? SenderComponent->GetTransportOption(LoopbackSender::QueueOptionKey) : FString();
			int32 QueueValue = QueueString.IsEmpty() ? LoopbackSender::DefaultQueue : FMath::Max(1, FCString::Atoi(*QueueString));
			Config.AdvancedParams.Add(LoopbackSender::QueueOptionKey, FString::FromInt(QueueValue));
		};
		LoopbackSenderCustomization.OptionSchema = LoopbackSchema::MakeSenderSchema();
		O3DSender::RegisterTransportCustomization(TEXT("Loopback"), MoveTemp(LoopbackSenderCustomization));

		UE_LOG(LogOpen3DTransportLoopbackModule, Log, TEXT("Open3D loopback transport module started."));
	}

	virtual void ShutdownModule() override
	{
		O3DTransport::UnregisterSender(TEXT("Loopback"));
		O3DTransport::UnregisterReceiver(TEXT("Loopback"));
		O3DReceiver::UnregisterTransportCustomization(TEXT("Loopback"));
		O3DSender::UnregisterTransportCustomization(TEXT("Loopback"));

		UE_LOG(LogOpen3DTransportLoopbackModule, Log, TEXT("Open3D loopback transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportLoopbackModule, Open3DTransportLoopback)

#undef LOCTEXT_NAMESPACE
