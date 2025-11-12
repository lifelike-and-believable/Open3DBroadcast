#include "SocketsTransportEditorWidgets.h"

#if WITH_EDITOR

#include "Math/UnrealMathUtility.h"
#include "O3DSenderComponent.h"
#include "SocketsTransportCommon.h"
#include "SocketsTransportConfig.h"

#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Open3DTransportTcpSender"

namespace
{
	using namespace O3DSocketsConfig;

	class STcpSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(STcpSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			if (SenderComponent)
			{
				if (SenderComponent->GetTransportOption(O3DSockets::BindOptionKey).IsEmpty())
				{
					SetBindHost(TEXT("0.0.0.0"));
				}

			const int32 ExistingDataPort = GetDataPortInternal();
			if (ExistingDataPort <= 0 || ExistingDataPort == DefaultUdpPort)
			{
				SetDataPort(DefaultTcpPort);
			}

			const int32 ExistingAudioPort = GetAudioPortInternal();
			if ((ExistingAudioPort <= 0 || ExistingAudioPort == DefaultUdpPort + 1) && GetDataPort() > 0)
			{
				SetAudioPort(GetDataPort() + 1);
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
						.Text(LOCTEXT("TcpSenderBindHostLabel", "Bind Address"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(BindHostTextBox, SEditableTextBox)
						.Text(FText::FromString(GetBindHost()))
						.OnTextCommitted(this, &STcpSenderSettingsPanel::HandleBindHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpSenderDataPortLabel", "Data Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(DataPortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(GetDataPort())
						.OnValueChanged(this, &STcpSenderSettingsPanel::HandlePortChanged)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpSenderAudioHostLabel", "Audio Bind Address"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 8.f)
					[
						SAssignNew(AudioHostTextBox, SEditableTextBox)
						.Text(CurrentAudioHost.IsEmpty() ? FText::GetEmpty() : FText::FromString(CurrentAudioHost))
						.HintText(LOCTEXT("TcpSenderAudioHostHint", "Defaults to bind address"))
						.OnTextCommitted(this, &STcpSenderSettingsPanel::HandleAudioHostCommitted)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TcpSenderAudioPortLabel", "Audio Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 0.f)
					[
						SAssignNew(AudioPortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(FMath::Max(1, GetAudioPort()))
						.OnValueChanged(this, &STcpSenderSettingsPanel::HandleAudioPortChanged)
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

		int32 GetPortInternal(const FString& Key) const
		{
			const FString Value = GetOption(Key);
			return Value.IsEmpty() ? 0 : FCString::Atoi(*Value);
		}

		void SetPortInternal(const FString& Key, int32 Port, bool bNotify = false)
		{
			const int32 Clamped = FMath::Clamp(Port, 1, 65535);
			SetOption(Key, FString::FromInt(Clamped), bNotify);
		}

		FString GetBindHost() const
		{
			const FString Value = GetOption(O3DSockets::BindOptionKey);
			return Value.IsEmpty() ? FString(TEXT("0.0.0.0")) : Value;
		}

		void SetBindHost(const FString& Value, bool bNotify = false)
		{
			FString Sanitized = Value.TrimStartAndEnd();
			if (Sanitized.IsEmpty())
			{
				Sanitized = TEXT("0.0.0.0");
			}
			SetOption(O3DSockets::BindOptionKey, O3DSockets::NormaliseHostname(Sanitized), bNotify);
		}

		int32 GetDataPortInternal() const
		{
			return GetPortInternal(O3DSockets::PortOptionKey);
		}

		int32 GetDataPort() const
		{
			const int32 Raw = GetDataPortInternal();
			return Raw > 0 ? Raw : DefaultTcpPort;
		}

		void SetDataPort(int32 Port, bool bNotify = false)
		{
			if (Port <= 0)
			{
				Port = DefaultTcpPort;
			}
			SetPortInternal(O3DSockets::PortOptionKey, Port, bNotify);
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
			return GetPortInternal(O3DSockets::AudioPortOptionKey);
		}

		int32 GetAudioPort() const
		{
			const int32 Raw = GetAudioPortInternal();
			return Raw > 0 ? Raw : GetDataPort() + 1;
		}

		void SetAudioPort(int32 Port, bool bNotify = false)
		{
			if (Port <= 0)
			{
				Port = GetDataPort() + 1;
			}
			SetPortInternal(O3DSockets::AudioPortOptionKey, Port, bNotify);
		}

		void HandleBindHostCommitted(const FText& NewText, ETextCommit::Type)
		{
			SetBindHost(NewText.ToString(), true);
			if (BindHostTextBox.IsValid())
			{
				BindHostTextBox->SetText(FText::FromString(GetBindHost()));
			}
		}

		void HandlePortChanged(int32 NewValue)
		{
			SetDataPort(NewValue, true);
			if (AudioPortSpinBox.IsValid())
			{
				AudioPortSpinBox->SetValue(FMath::Max(1, GetAudioPort()));
			}
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
		TSharedPtr<SEditableTextBox> BindHostTextBox;
		TSharedPtr<SSpinBox<int32>> DataPortSpinBox;
		TSharedPtr<SEditableTextBox> AudioHostTextBox;
		TSharedPtr<SSpinBox<int32>> AudioPortSpinBox;
	};
} // anonymous namespace

namespace SocketsEditor
{
namespace Sender
{
TSharedPtr<SWidget> BuildTcpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged)
{
	return SNew(STcpSenderSettingsPanel)
		.SenderComponent(SenderComponent)
		.OnConfigChanged(OnConfigChanged);
}
} // namespace Sender
} // namespace SocketsEditor

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
