#include "SocketsTransportEditorWidgets.h"

#if WITH_EDITOR

#include "Math/UnrealMathUtility.h"
#include "O3DReceiverSourceSettings.h"
#include "SocketsTransportCommon.h"
#include "SocketsTransportConfig.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Open3DTransportUdpReceiver"

namespace
{
	using namespace O3DSocketsConfig;

	class SUdpReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(SUdpReceiverSettingsPanel)
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

			if (SettingsObject)
			{
				if (!SettingsObject->Settings.TransportOptions.Contains(O3DSockets::HostOptionKey))
				{
					SetHost(TEXT("0.0.0.0"));
				}

			const int32 ExistingPort = GetPortInternal();
			if (ExistingPort <= 0 || ExistingPort == DefaultTcpPort)
			{
				SetPort(DefaultUdpPort);
			}

				if (!SettingsObject->Settings.TransportOptions.Contains(O3DSockets::BroadcastOptionKey))
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
			}			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UdpReceiverBindHostLabel", "Bind Address"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(HostTextBox, SEditableTextBox)
					.Text(FText::FromString(GetHost()))
					.OnTextCommitted(this, &SUdpReceiverSettingsPanel::HandleHostCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UdpReceiverPortLabel", "Port"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(PortSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(65535)
					.Value(GetPort())
					.OnValueChanged(this, &SUdpReceiverSettingsPanel::HandlePortChanged)
					.OnValueCommitted(this, &SUdpReceiverSettingsPanel::HandlePortCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SAssignNew(BroadcastCheckBox, SCheckBox)
						.IsChecked(this, &SUdpReceiverSettingsPanel::GetBroadcastCheckState)
						.OnCheckStateChanged(this, &SUdpReceiverSettingsPanel::HandleBroadcastChanged)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					.Padding(4.f, 0.f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UdpReceiverBroadcastLabel", "Accept Broadcast Packets"))
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UdpReceiverMtuLabel", "MTU"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(MtuSpinBox, SSpinBox<int32>)
					.MinValue(256)
					.MaxValue(65507)
					.Value(GetMtu())
					.OnValueChanged(this, &SUdpReceiverSettingsPanel::HandleMtuChanged)
					.OnValueCommitted(this, &SUdpReceiverSettingsPanel::HandleMtuCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UdpReceiverMaxDatagramLabel", "Max Datagram Bytes"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(MaxDatagramSpinBox, SSpinBox<int32>)
					.MinValue(512)
					.MaxValue(65507)
					.Value(GetMaxDatagram())
					.OnValueChanged(this, &SUdpReceiverSettingsPanel::HandleMaxDatagramChanged)
					.OnValueCommitted(this, &SUdpReceiverSettingsPanel::HandleMaxDatagramCommitted)
				];

			BuildPanel(PanelContent, InArgs._PanelWidthOverride);
		}

	private:
		FString GetOption(const FString& Key) const
		{
			if (!SettingsObject)
			{
				return FString();
			}
			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(Key))
			{
				return *Existing;
			}
			return FString();
		}

		void SetOption(const FString& Key, const FString& Value)
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

		FString GetHost() const
		{
			const FString Value = GetOption(O3DSockets::HostOptionKey);
			return Value.IsEmpty() ? FString(TEXT("0.0.0.0")) : Value;
		}

		void SetHost(const FString& Value)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				Sanitized = TEXT("0.0.0.0");
			}
			SetOption(O3DSockets::HostOptionKey, O3DSockets::NormaliseHostname(Sanitized));
		}

		bool GetBroadcast() const
		{
			return ParseBoolOption(GetOption(O3DSockets::BroadcastOptionKey), false);
		}

		void SetBroadcast(bool bEnabled)
		{
			SetOption(O3DSockets::BroadcastOptionKey, bEnabled ? TEXT("true") : TEXT("false"));
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

		void SetPort(int32 Port)
		{
			if (Port <= 0)
			{
				Port = DefaultUdpPort;
			}
			SetOption(O3DSockets::PortOptionKey, FString::FromInt(FMath::Clamp(Port, 1, 65535)));
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

		void SetMtu(int32 Value)
		{
			const int32 Clamped = FMath::Clamp(Value > 0 ? Value : 1200, 256, 65507);
			SetOption(O3DSockets::MtuOptionKey, FString::FromInt(Clamped));
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

		void SetMaxDatagram(int32 Value)
		{
			const int32 Clamped = FMath::Clamp(Value > 0 ? Value : 64000, 512, 65507);
			SetOption(O3DSockets::MaxDatagramOptionKey, FString::FromInt(Clamped));
		}

		void HandleHostCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetHost(NewText.ToString());
			if (HostTextBox.IsValid())
			{
				HostTextBox->SetText(FText::FromString(GetHost()));
			}

			SubmitFromTextCommit(CommitType);
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetPort(NewValue);
		}

		void HandlePortCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandlePortChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		void HandleBroadcastChanged(ECheckBoxState NewState)
		{
			SetBroadcast(NewState == ECheckBoxState::Checked);
		}

		ECheckBoxState GetBroadcastCheckState() const
		{
			return GetBroadcast() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		}

		void HandleMtuChanged(int32 NewValue)
		{
			SetMtu(NewValue);
		}

		void HandleMtuCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandleMtuChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		void HandleMaxDatagramChanged(int32 NewValue)
		{
			SetMaxDatagram(NewValue);
		}

		void HandleMaxDatagramCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandleMaxDatagramChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SCheckBox> BroadcastCheckBox;
		TSharedPtr<SSpinBox<int32>> MtuSpinBox;
		TSharedPtr<SSpinBox<int32>> MaxDatagramSpinBox;
	};
} // anonymous namespace

namespace SocketsEditor
{
namespace Receiver
{
TSharedPtr<SO3DTransportConfigPanelBase> BuildUdpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit)
{
	if (!SettingsObject)
	{
		return nullptr;
	}

	return SNew(SUdpReceiverSettingsPanel)
		.SettingsObject(SettingsObject)
		.PanelWidthOverride(SocketsEditor::TransportPanelWidth)
		.OnSubmit(OnSubmit);
}
} // namespace Receiver
} // namespace SocketsEditor

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
