#include "SocketsTransportEditorWidgets.h"

#if WITH_EDITOR

#include "Math/UnrealMathUtility.h"
#include "O3DReceiverSourceSettings.h"
#include "SocketsTransportCommon.h"
#include "SocketsTransportConfig.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Open3DTransportTcpReceiver"

namespace
{
	using namespace O3DSocketsConfig;

	class STcpReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(STcpReceiverSettingsPanel)
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

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TcpReceiverHostLabel", "Remote Host"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(HostTextBox, SEditableTextBox)
					.Text(FText::FromString(GetHost()))
					.OnTextCommitted(this, &STcpReceiverSettingsPanel::HandleHostCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TcpReceiverPortLabel", "Data Port"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(PortSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(65535)
					.Value(GetPort())
					.OnValueChanged(this, &STcpReceiverSettingsPanel::HandlePortChanged)
					.OnValueCommitted(this, &STcpReceiverSettingsPanel::HandlePortCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TcpReceiverAudioHostLabel", "Audio Host"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(AudioHostTextBox, SEditableTextBox)
					.Text(CurrentAudioHost.IsEmpty() ? FText::GetEmpty() : FText::FromString(CurrentAudioHost))
					.HintText(LOCTEXT("TcpReceiverAudioHostHint", "Defaults to remote host"))
					.OnTextCommitted(this, &STcpReceiverSettingsPanel::HandleAudioHostCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TcpReceiverAudioPortLabel", "Audio Port"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(AudioPortSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(65535)
					.Value(FMath::Max(1, GetAudioPort()))
					.OnValueChanged(this, &STcpReceiverSettingsPanel::HandleAudioPortChanged)
					.OnValueCommitted(this, &STcpReceiverSettingsPanel::HandleAudioPortCommitted)
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TcpReceiverTimeoutLabel", "Connection Timeout (seconds)"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TimeoutSpinBox, SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(60)
					.Value(GetTimeout())
					.ToolTipText(LOCTEXT("TcpReceiverTimeoutTooltip", "Reconnect if no data received for this many seconds (1-60)"))
					.OnValueChanged(this, &STcpReceiverSettingsPanel::HandleTimeoutChanged)
					.OnValueCommitted(this, &STcpReceiverSettingsPanel::HandleTimeoutCommitted)
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

		int32 GetTimeout() const
		{
			const FString Value = GetOption(O3DSockets::TimeoutOptionKey);
			return Value.IsEmpty() ? 5 : FCString::Atoi(*Value);
		}

		void SetTimeout(int32 Seconds)
		{
			if (Seconds <= 0)
			{
				Seconds = 5;
			}
			SetOption(O3DSockets::TimeoutOptionKey, FString::FromInt(FMath::Clamp(Seconds, 1, 60)));
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
			if (AudioPortSpinBox.IsValid())
			{
				AudioPortSpinBox->SetValue(FMath::Max(1, GetAudioPort()));
			}
		}

		void HandlePortCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandlePortChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		void HandleAudioHostCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetAudioHost(NewText.ToString());
			if (AudioHostTextBox.IsValid())
			{
				const FString Value = GetAudioHost();
				AudioHostTextBox->SetText(Value.IsEmpty() ? FText::GetEmpty() : FText::FromString(Value));
			}

			SubmitFromTextCommit(CommitType);
		}

		void HandleAudioPortChanged(int32 NewValue)
		{
			SetAudioPort(NewValue);
		}

		void HandleAudioPortCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandleAudioPortChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		void HandleTimeoutChanged(int32 NewValue)
		{
			SetTimeout(NewValue);
		}

		void HandleTimeoutCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			HandleTimeoutChanged(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> HostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
		TSharedPtr<SEditableTextBox> AudioHostTextBox;
		TSharedPtr<SSpinBox<int32>> AudioPortSpinBox;
		TSharedPtr<SSpinBox<int32>> TimeoutSpinBox;
	};
} // anonymous namespace

namespace SocketsEditor
{
namespace Receiver
{
TSharedPtr<SO3DTransportConfigPanelBase> BuildTcpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit)
{
	if (!SettingsObject)
	{
		return nullptr;
	}

	return SNew(STcpReceiverSettingsPanel)
		.SettingsObject(SettingsObject)
		.PanelWidthOverride(SocketsEditor::TransportPanelWidth)
		.OnSubmit(OnSubmit);
}
} // namespace Receiver
} // namespace SocketsEditor

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
