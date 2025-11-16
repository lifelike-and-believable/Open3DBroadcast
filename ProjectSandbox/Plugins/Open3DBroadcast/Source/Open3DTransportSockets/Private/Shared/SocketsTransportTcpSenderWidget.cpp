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

				const int32 ExistingPort = GetPortInternal();
				if (ExistingPort <= 0 || ExistingPort == DefaultUdpPort)
				{
					SetPort(DefaultTcpPort);
				}
			}			ChildSlot
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
						.Text(LOCTEXT("TcpSenderPortLabel", "Port"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.f, 4.f, 0.f, 0.f)
					[
						SAssignNew(PortSpinBox, SSpinBox<int32>)
						.MinValue(1)
						.MaxValue(65535)
						.Value(GetPort())
						.OnValueChanged(this, &STcpSenderSettingsPanel::HandlePortChanged)
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

		int32 GetPortInternal() const
		{
			return GetPortInternal(O3DSockets::PortOptionKey);
		}

		int32 GetPort() const
		{
			const int32 Raw = GetPortInternal();
			return Raw > 0 ? Raw : DefaultTcpPort;
		}

		void SetPort(int32 Port, bool bNotify = false)
		{
			if (Port <= 0)
			{
				Port = DefaultTcpPort;
			}
			SetPortInternal(O3DSockets::PortOptionKey, Port, bNotify);
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
			SetPort(NewValue, true);
		}

		UO3DSenderComponent* SenderComponent = nullptr;
		FSimpleDelegate OnConfigChanged;
		TSharedPtr<SEditableTextBox> BindHostTextBox;
		TSharedPtr<SSpinBox<int32>> PortSpinBox;
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
