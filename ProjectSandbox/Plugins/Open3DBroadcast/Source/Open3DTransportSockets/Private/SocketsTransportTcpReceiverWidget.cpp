#include "SocketsTransportEditorWidgets.h"

#if WITH_EDITOR

#include "Math/UnrealMathUtility.h"
#include "O3DReceiverSourceSettings.h"
#include "SocketsTransportCommon.h"
#include "SocketsTransportConfig.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Open3DTransportTcpReceiver"

namespace
{
	using namespace O3DSocketsConfig;

	class STcpReceiverSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(STcpReceiverSettingsPanel) {}
			SLATE_ARGUMENT(UO3DReceiverSettingsObject*, SettingsObject)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SettingsObject = InArgs._SettingsObject;

			if (SettingsObject)
			{
				if (!SettingsObject->Settings.TransportOptions.Contains(O3DSockets::HostOptionKey))
				{
					SetHost(TEXT("127.0.0.1"));
				}

			const int32 ExistingPort = GetPortInternal();
			if (ExistingPort <= 0 || ExistingPort == DefaultUdpPort)
			{
				SetPort(DefaultTcpPort);
			}

			const int32 ExistingAudioPort = GetAudioPortInternal();
			if ((ExistingAudioPort <= 0 || ExistingAudioPort == DefaultUdpPort + 1) && GetPort() > 0)
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
						.Text(LOCTEXT("TcpReceiverHostLabel", "Remote Host"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(HostTextBox, SEditableTextBox)
						.Text(FText::FromString(GetHost()))
						.OnTextCommitted(this, &STcpReceiverSettingsPanel::HandleHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpReceiverPortLabel", "Data Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(PortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(GetPort())
						.OnValueChanged(this, &STcpReceiverSettingsPanel::HandlePortChanged)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpReceiverAudioHostLabel", "Audio Host"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(AudioHostTextBox, SEditableTextBox)
						.Text(CurrentAudioHost.IsEmpty() ? FText::GetEmpty() : FText::FromString(CurrentAudioHost))
						.HintText(LOCTEXT("TcpReceiverAudioHostHint", "Defaults to remote host"))
						.OnTextCommitted(this, &STcpReceiverSettingsPanel::HandleAudioHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpReceiverAudioPortLabel", "Audio Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 0.f)
					[
						SAssignNew(AudioPortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(FMath::Max(1, GetAudioPort()))
						.OnValueChanged(this, &STcpReceiverSettingsPanel::HandleAudioPortChanged)
					]
				]
			];
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
			return Value.IsEmpty() ? FString(TEXT("127.0.0.1")) : Value;
		}

		void SetHost(const FString& Value)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				Sanitized = TEXT("127.0.0.1");
			}
			SetOption(O3DSockets::HostOptionKey, O3DSockets::NormaliseHostname(Sanitized));
		}

		int32 GetPortInternal() const
		{
			const FString Value = GetOption(O3DSockets::PortOptionKey);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		int32 GetPort() const
		{
			const int32 Raw = GetPortInternal();
			return Raw > 0 ? Raw : DefaultTcpPort;
		}

		void SetPort(int32 Port)
		{
			if (Port <= 0)
			{
				Port = DefaultTcpPort;
			}
			SetOption(O3DSockets::PortOptionKey, FString::FromInt(FMath::Clamp(Port, 1, 65535)));
		}

		FString GetAudioHost() const
		{
			return GetOption(O3DSockets::AudioHostOptionKey);
		}

		void SetAudioHost(const FString& Value)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				SetOption(O3DSockets::AudioHostOptionKey, FString());
			}
			else
			{
				SetOption(O3DSockets::AudioHostOptionKey, O3DSockets::NormaliseHostname(Sanitized));
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

		void SetAudioPort(int32 Port)
		{
			if (Port <= 0)
			{
				Port = GetPort() + 1;
			}
			SetOption(O3DSockets::AudioPortOptionKey, FString::FromInt(FMath::Clamp(Port, 1, 65535)));
		}

		void HandleHostCommitted(const FText& NewText, ETextCommit::Type)
		{
			SetHost(NewText.ToString());
			if (HostTextBox.IsValid())
			{
				HostTextBox->SetText(FText::FromString(GetHost()));
			}
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetPort(NewValue);
			if (AudioPortSpinBox.IsValid())
			{
				AudioPortSpinBox->SetValue(FMath::Max(1, GetAudioPort()));
			}
		}

		void HandleAudioHostCommitted(const FText& NewText, ETextCommit::Type)
		{
			SetAudioHost(NewText.ToString());
			if (AudioHostTextBox.IsValid())
			{
				const FString Value = GetAudioHost();
				AudioHostTextBox->SetText(Value.IsEmpty() ? FText::GetEmpty() : FText::FromString(Value));
			}
		}

		void HandleAudioPortChanged(int32 NewValue)
		{
			SetAudioPort(NewValue);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SEditableTextBox> AudioHostTextBox;
		TSharedPtr<SSpinBox<int32>> AudioPortSpinBox;
	};
} // anonymous namespace

namespace SocketsEditor
{
namespace Receiver
{
TSharedPtr<SWidget> BuildTcpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject)
{
	return SNew(STcpReceiverSettingsPanel)
		.SettingsObject(SettingsObject);
}
} // namespace Receiver
} // namespace SocketsEditor

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
