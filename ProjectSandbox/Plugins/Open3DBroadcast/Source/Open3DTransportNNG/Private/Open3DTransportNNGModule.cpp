// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_NNG

#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DTransportTypes.h"
#include "Shared/NngHelpers.h"
#include "Sender/NngSender.h"
#include "Receiver/NngReceiver.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportNNGModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportNNG"

namespace NNGTransportCommon
{
	static constexpr uint64 DefaultQueueBytes = 4ull * 1024ull * 1024ull;

	// Defaults live in NngHelpers so the panels, ConfigureTransport and the parser agree (TRB-40).
	static int32 ResolveDefaultPort(O3DNNG::ENngMode Mode)
	{
		return O3DNNG::GetDefaultPort(Mode);
	}

	static FString ResolveDefaultHost(bool bListen)
	{
		return O3DNNG::GetDefaultHost(bListen);
	}

	static FString UInt64ToString(uint64 Value)
	{
		return LexToString(Value);
	}

	/**
	 * The configure functions read Config.AdvancedParams, which the sender component and the
	 * receiver source fill from their transport options before calling them (WP-A1 PR 4d), so
	 * this module needs neither Open3DSender nor Open3DReceiver.
	 */
	static FString GetOption(const FO3DTransportConfig& Config, const TCHAR* Key)
	{
		return O3DTransportOptions::GetString(Config.AdvancedParams, Key);
	}

	/** A port option: 1 to 65535 in digits, otherwise the mode's default (TRB-26: "80abc" is not port 80). */
	static int32 ReadPort(const FO3DTransportConfig& Config, O3DNNG::ENngMode Mode)
	{
		int32 Port = 0;
		return O3DTransportOptions::TryParsePort(GetOption(Config, O3DNNG::PortOptionKey), Port) ? Port : ResolveDefaultPort(Mode);
	}

	/** nng.qmax in bytes: a positive integer, otherwise the default. */
	static uint64 ReadQueueBytes(const FO3DTransportConfig& Config)
	{
		int64 Parsed = 0;
		if (O3DTransportOptions::TryParseInt(GetOption(Config, O3DNNG::QueueOptionKey), Parsed) && Parsed > 0)
		{
			return static_cast<uint64>(Parsed);
		}
		return DefaultQueueBytes;
	}
}

/**
 * Option schemas (ADR 0010 §4). The editor module renders them. Host and port have no single
 * default: ConfigureTransport picks them from the mode and role (NngHelpers, TRB-40), so the
 * tooltips say what an empty value means. The old combined mode+role list is two fields now
 * (ADR 0010 open question 5).
 */
namespace NNGSchema
{
	static constexpr int64 BytesPerMiB = 1024ll * 1024ll;

	static FO3DTransportOptionField MakeHost()
	{
		FO3DTransportOptionField Field;
		Field.Key = O3DNNG::HostOptionKey;
		Field.DisplayName = LOCTEXT("NNGHostLabel", "Host");
		Field.Tooltip = LOCTEXT("NNGHostTooltip", "Address to listen on or dial. Empty: 0.0.0.0 when the mode listens, 127.0.0.1 when it dials.");
		Field.Type = EO3DTransportOptionType::String;
		Field.Hint = LOCTEXT("NNGHostHint", "auto: from the mode");
		return Field;
	}

	static FO3DTransportOptionField MakePort()
	{
		FO3DTransportOptionField Field;
		Field.Key = O3DNNG::PortOptionKey;
		Field.DisplayName = LOCTEXT("NNGPortLabel", "Port");
		Field.Tooltip = LOCTEXT("NNGPortTooltip", "TCP port. Empty: 6000 for Pub/Sub, 7000 for Pair, 8000 for Push/Pull.");
		Field.Type = EO3DTransportOptionType::Int;
		Field.Hint = LOCTEXT("NNGPortHint", "auto: from the mode");
		Field.Min = 1;
		Field.Max = 65535;
		return Field;
	}

	static FO3DTransportOptionEnumValue MakeChoice(const TCHAR* Value, FText DisplayName)
	{
		FO3DTransportOptionEnumValue Choice;
		Choice.Value = Value;
		Choice.DisplayName = MoveTemp(DisplayName);
		return Choice;
	}

	/** Role row, shown only for the modes that can either listen or dial. */
	static FO3DTransportOptionField MakeRole(const TCHAR* DefaultMode, const TCHAR* ModeA, const TCHAR* ModeB)
	{
		FO3DTransportOptionField Field;
		Field.Key = O3DNNG::RoleOptionKey;
		Field.DisplayName = LOCTEXT("NNGRoleLabel", "Role");
		Field.Tooltip = LOCTEXT("NNGRoleTooltip", "Whether this end listens (server) or dials (client). Default: the usual role for the mode, so a default sender and a default receiver connect.");
		Field.Type = EO3DTransportOptionType::Enum;
		Field.EnumValues.Add(MakeChoice(TEXT(""), LOCTEXT("NNGRoleDefault", "Default for the mode")));
		Field.EnumValues.Add(MakeChoice(TEXT("server"), LOCTEXT("NNGRoleServer", "Listen (server)")));
		Field.EnumValues.Add(MakeChoice(TEXT("client"), LOCTEXT("NNGRoleClient", "Dial (client)")));
		const FString ModeKey = O3DNNG::ModeOptionKey;
		const FString ModeWhenUnset = DefaultMode;
		const FString FirstMode = ModeA;
		const FString SecondMode = ModeB;
		Field.VisibleWhen = [ModeKey, ModeWhenUnset, FirstMode, SecondMode](const TMap<FString, FString>& Options)
		{
			const FString Mode = O3DTransportOptions::GetOption(Options, ModeKey, ModeWhenUnset);
			return Mode.Equals(FirstMode, ESearchCase::IgnoreCase) || Mode.Equals(SecondMode, ESearchCase::IgnoreCase);
		};
		return Field;
	}

	static FO3DTransportOptionSchema MakeSender()
	{
		FO3DTransportOptionField Mode;
		Mode.Key = O3DNNG::ModeOptionKey;
		Mode.DisplayName = LOCTEXT("NNGModeLabel", "Mode");
		Mode.Tooltip = LOCTEXT("NNGSenderModeTooltip", "NNG protocol. Match it on the receiver: Publisher with Subscriber, Pair with Pair, Push with Pull.");
		Mode.Type = EO3DTransportOptionType::Enum;
		Mode.Default = TEXT("pub");
		Mode.EnumValues.Add(MakeChoice(TEXT("pub"), LOCTEXT("NNGModePub", "Publisher")));
		Mode.EnumValues.Add(MakeChoice(TEXT("pair"), LOCTEXT("NNGModePair", "Pair")));
		Mode.EnumValues.Add(MakeChoice(TEXT("push"), LOCTEXT("NNGModePush", "Push")));

		FO3DTransportOptionField Queue;
		Queue.Key = O3DNNG::QueueOptionKey;
		Queue.DisplayName = LOCTEXT("NNGSenderQueueLabel", "Queue Capacity (MiB)");
		Queue.Tooltip = LOCTEXT("NNGSenderQueueTooltip", "Bytes the sender queues for a slow receiver before it drops frames. Stored in bytes.");
		Queue.Type = EO3DTransportOptionType::Int;
		Queue.Default = NNGTransportCommon::UInt64ToString(NNGTransportCommon::DefaultQueueBytes);
		Queue.StoredUnitScale = BytesPerMiB;
		Queue.Min = 1;
		Queue.Max = 512;

		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeHost());
		Schema.Add(MakePort());
		Schema.Add(MoveTemp(Mode));
		Schema.Add(MakeRole(TEXT("pub"), TEXT("pair"), TEXT("push")));
		Schema.Add(MoveTemp(Queue));
		return Schema;
	}

	static FO3DTransportOptionSchema MakeReceiver()
	{
		FO3DTransportOptionField Mode;
		Mode.Key = O3DNNG::ModeOptionKey;
		Mode.DisplayName = LOCTEXT("NNGModeLabel", "Mode");
		Mode.Tooltip = LOCTEXT("NNGReceiverModeTooltip", "NNG protocol. Match it on the sender: Subscriber with Publisher, Pair with Pair, Pull with Push.");
		Mode.Type = EO3DTransportOptionType::Enum;
		Mode.Default = TEXT("sub");
		Mode.EnumValues.Add(MakeChoice(TEXT("sub"), LOCTEXT("NNGModeSub", "Subscriber")));
		Mode.EnumValues.Add(MakeChoice(TEXT("pair"), LOCTEXT("NNGModePair", "Pair")));
		Mode.EnumValues.Add(MakeChoice(TEXT("pull"), LOCTEXT("NNGModePull", "Pull")));

		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeHost());
		Schema.Add(MakePort());
		Schema.Add(MoveTemp(Mode));
		Schema.Add(MakeRole(TEXT("sub"), TEXT("pair"), TEXT("pull")));
		return Schema;
	}
}

class FOpen3DTransportNNGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// One descriptor for the transport name (ADR 0007 item 4, WP-A1).
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = TEXT("NNG");
		Descriptor.OwningModule = TEXT("Open3DTransportNNG");
		Descriptor.CreateSender = []() { return MakeShared<FO3DNngSender>(); };
		Descriptor.CreateReceiver = []() { return MakeShared<FO3DNngReceiver>(); };
		Descriptor.GetCapabilities = [](const FO3DTransportConfig& Config) { return O3DNNG::GetCapabilities(Config); };

		Descriptor.ConfigureSender = [](const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("NNG");

			const FString ModeString = NNGTransportCommon::GetOption(Config, O3DNNG::ModeOptionKey);
			const FString RoleString = NNGTransportCommon::GetOption(Config, O3DNNG::RoleOptionKey);
			const O3DNNG::ENngMode Mode = O3DNNG::ModeFromString(ModeString, O3DNNG::ENngMode::Pub);
			const O3DNNG::ENngRole Role = O3DNNG::ResolveRole(Mode, O3DNNG::RoleFromString(RoleString), /*bSender=*/true);
			const bool bListen = O3DNNG::IsListenRole(Role);

			FString Host = NNGTransportCommon::GetOption(Config, O3DNNG::HostOptionKey);
			if (Host.IsEmpty())
			{
				Host = NNGTransportCommon::ResolveDefaultHost(bListen);
			}

			const int32 Port = NNGTransportCommon::ReadPort(Config, Mode);
			const uint64 QueueBytes = NNGTransportCommon::ReadQueueBytes(Config);

			Config.AdvancedParams.Add(O3DNNG::HostOptionKey, Host);
			Config.AdvancedParams.Add(O3DNNG::PortOptionKey, FString::FromInt(Port));
			Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(Mode));
			Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(Role));
			Config.AdvancedParams.Add(O3DNNG::QueueOptionKey, NNGTransportCommon::UInt64ToString(QueueBytes));

			O3DNNG::FNngSenderOptions ParsedOptions;
			FString ErrorMessage;
			if (O3DNNG::ParseSenderOptions(Config, ParsedOptions, ErrorMessage))
			{
				Config.Uri = ParsedOptions.CanonicalUri;
				Config.StreamId = ParsedOptions.StreamId;
				// The NNG socket role (listen or dial side) is the "nng.role" option; Role is the side (TRB-27).
				Config.Role = EO3DTransportRole::Sender;
				Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(ParsedOptions.Mode));
				Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(ParsedOptions.Role));
				Config.AdvancedParams.Add(O3DNNG::QueueOptionKey, NNGTransportCommon::UInt64ToString(ParsedOptions.MaxQueueBytes));
			}
			else
			{
				UE_LOG(LogOpen3DTransportNNGModule, Warning, TEXT("NNG sender configuration parse failed: %s"), *ErrorMessage);
				Config.Uri = O3DNNG::BuildCanonicalUri(Mode, Host, Port, Role);
				Config.StreamId = O3DNNG::MakeStreamId(Host, Port);
				// The NNG socket role (listen or dial side) is the "nng.role" option; Role is the side (TRB-27).
				Config.Role = EO3DTransportRole::Sender;
			}
		};
		Descriptor.SenderOptions.OptionSchema = NNGSchema::MakeSender();

		Descriptor.ConfigureReceiver = [](const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("NNG");

			const FString ModeString = NNGTransportCommon::GetOption(Config, O3DNNG::ModeOptionKey);
			const FString RoleString = NNGTransportCommon::GetOption(Config, O3DNNG::RoleOptionKey);
			const FString HostValue = NNGTransportCommon::GetOption(Config, O3DNNG::HostOptionKey);

			const O3DNNG::ENngMode Mode = O3DNNG::ModeFromString(ModeString, O3DNNG::ENngMode::Sub);
			// TRB-40: Pair defaults to dial here and to listen on the sender, so default ends connect.
			const O3DNNG::ENngRole Role = O3DNNG::ResolveRole(Mode, O3DNNG::RoleFromString(RoleString), /*bSender=*/false);
			const bool bListen = O3DNNG::IsListenRole(Role);

			FString Host = HostValue;
			if (Host.IsEmpty())
			{
				Host = NNGTransportCommon::ResolveDefaultHost(bListen);
			}

			const int32 Port = NNGTransportCommon::ReadPort(Config, Mode);

			Config.AdvancedParams.Add(O3DNNG::HostOptionKey, Host);
			Config.AdvancedParams.Add(O3DNNG::PortOptionKey, FString::FromInt(Port));
			Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(Mode));
			Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(Role));

			// Config.Audio.bEnableAudio was set by the receiver source from its settings.

			O3DNNG::FNngReceiverOptions ParsedOptions;
			FString ErrorMessage;
			if (O3DNNG::ParseReceiverOptions(Config, ParsedOptions, ErrorMessage))
			{
				Config.Uri = ParsedOptions.CanonicalUri;
				Config.StreamId = ParsedOptions.StreamId;
				// The NNG socket role (listen or dial side) is the "nng.role" option; Role is the side (TRB-27).
				Config.Role = EO3DTransportRole::Receiver;
				Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(ParsedOptions.Mode));
				Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(ParsedOptions.Role));
			}
			else
			{
				UE_LOG(LogOpen3DTransportNNGModule, Warning, TEXT("NNG receiver configuration parse failed: %s"), *ErrorMessage);
				Config.Uri = O3DNNG::BuildCanonicalUri(Mode, Host, Port, Role);
				Config.StreamId = O3DNNG::MakeStreamId(Host, Port);
				// The NNG socket role (listen or dial side) is the "nng.role" option; Role is the side (TRB-27).
				Config.Role = EO3DTransportRole::Receiver;
			}
		};
		Descriptor.ReceiverOptions.OptionSchema = NNGSchema::MakeReceiver();

		Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));

		UE_LOG(LogOpen3DTransportNNGModule, Log, TEXT("Open3D NNG transport module started."));
	}

	virtual void ShutdownModule() override
	{
		// Unregistering drains the transport (ADR 0007 item 5, WP-A1 PR 2): sender components and
		// LiveLink sources stop and release their instances, and the registry stops and reports any
		// left, before this module's code goes away. There is no FFI handle to free afterwards.
		Registration.Reset();

		UE_LOG(LogOpen3DTransportNNGModule, Log, TEXT("Open3D NNG transport module shut down."));
	}

private:
	FO3DTransportRegistration Registration;
};

#else // O3D_WITH_TRANSPORT_NNG

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportNNGModule, Log, All);

/**
 * Stub module, compiled when O3D_WITH_TRANSPORT_NNG is 0: the transport was switched off with that
 * environment variable, or the target platform has no prebuilt nng.lib
 * (O3DBuildFlags in Open3DBroadcastBuildFlags.Build.cs). It registers nothing.
 */
class FOpen3DTransportNNGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogOpen3DTransportNNGModule, Display, TEXT("Open3D NNG transport is not available in this build (O3D_WITH_TRANSPORT_NNG=0)."));
	}

	virtual void ShutdownModule() override {}
};

#endif // O3D_WITH_TRANSPORT_NNG

IMPLEMENT_MODULE(FOpen3DTransportNNGModule, Open3DTransportNNG)

#undef LOCTEXT_NAMESPACE
