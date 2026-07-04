#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQFfiSupport.h"
#include "Shared/MoQHelpers.h"
#include "Sender/MoQSender.h"
#include "Receiver/MoQReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"

#if WITH_EDITOR
#include "O3DTransportConfigPanelBase.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#endif // WITH_EDITOR

#define LOCTEXT_NAMESPACE "Open3DTransportMoQ"

namespace MoQConfig
{
	// Mirrors MoQHelpers::GetAdvancedOption's key set (relay_url, track_namespace,
	// track_name, delivery_mode, queue_bytes - see MoQHelpers.h) but reads/writes
	// through the editor-facing option stores (UO3DSenderComponent::TransportOptions /
	// FO3DReceiverSourceConfig::TransportOptions) rather than a resolved
	// FO3DTransportConfig, since these are used by the settings widgets below as
	// well as by ConfigureTransport.

	static FString GetSenderOption(const UO3DSenderComponent* Component, const TCHAR* Key)
	{
		return Component ? Component->GetTransportOption(Key) : FString();
	}

	static void SetSenderOption(UO3DSenderComponent* Component, const TCHAR* Key, const FString& Value)
	{
		if (Component)
		{
			Component->SetTransportOption(Key, Value);
		}
	}

	static FString GetReceiverOption(const FO3DReceiverSourceConfig& Settings, const TCHAR* Key)
	{
		if (const FString* Existing = Settings.TransportOptions.Find(Key))
		{
			return *Existing;
		}
		return FString();
	}

	static void SetReceiverOption(UO3DReceiverSettingsObject* SettingsObject, const TCHAR* Key, const FString& Value)
	{
		if (!SettingsObject)
		{
			return;
		}

		SettingsObject->Modify();
		if (Value.IsEmpty())
		{
			SettingsObject->Settings.TransportOptions.Remove(Key);
		}
		else
		{
			SettingsObject->Settings.TransportOptions.Add(Key, Value);
		}
	}

	static uint64 ParseQueueBytes(const FString& InValue)
	{
		if (InValue.IsEmpty())
		{
			return MoQHelpers::kDefaultQueueBytes;
		}

		TCHAR* EndPtr = nullptr;
		const uint64 Parsed = FCString::Strtoui64(*InValue, &EndPtr, 10);
		if (EndPtr && *EndPtr == TEXT('\0') && Parsed > 0)
		{
			return FMath::Clamp<uint64>(Parsed, MoQHelpers::kMinQueueBytes, MoQHelpers::kMaxQueueBytes);
		}
		return MoQHelpers::kDefaultQueueBytes;
	}

	static bool DeliveryModeIsDatagram(const FString& Value)
	{
		return Value.Equals(TEXT("datagram"), ESearchCase::IgnoreCase);
	}
}

#if WITH_EDITOR
namespace MoQEditor
{
	static TArray<TSharedPtr<FString>> BuildDeliveryModeOptions()
	{
		TArray<TSharedPtr<FString>> Options;
		Options.Add(MakeShared<FString>(TEXT("Stream")));
		Options.Add(MakeShared<FString>(TEXT("Datagram")));
		return Options;
	}

	static TSharedPtr<FString> FindDeliveryModeOption(const TArray<TSharedPtr<FString>>& Options, bool bDatagram)
	{
		const FString Target = bDatagram ? TEXT("Datagram") : TEXT("Stream");
		for (const TSharedPtr<FString>& Option : Options)
		{
			if (Option.IsValid() && Option->Equals(Target))
			{
				return Option;
			}
		}
		return Options.Num() > 0 ? Options[0] : nullptr;
	}

	static int32 BytesToMb(uint64 Bytes)
	{
		const uint64 Mb = Bytes / (1024ull * 1024ull);
		return Mb > 0 ? static_cast<int32>(Mb) : 1;
	}

	static uint64 MbToBytes(int32 Mb)
	{
		return static_cast<uint64>(FMath::Max(1, Mb)) * 1024ull * 1024ull;
	}

	// Sender-side settings panel: relay URL is required (no generic fallback
	// exists on UO3DSenderComponent for it - see MoQHelpers::ResolveRelayUrl);
	// namespace/track name are optional overrides of the auto-derived
	// mocap/<session>/<track> naming (README.md's "Configuration Options").
	class SMoQSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMoQSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			DeliveryModeOptions = BuildDeliveryModeOptions();
			SelectedDeliveryMode = FindDeliveryModeOption(DeliveryModeOptions, MoQConfig::DeliveryModeIsDatagram(GetOption(MoQHelpers::kKeyDeliveryMode)));

			if (ResolveQueueValueMb() <= 0)
			{
				SetQueueValueMb(MoQEditor::BytesToMb(MoQHelpers::kDefaultQueueBytes));
			}

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQSenderRelayLabel", "Relay URL"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(RelayUrlTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQRelayUrlHint", "https://relay.example.com:443"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyRelayUrl)))
					.OnTextCommitted(this, &SMoQSenderSettingsPanel::HandleRelayUrlCommitted)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQSenderNamespaceLabel", "Track Namespace (optional)"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(NamespaceTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQNamespaceHint", "auto: mocap/<session> if blank"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyTrackNamespace)))
					.OnTextCommitted(this, &SMoQSenderSettingsPanel::HandleNamespaceCommitted)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQSenderTrackLabel", "Track Name (optional)"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(TrackNameTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQTrackNameHint", "auto: from Subject Name if blank"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyTrackName)))
					.OnTextCommitted(this, &SMoQSenderSettingsPanel::HandleTrackNameCommitted)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQSenderDeliveryLabel", "Delivery Mode"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(DeliveryModeComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&DeliveryModeOptions)
					.InitiallySelectedItem(SelectedDeliveryMode)
					.OnSelectionChanged(this, &SMoQSenderSettingsPanel::HandleDeliveryModeChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
					{
						return SNew(STextBlock).Text(Option.IsValid() ? FText::FromString(*Option) : FText::GetEmpty());
					})
					[
						SNew(STextBlock)
						.Text(this, &SMoQSenderSettingsPanel::GetCurrentDeliveryModeLabel)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQSenderQueueLabel", "Queue Capacity (MiB)"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(QueueSpinBox, SSpinBox<int32>)
					.MinValue(MoQEditor::BytesToMb(MoQHelpers::kMinQueueBytes))
					.MaxValue(MoQEditor::BytesToMb(MoQHelpers::kMaxQueueBytes))
					.Value(ResolveQueueValueMb())
					.OnValueChanged(this, &SMoQSenderSettingsPanel::HandleQueueChanged)
				]
			];
		}

	private:
		FString GetOption(const TCHAR* Key) const
		{
			return MoQConfig::GetSenderOption(SenderComponent, Key);
		}

		void SetOption(const TCHAR* Key, const FString& Value)
		{
			MoQConfig::SetSenderOption(SenderComponent, Key, Value);
		}

		int32 ResolveQueueValueMb() const
		{
			const FString Existing = GetOption(MoQHelpers::kKeyQueueBytes);
			return Existing.IsEmpty() ? 0 : MoQEditor::BytesToMb(MoQConfig::ParseQueueBytes(Existing));
		}

		void SetQueueValueMb(int32 Mb)
		{
			SetOption(MoQHelpers::kKeyQueueBytes, LexToString(MoQEditor::MbToBytes(Mb)));
		}

		void HandleRelayUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyRelayUrl, NewText.ToString().TrimStartAndEnd());
			NotifyConfigChanged();
		}

		void HandleNamespaceCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyTrackNamespace, NewText.ToString().TrimStartAndEnd());
			NotifyConfigChanged();
		}

		void HandleTrackNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyTrackName, NewText.ToString().TrimStartAndEnd());
			NotifyConfigChanged();
		}

		void HandleDeliveryModeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectionType)
		{
			if (!NewSelection.IsValid())
			{
				return;
			}

			SelectedDeliveryMode = NewSelection;
			SetOption(MoQHelpers::kKeyDeliveryMode, NewSelection->Equals(TEXT("Datagram")) ? TEXT("datagram") : TEXT("stream"));
			NotifyConfigChanged();
		}

		void HandleQueueChanged(int32 NewValue)
		{
			SetQueueValueMb(NewValue);
			NotifyConfigChanged();
		}

		FText GetCurrentDeliveryModeLabel() const
		{
			return SelectedDeliveryMode.IsValid() ? FText::FromString(*SelectedDeliveryMode) : FText::GetEmpty();
		}

		void NotifyConfigChanged()
		{
			if (OnConfigChanged.IsBound())
			{
				OnConfigChanged.Execute();
			}
		}

		UO3DSenderComponent* SenderComponent = nullptr;
		FSimpleDelegate OnConfigChanged;
		TArray<TSharedPtr<FString>> DeliveryModeOptions;
		TSharedPtr<FString> SelectedDeliveryMode;
		TSharedPtr<SEditableTextBox> RelayUrlTextBox;
		TSharedPtr<SEditableTextBox> NamespaceTextBox;
		TSharedPtr<SEditableTextBox> TrackNameTextBox;
		TSharedPtr<SComboBox<TSharedPtr<FString>>> DeliveryModeComboBox;
		TSharedPtr<SSpinBox<int32>> QueueSpinBox;
	};

	// Receiver-side settings panel: same fields, backed by the receiver's
	// UO3DReceiverSettingsObject/FO3DReceiverSourceConfig option store and
	// SO3DTransportConfigPanelBase's submit-on-Enter convention instead of
	// SenderComponent's OnConfigChanged delegate.
	class SMoQReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(SMoQReceiverSettingsPanel)
			: _PanelWidthOverride(SO3DTransportConfigPanelBase::DefaultPanelWidth)
		{}
			SLATE_ARGUMENT(UO3DReceiverSettingsObject*, SettingsObject)
			SLATE_ARGUMENT(float, PanelWidthOverride)
			SLATE_EVENT(FSimpleDelegate, OnSubmit)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SettingsObject = InArgs._SettingsObject;
			SetOnSubmit(InArgs._OnSubmit);

			DeliveryModeOptions = BuildDeliveryModeOptions();
			SelectedDeliveryMode = FindDeliveryModeOption(DeliveryModeOptions, MoQConfig::DeliveryModeIsDatagram(GetOption(MoQHelpers::kKeyDeliveryMode)));

			if (ResolveQueueValueMb() <= 0)
			{
				SetQueueValueMb(MoQEditor::BytesToMb(MoQHelpers::kDefaultQueueBytes));
			}

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQReceiverRelayLabel", "Relay URL"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(RelayUrlTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQRelayUrlHint", "https://relay.example.com:443"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyRelayUrl)))
					.OnTextCommitted(this, &SMoQReceiverSettingsPanel::HandleRelayUrlCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQReceiverNamespaceLabel", "Track Namespace (optional)"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(NamespaceTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQNamespaceHint", "auto: mocap/<session> if blank"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyTrackNamespace)))
					.OnTextCommitted(this, &SMoQReceiverSettingsPanel::HandleNamespaceCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQReceiverTrackLabel", "Track Name (optional)"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(TrackNameTextBox, SEditableTextBox)
					.HintText(LOCTEXT("MoQTrackNameHint", "auto: from Stream Id if blank"))
					.Text(FText::FromString(GetOption(MoQHelpers::kKeyTrackName)))
					.OnTextCommitted(this, &SMoQReceiverSettingsPanel::HandleTrackNameCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQReceiverDeliveryLabel", "Delivery Mode"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(DeliveryModeComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&DeliveryModeOptions)
					.InitiallySelectedItem(SelectedDeliveryMode)
					.OnSelectionChanged(this, &SMoQReceiverSettingsPanel::HandleDeliveryModeChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
					{
						return SNew(STextBlock).Text(Option.IsValid() ? FText::FromString(*Option) : FText::GetEmpty());
					})
					[
						SNew(STextBlock)
						.Text(this, &SMoQReceiverSettingsPanel::GetCurrentDeliveryModeLabel)
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MoQReceiverQueueLabel", "Queue Capacity (MiB)"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(QueueSpinBox, SSpinBox<int32>)
					.MinValue(MoQEditor::BytesToMb(MoQHelpers::kMinQueueBytes))
					.MaxValue(MoQEditor::BytesToMb(MoQHelpers::kMaxQueueBytes))
					.Value(ResolveQueueValueMb())
					.OnValueChanged(this, &SMoQReceiverSettingsPanel::HandleQueueChanged)
					.OnValueCommitted(this, &SMoQReceiverSettingsPanel::HandleQueueCommitted)
				];

			BuildPanel(PanelContent, InArgs._PanelWidthOverride);
		}

	private:
		FString GetOption(const TCHAR* Key) const
		{
			return SettingsObject ? MoQConfig::GetReceiverOption(SettingsObject->Settings, Key) : FString();
		}

		void SetOption(const TCHAR* Key, const FString& Value)
		{
			MoQConfig::SetReceiverOption(SettingsObject, Key, Value);
		}

		int32 ResolveQueueValueMb() const
		{
			const FString Existing = GetOption(MoQHelpers::kKeyQueueBytes);
			return Existing.IsEmpty() ? 0 : MoQEditor::BytesToMb(MoQConfig::ParseQueueBytes(Existing));
		}

		void SetQueueValueMb(int32 Mb)
		{
			SetOption(MoQHelpers::kKeyQueueBytes, LexToString(MoQEditor::MbToBytes(Mb)));
		}

		void HandleRelayUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyRelayUrl, NewText.ToString().TrimStartAndEnd());
			SubmitFromTextCommit(CommitType);
		}

		void HandleNamespaceCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyTrackNamespace, NewText.ToString().TrimStartAndEnd());
			SubmitFromTextCommit(CommitType);
		}

		void HandleTrackNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetOption(MoQHelpers::kKeyTrackName, NewText.ToString().TrimStartAndEnd());
			SubmitFromTextCommit(CommitType);
		}

		void HandleDeliveryModeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectionType)
		{
			if (!NewSelection.IsValid())
			{
				return;
			}

			SelectedDeliveryMode = NewSelection;
			SetOption(MoQHelpers::kKeyDeliveryMode, NewSelection->Equals(TEXT("Datagram")) ? TEXT("datagram") : TEXT("stream"));
			Submit();
		}

		void HandleQueueChanged(int32 NewValue)
		{
			SetQueueValueMb(NewValue);
		}

		void HandleQueueCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandleQueueChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		FText GetCurrentDeliveryModeLabel() const
		{
			return SelectedDeliveryMode.IsValid() ? FText::FromString(*SelectedDeliveryMode) : FText::GetEmpty();
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TArray<TSharedPtr<FString>> DeliveryModeOptions;
		TSharedPtr<FString> SelectedDeliveryMode;
		TSharedPtr<SEditableTextBox> RelayUrlTextBox;
		TSharedPtr<SEditableTextBox> NamespaceTextBox;
		TSharedPtr<SEditableTextBox> TrackNameTextBox;
		TSharedPtr<SComboBox<TSharedPtr<FString>>> DeliveryModeComboBox;
		TSharedPtr<SSpinBox<int32>> QueueSpinBox;
	};
}
#endif // WITH_EDITOR

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

		RegisterTransportCustomizations();
	}

	void UnregisterTransports()
	{
		O3DTransport::UnregisterSender(TEXT("MoQ"));
		O3DTransport::UnregisterReceiver(TEXT("MoQ"));
		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport factories unregistered"));

		UnregisterTransportCustomizations();
	}

	// Registers the customization each transport's editor UI is enumerated
	// from (O3DSender::GetRegisteredTransportNames() / O3DReceiver's
	// equivalent) - without this, "MoQ" is a working transport (factories
	// above) but never appears in either the Sender component's or the
	// Receiver LiveLink source's transport dropdown.
	void RegisterTransportCustomizations()
	{
		FO3DSenderTransportCustomization SenderCustomization;
		SenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			// AdvancedParams (relay_url/track_namespace/track_name/delivery_mode/
			// queue_bytes) are already copied generically from
			// SenderComponent->TransportOptions by BuildTransportConfig() before
			// this runs; MoQHelpers reads them directly from Config.AdvancedParams.
			// Mirror the relay URL into Config.Uri/StreamId too, for parity with
			// other transports and any code that inspects those fields directly
			// instead of going through MoQHelpers::ResolveRelayUrl.
			Config.Transport = TEXT("MoQ");
			Config.Uri = MoQConfig::GetSenderOption(SenderComponent, MoQHelpers::kKeyRelayUrl);
			if (Config.StreamId.IsEmpty() && SenderComponent)
			{
				Config.StreamId = SenderComponent->SubjectName;
			}
		};
#if WITH_EDITOR
		SenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			if (!SenderComponent)
			{
				return nullptr;
			}

			return SNew(MoQEditor::SMoQSenderSettingsPanel)
				.SenderComponent(SenderComponent)
				.OnConfigChanged(OnConfigChanged);
		};
#endif // WITH_EDITOR
		O3DSender::RegisterTransportCustomization(TEXT("MoQ"), MoveTemp(SenderCustomization));

		FO3DReceiverTransportCustomization ReceiverCustomization;
		ReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("MoQ");
			Config.Uri = MoQConfig::GetReceiverOption(Settings, MoQHelpers::kKeyRelayUrl);
		};
#if WITH_EDITOR
		ReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			return SNew(MoQEditor::SMoQReceiverSettingsPanel)
				.SettingsObject(SettingsObject)
				.OnSubmit(OnSubmit);
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(TEXT("MoQ"), MoveTemp(ReceiverCustomization));

		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport customizations registered"));
	}

	void UnregisterTransportCustomizations()
	{
		O3DSender::UnregisterTransportCustomization(TEXT("MoQ"));
		O3DReceiver::UnregisterTransportCustomization(TEXT("MoQ"));
		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport customizations unregistered"));
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
