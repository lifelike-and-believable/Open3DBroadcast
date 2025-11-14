#include "Modules/ModuleManager.h"

#include "LoopbackSender.h"
#include "LoopbackReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTransportConfigPanelBase.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DTransportTypes.h"

#if WITH_EDITOR
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportLoopbackModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportLoopback"

namespace LoopbackReceiver
{
	static constexpr TCHAR ChannelOptionKey[] = TEXT("channel");

#if WITH_EDITOR
	class SLoopbackReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(SLoopbackReceiverSettingsPanel)
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

			FString InitialChannel = GetChannelValue();
			if (InitialChannel.IsEmpty())
			{
				InitialChannel = TEXT("default");
				SetChannelValue(InitialChannel);
			}

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);
			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LoopbackChannelLabel", "Channel Name"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(ChannelTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialChannel))
					.OnTextCommitted(this, &SLoopbackReceiverSettingsPanel::HandleChannelCommitted)
				];

			BuildPanel(PanelContent, InArgs._PanelWidthOverride);
		}

	private:
		void HandleChannelCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			const FString NewValue = NewText.ToString().TrimStartAndEnd();
			if (NewValue.IsEmpty())
			{
				SetChannelValue(TEXT("default"));
				if (ChannelTextBox.IsValid())
				{
					ChannelTextBox->SetText(FText::FromString(TEXT("default")));
				}
			}
			else
			{
				SetChannelValue(NewValue);
			}

			SubmitFromTextCommit(CommitType);
		}

		FString GetChannelValue() const
		{
			if (!SettingsObject)
			{
				return FString();
			}

			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(ChannelOptionKey))
			{
				return *Existing;
			}

			return FString();
		}

		void SetChannelValue(const FString& NewValue)
		{
			if (!SettingsObject)
			{
				return;
			}

			SettingsObject->Modify();
			SettingsObject->Settings.TransportOptions.Add(ChannelOptionKey, NewValue);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> ChannelTextBox;
	};
#endif // WITH_EDITOR
}

namespace LoopbackSender
{
	static constexpr TCHAR ChannelOptionKey[] = TEXT("channel");
	static constexpr TCHAR QueueOptionKey[] = TEXT("loopback.maxqueue");

#if WITH_EDITOR
	class SLoopbackSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SLoopbackSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			FString InitialChannel = GetChannelValue();
			if (InitialChannel.IsEmpty())
			{
				InitialChannel = TEXT("default");
				SetChannelValue(InitialChannel);
			}

			int32 InitialQueue = GetQueueValue();
			if (InitialQueue <= 0)
			{
				InitialQueue = 64;
				SetQueueValue(InitialQueue);
			}

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LoopbackSenderChannelLabel", "Channel Name"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(ChannelTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialChannel))
					.OnTextCommitted(this, &SLoopbackSenderSettingsPanel::HandleChannelCommitted)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LoopbackSenderQueueLabel", "Queue Capacity"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(QueueSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(4096)
					.Value(InitialQueue)
					.OnValueChanged(this, &SLoopbackSenderSettingsPanel::HandleQueueChanged)
				]
			];
		}

	private:
		FString GetChannelValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(ChannelOptionKey) : FString();
		}

		void SetChannelValue(const FString& Value)
		{
			if (!SenderComponent)
			{
				return;
			}

			const FString Sanitized = Value.TrimStartAndEnd();
			SenderComponent->SetTransportOption(ChannelOptionKey, Sanitized.IsEmpty() ? TEXT("default") : Sanitized);
		}

		int32 GetQueueValue() const
		{
			if (!SenderComponent)
			{
				return 64;
			}

			const FString Stored = SenderComponent->GetTransportOption(QueueOptionKey);
			if (Stored.IsEmpty())
			{
				return 64;
			}

			const int32 Parsed = FCString::Atoi(*Stored);
			return Parsed > 0 ? Parsed : 64;
		}

		void SetQueueValue(int32 NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}

			const int32 ClampedValue = FMath::Max(1, NewValue);
			SenderComponent->SetTransportOption(QueueOptionKey, FString::FromInt(ClampedValue));
		}

		void HandleChannelCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetChannelValue(NewText.ToString());
			if (ChannelTextBox.IsValid())
			{
				ChannelTextBox->SetText(FText::FromString(SenderComponent ? SenderComponent->GetTransportOption(ChannelOptionKey) : TEXT("default")));
			}
			if (OnConfigChanged.IsBound())
			{
				OnConfigChanged.Execute();
			}
		}

		void HandleQueueChanged(int32 NewValue)
		{
			SetQueueValue(NewValue);
			if (OnConfigChanged.IsBound())
			{
				OnConfigChanged.Execute();
			}
		}

		UO3DSenderComponent* SenderComponent = nullptr;
		FSimpleDelegate OnConfigChanged;
		TSharedPtr<SEditableTextBox> ChannelTextBox;
		TSharedPtr<SSpinBox<int32>> QueueSpinBox;
	};
#endif // WITH_EDITOR
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
				ChannelName = TEXT("default");
			}

			Config.Transport = TEXT("Loopback");
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=sub"), *ChannelName);
			Config.AdvancedParams.Add(LoopbackReceiver::ChannelOptionKey, ChannelName);
		};
#if WITH_EDITOR
	LoopbackCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			if (!SettingsObject)
			{
				return nullptr;
			}

			return SNew(LoopbackReceiver::SLoopbackReceiverSettingsPanel)
		.SettingsObject(SettingsObject)
		.PanelWidthOverride(SO3DTransportConfigPanelBase::DefaultPanelWidth)
		.OnSubmit(OnSubmit);
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(TEXT("Loopback"), MoveTemp(LoopbackCustomization));

		FO3DSenderTransportCustomization LoopbackSenderCustomization;
		LoopbackSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			FString ChannelName = SenderComponent ? SenderComponent->GetTransportOption(LoopbackSender::ChannelOptionKey) : FString();
			if (ChannelName.IsEmpty())
			{
				ChannelName = TEXT("default");
			}

			Config.Transport = TEXT("Loopback");
			Config.Role = TEXT("sender");
			Config.StreamId = ChannelName;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=pub"), *ChannelName);
			Config.AdvancedParams.Add(LoopbackSender::ChannelOptionKey, ChannelName);

			FString QueueString = SenderComponent ? SenderComponent->GetTransportOption(LoopbackSender::QueueOptionKey) : FString();
			int32 QueueValue = QueueString.IsEmpty() ? 64 : FMath::Max(1, FCString::Atoi(*QueueString));
			Config.AdvancedParams.Add(LoopbackSender::QueueOptionKey, FString::FromInt(QueueValue));
		};
#if WITH_EDITOR
		LoopbackSenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			if (!SenderComponent)
			{
				return nullptr;
			}

			return SNew(LoopbackSender::SLoopbackSenderSettingsPanel)
				.SenderComponent(SenderComponent)
				.OnConfigChanged(OnConfigChanged);
		};
#endif // WITH_EDITOR
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
