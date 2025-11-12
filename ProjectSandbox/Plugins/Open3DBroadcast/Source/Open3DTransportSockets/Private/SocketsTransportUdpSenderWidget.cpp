#include "SocketsTransportEditorWidgets.h"

#if WITH_EDITOR

#include "Math/UnrealMathUtility.h"
#include "O3DSenderComponent.h"
#include "SocketsTransportCommon.h"
#include "SocketsTransportConfig.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Open3DTransportUdpSender"

namespace
{
	using namespace O3DSocketsConfig;

	class SUdpSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SUdpSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			if (SenderComponent)
			{
				if (SenderComponent->GetTransportOption(O3DSockets::HostOptionKey).IsEmpty())
				{
					SetHost(TEXT("127.0.0.1"));
				}

			const int32 ExistingPort = GetPortInternal();
			if (ExistingPort <= 0 || ExistingPort == DefaultTcpPort)
			{
				SetPort(DefaultUdpPort);
			}

				if (SenderComponent->GetTransportOption(O3DSockets::BroadcastOptionKey).IsEmpty())
				{
					SetBroadcast(false);
				}

				if (GetMtuInternal() <= 0)
				{
					SetMtu(1200);
				}

				if (GetMaxDatagramInternal() <= 0)
				{
					SetMaxDatagram(64000);
				}

			const int32 ExistingAudioPort = GetAudioPortInternal();
			if ((ExistingAudioPort <= 0 || ExistingAudioPort == DefaultTcpPort + 1) && GetPort() > 0)
			{
				SetAudioPort(GetPort() + 1);
			}
			}

			const FString CurrentAudioHost = GetAudioHost();

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(SocketsEditor::TransportPanelWidth)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderHostLabel", "Destination Host"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(HostTextBox, SEditableTextBox)
						.Text(FText::FromString(GetHost()))
						.OnTextCommitted(this, &SUdpSenderSettingsPanel::HandleHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderPortLabel", "Data Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(PortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(GetPort())
						.OnValueChanged(this, &SUdpSenderSettingsPanel::HandlePortChanged)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SAssignNew(BroadcastCheckBox, SCheckBox)
							.IsChecked(this, &SUdpSenderSettingsPanel::GetBroadcastCheckState)
							.OnCheckStateChanged(this, &SUdpSenderSettingsPanel::HandleBroadcastChanged)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						.Padding(4.f, 0.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("UdpSenderBroadcastLabel", "Enable UDP Broadcast"))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 8.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderMtuLabel", "MTU"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(MtuSpinBox, SSpinBox<int32>)
						.MinValue(256)
						.MaxValue(65507)
						.Value(GetMtu())
						.OnValueChanged(this, &SUdpSenderSettingsPanel::HandleMtuChanged)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderMaxDatagramLabel", "Max Datagram Bytes"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(MaxDatagramSpinBox, SSpinBox<int32>)
						.MinValue(512)
						.MaxValue(65507)
						.Value(GetMaxDatagram())
						.OnValueChanged(this, &SUdpSenderSettingsPanel::HandleMaxDatagramChanged)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderAudioHostLabel", "Audio Host"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(AudioHostTextBox, SEditableTextBox)
						.Text(CurrentAudioHost.IsEmpty() ? FText::GetEmpty() : FText::FromString(CurrentAudioHost))
						.HintText(LOCTEXT("UdpSenderAudioHostHint", "Defaults to destination host"))
						.OnTextCommitted(this, &SUdpSenderSettingsPanel::HandleAudioHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpSenderAudioPortLabel", "Audio Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 0.f)
					[
						SAssignNew(AudioPortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(FMath::Max(1, GetAudioPort()))
						.OnValueChanged(this, &SUdpSenderSettingsPanel::HandleAudioPortChanged)
					]
				]
			];
		}

	private:
		FString GetOption(const FString& Key) const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(Key) : FString();
		}

		void SetOption(const FString& Key, const FString& Value, bool bNotify = false)
		{
			if (!SenderComponent)
			{
				return;
			}

			SenderComponent->SetTransportOption(Key, Value);
			if (bNotify && OnConfigChanged.IsBound())
			{
				OnConfigChanged.Execute();
			}
		}

		int32 GetPortInternal() const
		{
			const FString Value = GetOption(O3DSockets::PortOptionKey);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		int32 GetPort() const
		{
			const int32 Raw = GetPortInternal();
			return Raw > 0 ? Raw : DefaultUdpPort;
		}

		void SetPort(int32 Port, bool bNotify = false)
		{
			if (Port <= 0)
			{
				Port = DefaultUdpPort;
			}
			const int32 Clamped = FMath::Clamp(Port, 1, 65535);
			SetOption(O3DSockets::PortOptionKey, FString::FromInt(Clamped), bNotify);
		}

		FString GetHost() const
		{
			const FString Value = GetOption(O3DSockets::HostOptionKey);
			return Value.IsEmpty() ? FString(TEXT("127.0.0.1")) : Value;
		}

		void SetHost(const FString& Value, bool bNotify = false)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				Sanitized = TEXT("127.0.0.1");
			}
			SetOption(O3DSockets::HostOptionKey, O3DSockets::NormaliseHostname(Sanitized), bNotify);
		}

		bool GetBroadcast() const
		{
			return ParseBoolOption(GetOption(O3DSockets::BroadcastOptionKey), false);
		}

		void SetBroadcast(bool bEnabled, bool bNotify = false)
		{
			SetOption(O3DSockets::BroadcastOptionKey, bEnabled ? TEXT("true") : TEXT("false"), bNotify);
		}

		int32 GetMtuInternal() const
		{
			const FString Value = GetOption(O3DSockets::MtuOptionKey);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		int32 GetMtu() const
		{
			const int32 Raw = GetMtuInternal();
			return Raw > 0 ? Raw : 1200;
		}

		void SetMtu(int32 Value, bool bNotify = false)
		{
			const int32 Clamped = FMath::Clamp(Value > 0 ? Value : 1200, 256, 65507);
			SetOption(O3DSockets::MtuOptionKey, FString::FromInt(Clamped), bNotify);
		}

		int32 GetMaxDatagramInternal() const
		{
			const FString Value = GetOption(O3DSockets::MaxDatagramOptionKey);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		int32 GetMaxDatagram() const
		{
			const int32 Raw = GetMaxDatagramInternal();
			return Raw > 0 ? Raw : 64000;
		}

		void SetMaxDatagram(int32 Value, bool bNotify = false)
		{
			const int32 Clamped = FMath::Clamp(Value > 0 ? Value : 64000, 512, 65507);
			SetOption(O3DSockets::MaxDatagramOptionKey, FString::FromInt(Clamped), bNotify);
		}

		FString GetAudioHost() const
		{
			return GetOption(O3DSockets::AudioHostOptionKey);
		}

		void SetAudioHost(const FString& Value, bool bNotify = false)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				SetOption(O3DSockets::AudioHostOptionKey, FString(), bNotify);
			}
			else
			{
				SetOption(O3DSockets::AudioHostOptionKey, O3DSockets::NormaliseHostname(Sanitized), bNotify);
			}
		}

		int32 GetAudioPortInternal() const
		{
			const FString Value = GetOption(O3DSockets::AudioPortOptionKey);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		int32 GetAudioPort() const
		{
			const int32 Raw = GetAudioPortInternal();
			return Raw > 0 ? Raw : GetPort() + 1;
		}

		void SetAudioPort(int32 Port, bool bNotify = false)
		{
			if (Port <= 0)
			{
				Port = GetPort() + 1;
			}
			const int32 Clamped = FMath::Clamp(Port, 1, 65535);
			SetOption(O3DSockets::AudioPortOptionKey, FString::FromInt(Clamped), bNotify);
		}

		void HandleHostCommitted(const FText& NewText, ETextCommit::Type)
		{
			SetHost(NewText.ToString(), true);
			if (HostTextBox.IsValid())
			{
				HostTextBox->SetText(FText::FromString(GetHost()));
			}
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetPort(NewValue, true);
			if (AudioPortSpinBox.IsValid())
			{
				AudioPortSpinBox->SetValue(FMath::Max(1, GetAudioPort()));
			}
		}

		void HandleBroadcastChanged(ECheckBoxState NewState)
		{
			SetBroadcast(NewState == ECheckBoxState::Checked, true);
		}

		ECheckBoxState GetBroadcastCheckState() const
		{
			return GetBroadcast() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		}

		void HandleMtuChanged(int32 NewValue)
		{
			SetMtu(NewValue, true);
		}

		void HandleMaxDatagramChanged(int32 NewValue)
		{
			SetMaxDatagram(NewValue, true);
		}

		void HandleAudioHostCommitted(const FText& NewText, ETextCommit::Type)
		{
			SetAudioHost(NewText.ToString(), true);
			if (AudioHostTextBox.IsValid())
			{
				const FString Value = GetAudioHost();
				AudioHostTextBox->SetText(Value.IsEmpty() ? FText::GetEmpty() : FText::FromString(Value));
			}
		}

		void HandleAudioPortChanged(int32 NewValue)
		{
			SetAudioPort(NewValue, true);
		}

		UO3DSenderComponent* SenderComponent = nullptr;
		FSimpleDelegate OnConfigChanged;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SCheckBox> BroadcastCheckBox;
		TSharedPtr<SSpinBox<int32>> MtuSpinBox;
		TSharedPtr<SSpinBox<int32>> MaxDatagramSpinBox;
		TSharedPtr<SEditableTextBox> AudioHostTextBox;
		TSharedPtr<SSpinBox<int32>> AudioPortSpinBox;
	};
} // anonymous namespace

namespace SocketsEditor
{
namespace Sender
{
TSharedPtr<SWidget> BuildUdpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged)
{
	return SNew(SUdpSenderSettingsPanel)
		.SenderComponent(SenderComponent)
		.OnConfigChanged(OnConfigChanged);
}
} // namespace Sender
} // namespace SocketsEditor

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
