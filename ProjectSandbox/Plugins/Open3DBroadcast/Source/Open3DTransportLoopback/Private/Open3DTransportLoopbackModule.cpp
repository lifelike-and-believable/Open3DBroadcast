// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "Sender/LoopbackSender.h"
#include "Receiver/LoopbackReceiver.h"
#include "Shared/LoopbackChannel.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DTransportTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportLoopbackModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportLoopback"

namespace LoopbackSchema
{
	/** Channel name row, shared by both roles (ADR 0010 §4: the editor module renders it). */
	static FO3DTransportOptionField MakeChannelField()
	{
		FO3DTransportOptionField Field;
		Field.Key = O3DLoopback::ChannelOptionKey;
		Field.DisplayName = LOCTEXT("LoopbackChannelLabel", "Channel Name");
		Field.Tooltip = LOCTEXT("LoopbackChannelTooltip", "In-process channel the sender publishes to and the receiver reads from. Empty uses 'default'.");
		Field.Type = EO3DTransportOptionType::String;
		Field.Default = O3DLoopback::DefaultChannel;
		return Field;
	}

	static FO3DTransportOptionSchema MakeReceiverSchema()
	{
		return { MakeChannelField() };
	}

	static FO3DTransportOptionSchema MakeSenderSchema()
	{
		FO3DTransportOptionField Queue;
		Queue.Key = O3DLoopback::QueueOptionKey;
		Queue.DisplayName = LOCTEXT("LoopbackSenderQueueLabel", "Queue Capacity");
		Queue.Tooltip = LOCTEXT("LoopbackSenderQueueTooltip", "Frames the loopback channel buffers; while it is full, new frames are refused (DroppedBackpressure).");
		Queue.Type = EO3DTransportOptionType::Int;
		Queue.Default = FString::FromInt(O3DLoopback::DefaultQueueCapacity);
		Queue.Min = 1;
		Queue.Max = 4096;

		return { MakeChannelField(), MoveTemp(Queue) };
	}

	/**
	 * The channel option. The sender component and the receiver source copy their (non-secret)
	 * transport options into Config.AdvancedParams before the configure function runs, so the
	 * functions read only the config and this module needs neither Open3DSender nor
	 * Open3DReceiver (ADR 0007 step 4).
	 */
	static FString ReadChannel(const FO3DTransportConfig& Config)
	{
		return O3DTransportOptions::GetString(Config.AdvancedParams, O3DLoopback::ChannelOptionKey, O3DLoopback::DefaultChannel);
	}
}

class FOpen3DTransportLoopbackModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// One descriptor for the transport name (ADR 0007 item 4, WP-A1).
		FO3DTransportDescriptor Loopback;
		Loopback.Name = TEXT("Loopback");
		Loopback.OwningModule = TEXT("Open3DTransportLoopback");
		Loopback.CreateSender = []() { return MakeShared<FO3DLoopbackSender>(); };
		Loopback.CreateReceiver = []() { return MakeShared<FO3DLoopbackReceiver>(); };
		Loopback.GetCapabilities = [](const FO3DTransportConfig& Config) { return O3DLoopback::GetCapabilities(Config); };

		Loopback.ConfigureReceiver = [](const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& Config)
		{
			const FString ChannelName = LoopbackSchema::ReadChannel(Config);
			Config.Transport = TEXT("Loopback");
			Config.Role = EO3DTransportRole::Receiver;
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=sub"), *ChannelName);
			Config.AdvancedParams.Add(O3DLoopback::ChannelOptionKey, ChannelName);
		};
		Loopback.ReceiverOptions.OptionSchema = LoopbackSchema::MakeReceiverSchema();

		Loopback.ConfigureSender = [](const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& Config)
		{
			const FString ChannelName = LoopbackSchema::ReadChannel(Config);
			Config.Transport = TEXT("Loopback");
			Config.Role = EO3DTransportRole::Sender;
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=pub"), *ChannelName);
			Config.AdvancedParams.Add(O3DLoopback::ChannelOptionKey, ChannelName);

			int32 QueueValue = O3DTransportOptions::GetInt(Config.AdvancedParams, O3DLoopback::QueueOptionKey, O3DLoopback::DefaultQueueCapacity);
			QueueValue = QueueValue > 0 ? QueueValue : O3DLoopback::DefaultQueueCapacity;
			Config.AdvancedParams.Add(O3DLoopback::QueueOptionKey, FString::FromInt(QueueValue));
		};
		Loopback.SenderOptions.OptionSchema = LoopbackSchema::MakeSenderSchema();

		Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Loopback));

		UE_LOG(LogOpen3DTransportLoopbackModule, Log, TEXT("Open3D loopback transport module started."));
	}

	virtual void ShutdownModule() override
	{
		// Unregistering drains the transport (ADR 0007 item 5, WP-A1 PR 2): sender components and
		// LiveLink sources stop and release their instances, and the registry stops and reports any
		// left, before this module's code goes away. There is no FFI handle to free afterwards.
		Registration.Reset();

		UE_LOG(LogOpen3DTransportLoopbackModule, Log, TEXT("Open3D loopback transport module shut down."));
	}

private:
	FO3DTransportRegistration Registration;
};

IMPLEMENT_MODULE(FOpen3DTransportLoopbackModule, Open3DTransportLoopback)

#undef LOCTEXT_NAMESPACE
