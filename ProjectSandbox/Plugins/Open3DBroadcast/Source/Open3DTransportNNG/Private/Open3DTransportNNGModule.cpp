#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#include "O3DTransportRegistry.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTransportConfigPanelBase.h"
#include "O3DTransportTypes.h"
#include "Shared/NngHelpers.h"
#include "Sender/NngSender.h"
#include "Receiver/NngReceiver.h"

#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"

#if WITH_EDITOR
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportNNGModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportNNG"

namespace NNGTransportCommon
{
	static constexpr uint64 DefaultQueueBytes = 4ull * 1024ull * 1024ull;

	static int32 ResolveDefaultPort(O3DNNG::ENngMode Mode)
	{
		switch (Mode)
		{
		case O3DNNG::ENngMode::Pair:
			return 7000;
		case O3DNNG::ENngMode::Push:
		case O3DNNG::ENngMode::Pull:
			return 8000;
		case O3DNNG::ENngMode::Sub:
		case O3DNNG::ENngMode::Pub:
		default:
			return 6000;
		}
	}

	static FString ResolveDefaultHost(bool bListen)
	{
		return bListen ? FString(TEXT("0.0.0.0")) : FString(TEXT("127.0.0.1"));
	}

	static FString UInt64ToString(uint64 Value)
	{
		return LexToString(Value);
	}
}

namespace NNGSenderConfig
{
	static FString GetOption(const UO3DSenderComponent* Component, const TCHAR* Key)
	{
		return Component ? Component->GetTransportOption(Key) : FString();
	}

	static uint64 ParseQueueBytes(const FString& InValue)
	{
		if (InValue.IsEmpty())
		{
			return NNGTransportCommon::DefaultQueueBytes;
		}

		TCHAR* EndPtr = nullptr;
		const uint64 Parsed = FCString::Strtoui64(*InValue, &EndPtr, 10);
		if (EndPtr && *EndPtr == TEXT('\0') && Parsed > 0)
		{
			return Parsed;
		}
		return NNGTransportCommon::DefaultQueueBytes;
	}
}

namespace NNGReceiverConfig
{
	static FString GetOption(const FO3DReceiverSourceConfig& Settings, const TCHAR* Key)
	{
		if (const FString* Existing = Settings.TransportOptions.Find(Key))
		{
			return *Existing;
		}
		return FString();
	}

	static void SetOption(UO3DReceiverSettingsObject* SettingsObject, const TCHAR* Key, const FString& Value)
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
}

#if WITH_EDITOR
namespace NNGSender
{
	struct FModeOption
	{
		FString Label;
		FString Mode;
		FString Role;
	};

	static TSharedPtr<FModeOption> FindMatchingMode(const TArray<TSharedPtr<FModeOption>>& Options, const FString& ModeString, const FString& RoleString)
	{
		for (const TSharedPtr<FModeOption>& Entry : Options)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			if (Entry->Mode.Equals(ModeString, ESearchCase::IgnoreCase))
			{
				if (Entry->Role.IsEmpty() || RoleString.IsEmpty() || Entry->Role.Equals(RoleString, ESearchCase::IgnoreCase))
				{
					return Entry;
				}
			}
		}

		return Options.Num() > 0 ? Options[0] : nullptr;
	}

	class SNngSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNngSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			BuildModeOptions();

			const FString ModeValue = SenderComponent ? SenderComponent->GetTransportOption(O3DNNG::ModeOptionKey) : FString(TEXT("pub"));
			const FString RoleValue = SenderComponent ? SenderComponent->GetTransportOption(O3DNNG::RoleOptionKey) : FString(TEXT("server"));
			SelectedMode = FindMatchingMode(ModeOptions, ModeValue.IsEmpty() ? TEXT("pub") : ModeValue, RoleValue);
			if (!SelectedMode.IsValid() && ModeOptions.Num() > 0)
			{
				SelectedMode = ModeOptions[0];
			}

			const FString InitialHost = ResolveHostValue();
			if (InitialHost.IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(CurrentModeIsListen()));
			}

			const int32 InitialPort = ResolvePortValue();
			if (InitialPort <= 0)
			{
				SetPortValue(NNGTransportCommon::ResolveDefaultPort(CurrentMode()));
			}

			const int32 InitialQueueMb = ResolveQueueValueMb();
			if (InitialQueueMb <= 0)
			{
				SetQueueValueMb(4);
			}

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGSenderHostLabel", "Host"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(HostTextBox, SEditableTextBox)
					.Text(FText::FromString(ResolveHostValue()))
					.OnTextCommitted(this, &SNngSenderSettingsPanel::HandleHostCommitted)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGSenderPortLabel", "Port"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(PortSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(65535)
					.Value(ResolvePortValue())
					.OnValueChanged(this, &SNngSenderSettingsPanel::HandlePortChanged)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGSenderModeLabel", "Mode"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(ModeComboBox, SComboBox<TSharedPtr<FModeOption>>)
					.OptionsSource(&ModeOptions)
					.InitiallySelectedItem(SelectedMode)
					.OnSelectionChanged(this, &SNngSenderSettingsPanel::HandleModeChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FModeOption> Option)
					{
						return SNew(STextBlock).Text(Option.IsValid() ? FText::FromString(Option->Label) : FText::GetEmpty());
					})
					[
						SNew(STextBlock)
						.Text(this, &SNngSenderSettingsPanel::GetCurrentModeLabel)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGSenderQueueLabel", "Queue Capacity (MiB)"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(QueueSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(512)
					.Value(ResolveQueueValueMb())
					.OnValueChanged(this, &SNngSenderSettingsPanel::HandleQueueChanged)
				]
			];
		}

	private:
		void BuildModeOptions()
		{
			ModeOptions.Reset();
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Publisher (listen)"), TEXT("pub"), TEXT("server") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pair (server listen)"), TEXT("pair"), TEXT("server") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pair (client dial)"), TEXT("pair"), TEXT("client") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Push (dial)"), TEXT("push"), TEXT("client") }));
		}

		FString ResolveHostValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(O3DNNG::HostOptionKey) : FString();
		}

		void SetHostValue(const FString& NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}

			const FString Sanitized = NewValue.TrimStartAndEnd();
			SenderComponent->SetTransportOption(O3DNNG::HostOptionKey, Sanitized);
		}

		int32 ResolvePortValue() const
		{
			if (!SenderComponent)
			{
				return 0;
			}

			const FString PortString = SenderComponent->GetTransportOption(O3DNNG::PortOptionKey);
			if (PortString.IsEmpty())
			{
				return 0;
			}

			return FCString::Atoi(*PortString);
		}

		void SetPortValue(int32 NewPort)
		{
			if (!SenderComponent)
			{
				return;
			}

			const int32 ClampedPort = FMath::Clamp(NewPort, 1, 65535);
			SenderComponent->SetTransportOption(O3DNNG::PortOptionKey, FString::FromInt(ClampedPort));
		}

		int32 ResolveQueueValueMb() const
		{
			if (!SenderComponent)
			{
				return 4;
			}

			const FString Stored = SenderComponent->GetTransportOption(O3DNNG::QueueOptionKey);
			const uint64 Bytes = Stored.IsEmpty() ? NNGTransportCommon::DefaultQueueBytes : NNGSenderConfig::ParseQueueBytes(Stored);
			const uint64 Mb = Bytes / (1024ull * 1024ull);
			return Mb > 0 ? static_cast<int32>(Mb) : 4;
		}

		void SetQueueValueMb(int32 NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}

			const int32 Clamped = FMath::Clamp(NewValue, 1, 512);
			const uint64 Bytes = static_cast<uint64>(Clamped) * 1024ull * 1024ull;
			SenderComponent->SetTransportOption(O3DNNG::QueueOptionKey, NNGTransportCommon::UInt64ToString(Bytes));
		}

		O3DNNG::ENngMode CurrentMode() const
		{
			if (SelectedMode.IsValid())
			{
				return O3DNNG::ModeFromString(SelectedMode->Mode, O3DNNG::ENngMode::Pub);
			}
			return O3DNNG::ENngMode::Pub;
		}

		bool CurrentModeIsListen() const
		{
			if (!SelectedMode.IsValid())
			{
				return true;
			}

			const O3DNNG::ENngMode Mode = CurrentMode();
			if (Mode == O3DNNG::ENngMode::Push)
			{
				return false;
			}
			if (Mode == O3DNNG::ENngMode::Pair)
			{
				return !SelectedMode->Role.Equals(TEXT("client"), ESearchCase::IgnoreCase);
			}
			return true;
		}

		void HandleHostCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			const FString Sanitized = NewText.ToString().TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(CurrentModeIsListen()));
				if (HostTextBox.IsValid())
				{
					HostTextBox->SetText(FText::FromString(ResolveHostValue()));
				}
			}
			else
			{
				SetHostValue(Sanitized);
			}

			NotifyConfigChanged();
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetPortValue(NewValue);
			NotifyConfigChanged();
		}

		void HandleQueueChanged(int32 NewValue)
		{
			SetQueueValueMb(NewValue);
			NotifyConfigChanged();
		}

		void HandleModeChanged(TSharedPtr<FModeOption> NewSelection, ESelectInfo::Type SelectionType)
		{
			if (!SenderComponent)
			{
				return;
			}

			if (!NewSelection.IsValid())
			{
				return;
			}

			SelectedMode = NewSelection;

			SenderComponent->SetTransportOption(O3DNNG::ModeOptionKey, SelectedMode->Mode);
			if (!SelectedMode->Role.IsEmpty())
			{
				SenderComponent->SetTransportOption(O3DNNG::RoleOptionKey, SelectedMode->Role);
			}

			if (ResolveHostValue().IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(CurrentModeIsListen()));
				if (HostTextBox.IsValid())
				{
					HostTextBox->SetText(FText::FromString(ResolveHostValue()));
				}
			}

			if (ResolvePortValue() <= 0)
			{
				SetPortValue(NNGTransportCommon::ResolveDefaultPort(CurrentMode()));
				if (PortSpinBox.IsValid())
				{
					PortSpinBox->SetValue(ResolvePortValue());
				}
			}

			NotifyConfigChanged();
		}

		FText GetCurrentModeLabel() const
		{
			return SelectedMode.IsValid() ? FText::FromString(SelectedMode->Label) : FText::GetEmpty();
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
		TArray<TSharedPtr<FModeOption>> ModeOptions;
		TSharedPtr<FModeOption> SelectedMode;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SSpinBox<int32>> QueueSpinBox;
		TSharedPtr<SComboBox<TSharedPtr<FModeOption>>> ModeComboBox;
	};
}

namespace NNGReceiver
{
	struct FModeOption
	{
		FString Label;
		FString Mode;
		FString Role;
	};

	static TSharedPtr<FModeOption> FindMatchingMode(const TArray<TSharedPtr<FModeOption>>& Options, const FString& ModeString, const FString& RoleString)
	{
		for (const TSharedPtr<FModeOption>& Entry : Options)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			if (Entry->Mode.Equals(ModeString, ESearchCase::IgnoreCase))
			{
				if (Entry->Role.IsEmpty() || RoleString.IsEmpty() || Entry->Role.Equals(RoleString, ESearchCase::IgnoreCase))
				{
					return Entry;
				}
			}
		}
		return Options.Num() > 0 ? Options[0] : nullptr;
	}

	class SNngReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(SNngReceiverSettingsPanel)
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
			BuildModeOptions();

			const FString ModeValue = GetOption(O3DNNG::ModeOptionKey, TEXT("sub"));
			const FString RoleValue = GetOption(O3DNNG::RoleOptionKey, TEXT("client"));
			SelectedMode = FindMatchingMode(ModeOptions, ModeValue, RoleValue);
			if (!SelectedMode.IsValid() && ModeOptions.Num() > 0)
			{
				SelectedMode = ModeOptions[0];
				SetModeOption(SelectedMode);
			}

			if (GetHostValue().IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(ModeIsListen()));
			}

			if (GetPortValue() <= 0)
			{
				SetPortValue(NNGTransportCommon::ResolveDefaultPort(CurrentMode()));
			}

			if (!SelectedMode.IsValid())
			{
				SelectedMode = ModeOptions.Num() > 0 ? ModeOptions[0] : nullptr;
			}

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGReceiverHostLabel", "Host"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(HostTextBox, SEditableTextBox)
					.Text(FText::FromString(GetHostValue()))
					.OnTextCommitted(this, &SNngReceiverSettingsPanel::HandleHostCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGReceiverPortLabel", "Port"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(PortSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(65535)
					.Value(GetPortValue())
					.OnValueChanged(this, &SNngReceiverSettingsPanel::HandlePortChanged)
					.OnValueCommitted(this, &SNngReceiverSettingsPanel::HandlePortCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGReceiverModeLabel", "Mode"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(ModeComboBox, SComboBox<TSharedPtr<FModeOption>>)
					.OptionsSource(&ModeOptions)
					.InitiallySelectedItem(SelectedMode)
					.OnSelectionChanged(this, &SNngReceiverSettingsPanel::HandleModeChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FModeOption> Option)
					{
						return SNew(STextBlock).Text(Option.IsValid() ? FText::FromString(Option->Label) : FText::GetEmpty());
					})
					[
						SNew(STextBlock)
						.Text(this, &SNngReceiverSettingsPanel::GetCurrentModeLabel)
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NNGReceiverTopicLabel", "Subscription Topic"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TopicTextBox, SEditableTextBox)
					.Text(FText::FromString(GetTopicValue()))
					.OnTextCommitted(this, &SNngReceiverSettingsPanel::HandleTopicCommitted)
					.IsEnabled(this, &SNngReceiverSettingsPanel::IsTopicEnabled)
				];

			BuildPanel(PanelContent, InArgs._PanelWidthOverride);
		}

	private:
		void BuildModeOptions()
		{
			ModeOptions.Reset();
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Subscriber (dial)"), TEXT("sub"), TEXT("client") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pair (client dial)"), TEXT("pair"), TEXT("client") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pair (server listen)"), TEXT("pair"), TEXT("server") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pull (server listen)"), TEXT("pull"), TEXT("server") }));
			ModeOptions.Add(MakeShared<FModeOption>(FModeOption{ TEXT("Pull (client dial)"), TEXT("pull"), TEXT("client") }));
		}

		FString GetOption(const TCHAR* Key, const TCHAR* DefaultValue = TEXT("")) const
		{
			if (!SettingsObject)
			{
				return FString(DefaultValue);
			}

			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(Key))
			{
				return *Existing;
			}
			return FString(DefaultValue);
		}

		void SetOption(const TCHAR* Key, const FString& Value)
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

		FString GetHostValue() const
		{
			return GetOption(O3DNNG::HostOptionKey);
		}

		void SetHostValue(const FString& Value)
		{
			SetOption(O3DNNG::HostOptionKey, Value.TrimStartAndEnd());
		}

		int32 GetPortValue() const
		{
			const FString PortString = GetOption(O3DNNG::PortOptionKey);
			return PortString.IsEmpty() ? 0 : FCString::Atoi(*PortString);
		}

		void SetPortValue(int32 NewPort)
		{
			const int32 Clamped = FMath::Clamp(NewPort, 1, 65535);
			SetOption(O3DNNG::PortOptionKey, FString::FromInt(Clamped));
		}

		FString GetTopicValue() const
		{
			return GetOption(O3DNNG::TopicOptionKey);
		}

		void SetTopicValue(const FString& Value)
		{
			if (Value.TrimStartAndEnd().IsEmpty())
			{
				SetOption(O3DNNG::TopicOptionKey, FString());
			}
			else
			{
				SetOption(O3DNNG::TopicOptionKey, Value.TrimStartAndEnd());
			}
		}

		O3DNNG::ENngMode CurrentMode() const
		{
			if (SelectedMode.IsValid())
			{
				return O3DNNG::ModeFromString(SelectedMode->Mode, O3DNNG::ENngMode::Sub);
			}
			return O3DNNG::ENngMode::Sub;
		}

		bool ModeIsListen() const
		{
			if (!SelectedMode.IsValid())
			{
				return false;
			}

			const O3DNNG::ENngMode Mode = CurrentMode();
			if (Mode == O3DNNG::ENngMode::Pair || Mode == O3DNNG::ENngMode::Pull)
			{
				return !SelectedMode->Role.Equals(TEXT("client"), ESearchCase::IgnoreCase);
			}
			return false;
		}

		void HandleHostCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			const FString Sanitized = NewText.ToString().TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(ModeIsListen()));
				if (HostTextBox.IsValid())
				{
					HostTextBox->SetText(FText::FromString(GetHostValue()));
				}
			}
			else
			{
				SetHostValue(Sanitized);
			}

			SubmitFromTextCommit(CommitType);
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetPortValue(NewValue);
		}

		void HandlePortCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandlePortChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		void HandleTopicCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetTopicValue(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		void HandleModeChanged(TSharedPtr<FModeOption> NewSelection, ESelectInfo::Type SelectionType)
		{
			if (!NewSelection.IsValid())
			{
				return;
			}

			SelectedMode = NewSelection;
			SetModeOption(SelectedMode);

			if (GetHostValue().IsEmpty())
			{
				SetHostValue(NNGTransportCommon::ResolveDefaultHost(ModeIsListen()));
				if (HostTextBox.IsValid())
				{
					HostTextBox->SetText(FText::FromString(GetHostValue()));
				}
			}

			if (GetPortValue() <= 0)
			{
				SetPortValue(NNGTransportCommon::ResolveDefaultPort(CurrentMode()));
				if (PortSpinBox.IsValid())
				{
					PortSpinBox->SetValue(GetPortValue());
				}
			}

			if (TopicTextBox.IsValid())
			{
				TopicTextBox->SetEnabled(IsTopicEnabled());
				if (!IsTopicEnabled())
				{
					SetTopicValue(FString());
					TopicTextBox->SetText(FText::GetEmpty());
				}
			}
		}

		void SetModeOption(const TSharedPtr<FModeOption>& Option)
		{
			if (!Option.IsValid())
			{
				return;
			}

			SetOption(O3DNNG::ModeOptionKey, Option->Mode);
			if (!Option->Role.IsEmpty())
			{
				SetOption(O3DNNG::RoleOptionKey, Option->Role);
			}
			else
			{
				SetOption(O3DNNG::RoleOptionKey, FString());
			}
		}

		FText GetCurrentModeLabel() const
		{
			return SelectedMode.IsValid() ? FText::FromString(SelectedMode->Label) : FText::GetEmpty();
		}

		bool IsTopicEnabled() const
		{
			return CurrentMode() == O3DNNG::ENngMode::Sub;
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TArray<TSharedPtr<FModeOption>> ModeOptions;
		TSharedPtr<FModeOption> SelectedMode;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SEditableTextBox> TopicTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SComboBox<TSharedPtr<FModeOption>>> ModeComboBox;
	};
}
#endif // WITH_EDITOR

class FOpen3DTransportNNGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		O3DTransport::RegisterSender(TEXT("NNG"), []() { return MakeShared<FO3DNngSender>(); });
		O3DTransport::RegisterReceiver(TEXT("NNG"), []() { return MakeShared<FO3DNngReceiver>(); });

		FO3DSenderTransportCustomization SenderCustomization;
		SenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("NNG");

			const FString ModeString = NNGSenderConfig::GetOption(SenderComponent, O3DNNG::ModeOptionKey);
			const FString RoleString = NNGSenderConfig::GetOption(SenderComponent, O3DNNG::RoleOptionKey);
			const O3DNNG::ENngMode Mode = O3DNNG::ModeFromString(ModeString, O3DNNG::ENngMode::Pub);
			const O3DNNG::ENngRole Role = O3DNNG::RoleFromString(RoleString, (Mode == O3DNNG::ENngMode::Push) ? O3DNNG::ENngRole::Client : O3DNNG::ENngRole::Server);
			const bool bListen = (Mode == O3DNNG::ENngMode::Push) ? false : (Mode == O3DNNG::ENngMode::Pair ? Role != O3DNNG::ENngRole::Client : true);

			FString Host = NNGSenderConfig::GetOption(SenderComponent, O3DNNG::HostOptionKey);
			if (Host.IsEmpty())
			{
				Host = NNGTransportCommon::ResolveDefaultHost(bListen);
			}

			int32 Port = 0;
			const FString PortString = NNGSenderConfig::GetOption(SenderComponent, O3DNNG::PortOptionKey);
			if (!PortString.IsEmpty())
			{
				Port = FCString::Atoi(*PortString);
			}
			if (Port <= 0)
			{
				Port = NNGTransportCommon::ResolveDefaultPort(Mode);
			}

			const uint64 QueueBytes = NNGSenderConfig::ParseQueueBytes(NNGSenderConfig::GetOption(SenderComponent, O3DNNG::QueueOptionKey));

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
				Config.Role = O3DNNG::RoleToString(ParsedOptions.Role);
				Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(ParsedOptions.Mode));
				Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(ParsedOptions.Role));
				Config.AdvancedParams.Add(O3DNNG::QueueOptionKey, NNGTransportCommon::UInt64ToString(ParsedOptions.MaxQueueBytes));
			}
			else
			{
				UE_LOG(LogOpen3DTransportNNGModule, Warning, TEXT("NNG sender configuration parse failed: %s"), *ErrorMessage);
				Config.Uri = O3DNNG::BuildCanonicalUri(Mode, Host, Port, Role, FString());
				Config.StreamId = O3DNNG::MakeStreamId(Host, Port, FString());
				Config.Role = O3DNNG::RoleToString(Role);
			}
		};
#if WITH_EDITOR
		SenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			if (!SenderComponent)
			{
				return nullptr;
			}

			return SNew(NNGSender::SNngSenderSettingsPanel)
				.SenderComponent(SenderComponent)
				.OnConfigChanged(OnConfigChanged);
		};
#endif // WITH_EDITOR
		O3DSender::RegisterTransportCustomization(TEXT("NNG"), MoveTemp(SenderCustomization));

		FO3DReceiverTransportCustomization ReceiverCustomization;
		ReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("NNG");

			const FString ModeString = NNGReceiverConfig::GetOption(Settings, O3DNNG::ModeOptionKey);
			const FString RoleString = NNGReceiverConfig::GetOption(Settings, O3DNNG::RoleOptionKey);
			const FString HostValue = NNGReceiverConfig::GetOption(Settings, O3DNNG::HostOptionKey);
			const FString PortString = NNGReceiverConfig::GetOption(Settings, O3DNNG::PortOptionKey);
			const FString TopicString = NNGReceiverConfig::GetOption(Settings, O3DNNG::TopicOptionKey);

			const O3DNNG::ENngMode Mode = O3DNNG::ModeFromString(ModeString, O3DNNG::ENngMode::Sub);
			const O3DNNG::ENngRole DefaultRole = (Mode == O3DNNG::ENngMode::Pull || Mode == O3DNNG::ENngMode::Pair) ? O3DNNG::ENngRole::Server : O3DNNG::ENngRole::Client;
			const O3DNNG::ENngRole Role = O3DNNG::RoleFromString(RoleString, DefaultRole);
			const bool bListen = (Mode == O3DNNG::ENngMode::Pair || Mode == O3DNNG::ENngMode::Pull) ? (Role == O3DNNG::ENngRole::Server) : false;

			FString Host = HostValue;
			if (Host.IsEmpty())
			{
				Host = NNGTransportCommon::ResolveDefaultHost(bListen);
			}

			int32 Port = 0;
			if (!PortString.IsEmpty())
			{
				Port = FCString::Atoi(*PortString);
			}
			if (Port <= 0)
			{
				Port = NNGTransportCommon::ResolveDefaultPort(Mode);
			}

			Config.AdvancedParams.Add(O3DNNG::HostOptionKey, Host);
			Config.AdvancedParams.Add(O3DNNG::PortOptionKey, FString::FromInt(Port));
			Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(Mode));
			Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(Role));

			const FString TrimmedTopic = TopicString.TrimStartAndEnd();
			if (Mode == O3DNNG::ENngMode::Sub && !TrimmedTopic.IsEmpty())
			{
				Config.AdvancedParams.Add(O3DNNG::TopicOptionKey, TrimmedTopic);
			}
			else
			{
				Config.AdvancedParams.Remove(O3DNNG::TopicOptionKey);
			}

			Config.Audio.bEnableAudio = Settings.bEnableAudio;
			Config.Audio.StreamLabel = Settings.AudioStreamLabel;

			O3DNNG::FNngReceiverOptions ParsedOptions;
			FString ErrorMessage;
			if (O3DNNG::ParseReceiverOptions(Config, ParsedOptions, ErrorMessage))
			{
				Config.Uri = ParsedOptions.CanonicalUri;
				Config.StreamId = ParsedOptions.StreamId;
				Config.Role = O3DNNG::RoleToString(ParsedOptions.Role);
				Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, O3DNNG::ModeToString(ParsedOptions.Mode));
				Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, O3DNNG::RoleToString(ParsedOptions.Role));
				if (Mode == O3DNNG::ENngMode::Sub && !ParsedOptions.Topic.IsEmpty())
				{
					Config.AdvancedParams.Add(O3DNNG::TopicOptionKey, ParsedOptions.Topic);
				}
				else
				{
					Config.AdvancedParams.Remove(O3DNNG::TopicOptionKey);
				}
			}
			else
			{
				UE_LOG(LogOpen3DTransportNNGModule, Warning, TEXT("NNG receiver configuration parse failed: %s"), *ErrorMessage);
				Config.Uri = O3DNNG::BuildCanonicalUri(Mode, Host, Port, Role, TrimmedTopic);
				Config.StreamId = O3DNNG::MakeStreamId(Host, Port, TrimmedTopic);
				Config.Role = O3DNNG::RoleToString(Role);
			}
		};
#if WITH_EDITOR
		ReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			if (!SettingsObject)
			{
				return nullptr;
			}

			return SNew(NNGReceiver::SNngReceiverSettingsPanel)
			.SettingsObject(SettingsObject)
			.PanelWidthOverride(SO3DTransportConfigPanelBase::DefaultPanelWidth)
			.OnSubmit(OnSubmit);
		};
#endif // WITH_EDITOR

		O3DReceiver::RegisterTransportCustomization(TEXT("NNG"), MoveTemp(ReceiverCustomization));

		UE_LOG(LogOpen3DTransportNNGModule, Log, TEXT("Open3D NNG transport module started."));
	}

	virtual void ShutdownModule() override
	{
		O3DTransport::UnregisterSender(TEXT("NNG"));
		O3DTransport::UnregisterReceiver(TEXT("NNG"));
		O3DSender::UnregisterTransportCustomization(TEXT("NNG"));
		O3DReceiver::UnregisterTransportCustomization(TEXT("NNG"));

		UE_LOG(LogOpen3DTransportNNGModule, Log, TEXT("Open3D NNG transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportNNGModule, Open3DTransportNNG)

#undef LOCTEXT_NAMESPACE
