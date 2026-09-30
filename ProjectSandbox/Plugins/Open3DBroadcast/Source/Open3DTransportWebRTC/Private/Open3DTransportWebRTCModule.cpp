#include "Modules/ModuleManager.h"
#include "O3DFfiLibrary.h"
#include "Sender/WebRTCSender.h"
#include "Receiver/WebRTCReceiver.h"
#include "Shared/WebRTCUtils.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTransportConfigPanelBase.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"

#if WITH_EDITOR
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY(LogO3DWebRTCSender);
DEFINE_LOG_CATEGORY(LogO3DWebRTCReceiver);

#define LOCTEXT_NAMESPACE "Open3DTransportWebRTC"

namespace WebRTCConfig
{
	/** Registered transport name; the secret store and the credential profile option are keyed by it. */
	static constexpr TCHAR TransportName[] = TEXT("WebRTC");

	static constexpr TCHAR UrlOptionKey[] = TEXT("webrtc.url");
	static constexpr const TCHAR* TokenOptionKey = WebRTCUtils::TokenOptionKey;
	static constexpr const TCHAR* TokenEndpointAuthKey = WebRTCUtils::TokenEndpointAuthOptionKey;
	static constexpr TCHAR UseAutoTokenFetchKey[] = TEXT("webrtc.useAutoTokenFetch");
	static constexpr TCHAR TokenEndpointUrlKey[] = TEXT("webrtc.tokenEndpointUrl");
	static constexpr TCHAR TokenRefreshLeadTimeKey[] = TEXT("webrtc.tokenRefreshLeadTimeSec");
	// LiveKit room requested from the token endpoint; must match on sender and receiver (TRF-25).
	static constexpr const TCHAR* RoomOptionKey = WebRTCUtils::RoomOptionKey;

	// Token refresh lead time constants
	static constexpr int32 MinTokenRefreshLeadTimeSec = 60;     // 1 minute
	static constexpr int32 MaxTokenRefreshLeadTimeSec = 3600;   // 1 hour
	static constexpr int32 DefaultTokenRefreshLeadTimeSec = 300; // 5 minutes

	/** Non-secret option selecting the credential profile ("webrtc.credentialProfile"). */
	static FString GetCredentialProfileKey()
	{
		return FO3DSecretStore::MakeCredentialProfileOptionKey(TransportName);
	}

	/** Secret keys and their environment variables (ADR 0004 item 1). */
	static void DeclareSecrets(TArray<FString>& OutKeys, TMap<FString, FString>& OutEnvVars)
	{
		OutKeys = { FString(TokenOptionKey), FString(TokenEndpointAuthKey) };
		OutEnvVars.Add(TokenOptionKey, WebRTCUtils::TokenEnvVar);
		OutEnvVars.Add(TokenEndpointAuthKey, WebRTCUtils::TokenEndpointAuthEnvVar);
	}

	static FString GetEnvVarForKey(const TCHAR* Key)
	{
		return FCString::Strcmp(Key, TokenOptionKey) == 0 ? FString(WebRTCUtils::TokenEnvVar) : FString(WebRTCUtils::TokenEndpointAuthEnvVar);
	}

	// Sender config helpers
	static FString GetSenderOption(const UO3DSenderComponent* Component, const TCHAR* Key)
	{
		return Component ? Component->GetTransportOption(Key) : FString();
	}

	// Receiver config helpers
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
}

#if WITH_EDITOR
namespace WebRTCSecretsUI
{
	/**
	 * How a secret field reads status and writes values, so one widget serves the sender component
	 * and the receiver settings object. None of these returns a secret value.
	 */
	struct FSecretBinding
	{
		TFunction<FO3DSecretStatus()> GetStatus;
		TFunction<void(const FString& /*Value*/, EO3DSecretPersistence)> Set;
		TFunction<bool(EO3DSecretPersistence)> SetPersistence;
		TFunction<void()> Clear;
	};

	DECLARE_DELEGATE_OneParam(FOnSecretCommitted, ETextCommit::Type);

	/**
	 * A password box that always opens empty (ADR 0004 item 5), a Clear button, a "Remember on
	 * this machine" checkbox and a status line saying where the current value comes from.
	 * Committing an empty box does nothing; Clear removes the value.
	 */
	class SWebRTCSecretField : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SWebRTCSecretField) {}
			SLATE_ARGUMENT(FText, Label)
			SLATE_ARGUMENT(FText, LabelToolTip)
			SLATE_ARGUMENT(FText, HintText)
			SLATE_EVENT(FOnSecretCommitted, OnCommitted)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, FSecretBinding InBinding)
		{
			Binding = MoveTemp(InBinding);
			OnCommitted = InArgs._OnCommitted;
			bRemember = Binding.GetStatus ? Binding.GetStatus().bRemembered : false;

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(InArgs._Label)
					.ToolTipText(InArgs._LabelToolTip)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 2.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					[
						SAssignNew(ValueTextBox, SEditableTextBox)
						.Text(FText::GetEmpty())
						.IsPassword(true)
						.HintText(InArgs._HintText)
						.OnTextCommitted(this, &SWebRTCSecretField::HandleCommitted)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SecretClear", "Clear"))
						.ToolTipText(LOCTEXT("SecretClearTooltip", "Forget the value for this session and on this machine. An environment variable still applies."))
						.OnClicked(this, &SWebRTCSecretField::HandleClearClicked)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 2.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SCheckBox)
						.IsChecked(this, &SWebRTCSecretField::GetRememberState)
						.OnCheckStateChanged(this, &SWebRTCSecretField::HandleRememberChanged)
						.ToolTipText(LOCTEXT("SecretRememberTooltip", "Keep the value in your per-user editor settings under Saved/Config on this machine. It is never written to the level, the Blueprint, the project ini or a LiveLink preset."))
					]
					+ SHorizontalBox::Slot()
					.Padding(4.f, 0.f, 0.f, 0.f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SecretRememberLabel", "Remember on this machine"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(STextBlock)
					.Text(this, &SWebRTCSecretField::GetStatusText)
				]
			];
		}

	private:
		void HandleCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			const FString Value = NewText.ToString().TrimStartAndEnd();
			if (Value.IsEmpty())
			{
				// The box opens empty; leaving it empty keeps the current value. Clear removes it.
				return;
			}

			if (Binding.Set)
			{
				Binding.Set(Value, bRemember ? EO3DSecretPersistence::RememberOnThisMachine : EO3DSecretPersistence::Session);
			}
			if (ValueTextBox.IsValid())
			{
				ValueTextBox->SetText(FText::GetEmpty());
			}
			OnCommitted.ExecuteIfBound(CommitType);
		}

		FReply HandleClearClicked()
		{
			if (Binding.Clear)
			{
				Binding.Clear();
			}
			bRemember = false;
			if (ValueTextBox.IsValid())
			{
				ValueTextBox->SetText(FText::GetEmpty());
			}
			OnCommitted.ExecuteIfBound(ETextCommit::Default);
			return FReply::Handled();
		}

		ECheckBoxState GetRememberState() const
		{
			return bRemember ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		}

		void HandleRememberChanged(ECheckBoxState NewState)
		{
			bRemember = (NewState == ECheckBoxState::Checked);
			// Moves a value set this session; unchecking also forgets a remembered copy.
			if (Binding.SetPersistence)
			{
				Binding.SetPersistence(bRemember ? EO3DSecretPersistence::RememberOnThisMachine : EO3DSecretPersistence::Session);
			}
		}

		FText GetStatusText() const
		{
			const FO3DSecretStatus Status = Binding.GetStatus ? Binding.GetStatus() : FO3DSecretStatus();
			switch (Status.Source)
			{
			case EO3DSecretSource::Session:
				return Status.bRemembered
					? LOCTEXT("SecretStatusSessionRemembered", "Set for this session and remembered on this machine")
					: LOCTEXT("SecretStatusSession", "Set for this session (gone after restart)");
			case EO3DSecretSource::Environment:
				return FText::Format(LOCTEXT("SecretStatusEnv", "From environment variable {0}"), FText::FromString(Status.EnvVarName));
			case EO3DSecretSource::UserSettings:
				return LOCTEXT("SecretStatusRemembered", "Remembered on this machine");
			default:
				return LOCTEXT("SecretStatusNotSet", "Not set");
			}
		}

		FSecretBinding Binding;
		FOnSecretCommitted OnCommitted;
		TSharedPtr<SEditableTextBox> ValueTextBox;
		bool bRemember = false;
	};

	static FSecretBinding MakeSenderBinding(UO3DSenderComponent* Component, const TCHAR* Key)
	{
		const TWeakObjectPtr<UO3DSenderComponent> Weak(Component);
		const FString KeyString(Key);

		FSecretBinding Binding;
		Binding.GetStatus = [Weak, KeyString]()
		{
			const UO3DSenderComponent* Pinned = Weak.Get();
			return Pinned ? Pinned->GetTransportSecretStatus(KeyString) : FO3DSecretStatus();
		};
		Binding.Set = [Weak, KeyString](const FString& Value, EO3DSecretPersistence Persistence)
		{
			if (UO3DSenderComponent* Pinned = Weak.Get())
			{
				Pinned->SetTransportSecret(KeyString, Value, Persistence);
			}
		};
		Binding.SetPersistence = [Weak, KeyString](EO3DSecretPersistence Persistence)
		{
			UO3DSenderComponent* Pinned = Weak.Get();
			return Pinned ? Pinned->SetTransportSecretPersistence(KeyString, Persistence) : false;
		};
		Binding.Clear = [Weak, KeyString]()
		{
			if (UO3DSenderComponent* Pinned = Weak.Get())
			{
				Pinned->ClearTransportSecret(KeyString);
			}
		};
		return Binding;
	}

	static FSecretBinding MakeReceiverBinding(UO3DReceiverSettingsObject* SettingsObject, const TCHAR* Key)
	{
		const TWeakObjectPtr<UO3DReceiverSettingsObject> Weak(SettingsObject);
		const FString KeyString(Key);
		const FString EnvVar = WebRTCConfig::GetEnvVarForKey(Key);

		// Receiver secrets are keyed by the transport and the profile the settings select.
		auto Profile = [Weak]()
		{
			const UO3DReceiverSettingsObject* Pinned = Weak.Get();
			return Pinned ? O3DReceiver::GetCredentialProfile(Pinned->Settings) : FString(FO3DSecretStore::DefaultProfile());
		};

		FSecretBinding Binding;
		Binding.GetStatus = [Profile, KeyString, EnvVar]()
		{
			return FO3DSecretStore::Get().Describe(WebRTCConfig::TransportName, Profile(), KeyString, EnvVar);
		};
		Binding.Set = [Profile, KeyString](const FString& Value, EO3DSecretPersistence Persistence)
		{
			FO3DSecretStore::Get().Set(WebRTCConfig::TransportName, Profile(), KeyString, Value, Persistence);
		};
		Binding.SetPersistence = [Profile, KeyString](EO3DSecretPersistence Persistence)
		{
			return FO3DSecretStore::Get().SetPersistence(WebRTCConfig::TransportName, Profile(), KeyString, Persistence);
		};
		Binding.Clear = [Profile, KeyString]()
		{
			FO3DSecretStore::Get().Clear(WebRTCConfig::TransportName, Profile(), KeyString);
		};
		return Binding;
	}

	static FText GetTokenLabelToolTip()
	{
		return FText::Format(LOCTEXT("WebRTCTokenTooltip", "LiveKit JWT access token, used when Auto Token Fetch is off. Kept out of the level, Blueprint, ini and LiveLink presets. Can also come from the {0} environment variable."),
			FText::FromString(WebRTCUtils::TokenEnvVar));
	}

	static FText GetEndpointAuthLabelToolTip()
	{
		return FText::Format(LOCTEXT("WebRTCEndpointAuthTooltip", "Credential for your token endpoint, sent as 'Authorization: Bearer <value>'. The endpoint must authenticate callers and decide their grants. Can also come from the {0} environment variable."),
			FText::FromString(WebRTCUtils::TokenEndpointAuthEnvVar));
	}

	static FText GetMissingTokenWarning()
	{
		return FText::Format(LOCTEXT("WebRTCMissingToken", "No access token is set. Enter one above, set {0}, or enable Auto Token Fetch."),
			FText::FromString(WebRTCUtils::TokenEnvVar));
	}
}

namespace WebRTCSender
{
	class SWebRTCSenderSettingsPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SWebRTCSenderSettingsPanel) {}
			SLATE_ARGUMENT(UO3DSenderComponent*, SenderComponent)
			SLATE_ARGUMENT(FSimpleDelegate, OnConfigChanged)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SenderComponent = InArgs._SenderComponent;
			OnConfigChanged = InArgs._OnConfigChanged;

			const FString InitialUrl = ResolveUrlValue();
			const bool bInitialUseAutoFetch = ResolveUseAutoTokenFetch();
			const FString InitialTokenEndpoint = ResolveTokenEndpointUrl();
			const int32 InitialRefreshLeadTime = ResolveTokenRefreshLeadTime();
			const FString InitialRoom = ResolveRoomValue();
			const FString InitialProfile = ResolveProfileValue();

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCUrlLabel", "LiveKit Host"))
					.ToolTipText(LOCTEXT("WebRTCUrlTooltip", "The WebSocket URL of your LiveKit server (e.g., wss://livekit.example.com)"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(UrlTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialUrl))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleUrlCommitted)
					.HintText(LOCTEXT("WebRTCUrlHint", "e.g., wss://livekit.example.com or ws://127.0.0.1:7880"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(UseAutoTokenFetchCheckBox, SCheckBox)
						.IsChecked(bInitialUseAutoFetch ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
						.OnCheckStateChanged(this, &SWebRTCSenderSettingsPanel::HandleUseAutoTokenFetchChanged)
						.ToolTipText(LOCTEXT("WebRTCUseAutoTokenFetchCheckboxTooltip", "Automatically fetch JWT tokens from a token generator endpoint instead of manually entering them. Requires a token server that implements the LiveKit token generation API."))
					]
					+ SHorizontalBox::Slot()
					.Padding(8.f, 0.f, 0.f, 0.f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("WebRTCUseAutoTokenFetchLabel", "Use Auto Token Fetch"))
						.ToolTipText(LOCTEXT("WebRTCUseAutoTokenFetchTooltip", "Automatically fetch JWT tokens from a token generator endpoint instead of manually entering them. Requires a token server that implements the LiveKit token generation API."))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCProfileLabel", "Credential Profile"))
					.ToolTipText(LOCTEXT("WebRTCProfileTooltip", "Names the stored credentials this component uses (saved with the component; the credentials are not). Leave empty for 'default'."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SNew(SEditableTextBox)
					.Text(FText::FromString(InitialProfile))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleProfileCommitted)
					.HintText(LOCTEXT("WebRTCProfileHint", "default"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(WebRTCSecretsUI::SWebRTCSecretField, WebRTCSecretsUI::MakeSenderBinding(SenderComponent, WebRTCConfig::TokenOptionKey))
						.Label(LOCTEXT("WebRTCTokenLabel", "Access Token"))
						.LabelToolTip(WebRTCSecretsUI::GetTokenLabelToolTip())
						.HintText(LOCTEXT("WebRTCTokenHint", "Paste a LiveKit access token"))
						.OnCommitted(WebRTCSecretsUI::FOnSecretCommitted::CreateSP(this, &SWebRTCSenderSettingsPanel::HandleSecretCommitted))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(STextBlock)
					.Text(WebRTCSecretsUI::GetMissingTokenWarning())
					.ColorAndOpacity(FLinearColor(1.f, 0.75f, 0.2f))
					.AutoWrapText(true)
					.Visibility_Lambda([this]() { return IsTokenMissing() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCTokenEndpointLabel", "Token Endpoint URL"))
					.ToolTipText(LOCTEXT("WebRTCTokenEndpointTooltip", "The HTTPS endpoint that issues LiveKit tokens (e.g., https://myserver.com/token). It receives room, identity and role, must authenticate the caller and decides the grants. Plain http:// is accepted only for localhost, 127.0.0.1 and ::1."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(TokenEndpointTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialTokenEndpoint))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleTokenEndpointCommitted)
					.HintText(LOCTEXT("WebRTCTokenEndpointHint", "https://myserver.com/token"))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(WebRTCSecretsUI::SWebRTCSecretField, WebRTCSecretsUI::MakeSenderBinding(SenderComponent, WebRTCConfig::TokenEndpointAuthKey))
						.Label(LOCTEXT("WebRTCEndpointAuthLabel", "Token Endpoint Credential"))
						.LabelToolTip(WebRTCSecretsUI::GetEndpointAuthLabelToolTip())
						.HintText(LOCTEXT("WebRTCEndpointAuthHint", "Bearer credential for your token endpoint"))
						.OnCommitted(WebRTCSecretsUI::FOnSecretCommitted::CreateSP(this, &SWebRTCSenderSettingsPanel::HandleSecretCommitted))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCRoomLabel", "Room"))
					.ToolTipText(LOCTEXT("WebRTCRoomTooltip", "LiveKit room requested from the token endpoint. Use the same room on the sender and the receiver. Required for Auto Token Fetch."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(RoomTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialRoom))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleRoomCommitted)
					.HintText(LOCTEXT("WebRTCRoomHint", "e.g., my-stage"))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCTokenRefreshLeadTimeLabel", "Token Refresh Lead Time (seconds)"))
					.ToolTipText(LOCTEXT("WebRTCTokenRefreshLeadTimeTooltip", "How many seconds before token expiry to trigger an automatic refresh. Default is 300 seconds (5 minutes)."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TokenRefreshLeadTimeSpinBox, SSpinBox<int32>)
					.Value(InitialRefreshLeadTime)
					.MinValue(WebRTCConfig::MinTokenRefreshLeadTimeSec)
					.MaxValue(WebRTCConfig::MaxTokenRefreshLeadTimeSec)
					.OnValueCommitted(this, &SWebRTCSenderSettingsPanel::HandleTokenRefreshLeadTimeCommitted)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetch() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
			];
		}

	private:
		FString ResolveUrlValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(WebRTCConfig::UrlOptionKey) : FString();
		}

		void SetUrlValue(const FString& NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}

			const FString Sanitized = NewValue.TrimStartAndEnd();
			SenderComponent->SetTransportOption(WebRTCConfig::UrlOptionKey, Sanitized);
		}

		FString ResolveProfileValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(WebRTCConfig::GetCredentialProfileKey()) : FString();
		}

		bool IsTokenMissing() const
		{
			return !GetUseAutoTokenFetch() && SenderComponent
				&& SenderComponent->GetTransportSecretStatus(WebRTCConfig::TokenOptionKey).Source == EO3DSecretSource::None;
		}

		bool ResolveUseAutoTokenFetch() const
		{
			if (!SenderComponent)
			{
				return false;
			}
			const FString Value = SenderComponent->GetTransportOption(WebRTCConfig::UseAutoTokenFetchKey);
			return Value.ToBool();
		}

		void SetUseAutoTokenFetch(bool bValue)
		{
			if (!SenderComponent)
			{
				return;
			}
			SenderComponent->SetTransportOption(WebRTCConfig::UseAutoTokenFetchKey, bValue ? TEXT("true") : TEXT("false"));
		}

		bool GetUseAutoTokenFetch() const
		{
			return UseAutoTokenFetchCheckBox.IsValid() && UseAutoTokenFetchCheckBox->IsChecked();
		}

		FString ResolveTokenEndpointUrl() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(WebRTCConfig::TokenEndpointUrlKey) : FString();
		}

		void SetTokenEndpointUrl(const FString& NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}
			const FString Sanitized = NewValue.TrimStartAndEnd();
			SenderComponent->SetTransportOption(WebRTCConfig::TokenEndpointUrlKey, Sanitized);
		}

		FString ResolveRoomValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(WebRTCConfig::RoomOptionKey) : FString();
		}

		void SetRoomValue(const FString& NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}
			SenderComponent->SetTransportOption(WebRTCConfig::RoomOptionKey, NewValue.TrimStartAndEnd());
		}

		void HandleRoomCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetRoomValue(NewText.ToString());
			NotifyConfigChanged();
		}

		void HandleProfileCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			if (SenderComponent)
			{
				SenderComponent->SetTransportOption(WebRTCConfig::GetCredentialProfileKey(), NewText.ToString().TrimStartAndEnd());
			}
			NotifyConfigChanged();
		}

		void HandleSecretCommitted(ETextCommit::Type CommitType)
		{
			NotifyConfigChanged();
		}

		int32 ResolveTokenRefreshLeadTime() const
		{
			if (!SenderComponent)
			{
				return WebRTCConfig::DefaultTokenRefreshLeadTimeSec;
			}
			const FString Value = SenderComponent->GetTransportOption(WebRTCConfig::TokenRefreshLeadTimeKey);
			return Value.IsEmpty() ? WebRTCConfig::DefaultTokenRefreshLeadTimeSec : FCString::Atoi(*Value);
		}

		void SetTokenRefreshLeadTime(int32 Value)
		{
			if (!SenderComponent)
			{
				return;
			}
			SenderComponent->SetTransportOption(WebRTCConfig::TokenRefreshLeadTimeKey, FString::FromInt(Value));
		}

		void HandleUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetUrlValue(NewText.ToString());
			NotifyConfigChanged();
		}

		void HandleUseAutoTokenFetchChanged(ECheckBoxState NewState)
		{
			const bool bNewValue = (NewState == ECheckBoxState::Checked);
			SetUseAutoTokenFetch(bNewValue);
			NotifyConfigChanged();
		}

		void HandleTokenEndpointCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetTokenEndpointUrl(NewText.ToString());
			NotifyConfigChanged();
		}

		void HandleTokenRefreshLeadTimeCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			SetTokenRefreshLeadTime(NewValue);
			NotifyConfigChanged();
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
		TSharedPtr<SEditableTextBox> RoomTextBox;
		TSharedPtr<SEditableTextBox> UrlTextBox;
		TSharedPtr<SCheckBox> UseAutoTokenFetchCheckBox;
		TSharedPtr<SEditableTextBox> TokenEndpointTextBox;
		TSharedPtr<SSpinBox<int32>> TokenRefreshLeadTimeSpinBox;
	};
}

namespace WebRTCReceiver
{
	class SWebRTCReceiverSettingsPanel : public SO3DTransportConfigPanelBase
	{
	public:
		SLATE_BEGIN_ARGS(SWebRTCReceiverSettingsPanel)
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

			const FString InitialUrl = GetUrlValue();
			const bool bInitialUseAutoFetch = GetUseAutoTokenFetch();
			const FString InitialTokenEndpoint = GetTokenEndpointUrl();
			const int32 InitialRefreshLeadTime = GetTokenRefreshLeadTime();
			const FString InitialRoom = GetRoomValue();
			const FString InitialProfile = SettingsObject ? WebRTCConfig::GetReceiverOption(SettingsObject->Settings, *WebRTCConfig::GetCredentialProfileKey()) : FString();

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverUrlLabel", "LiveKit Host"))
					.ToolTipText(LOCTEXT("WebRTCReceiverUrlTooltip", "The WebSocket URL of your LiveKit server (e.g., wss://livekit.example.com)"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(UrlTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialUrl))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleUrlCommitted)
					.HintText(LOCTEXT("WebRTCReceiverUrlHint", "e.g., wss://livekit.example.com or ws://127.0.0.1:7880"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(UseAutoTokenFetchCheckBox, SCheckBox)
						.IsChecked(bInitialUseAutoFetch ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
						.OnCheckStateChanged(this, &SWebRTCReceiverSettingsPanel::HandleUseAutoTokenFetchChanged)
						.ToolTipText(LOCTEXT("WebRTCReceiverUseAutoTokenFetchCheckboxTooltip", "Automatically fetch JWT tokens from a token generator endpoint instead of manually entering them. Requires a token server that implements the LiveKit token generation API."))
					]
					+ SHorizontalBox::Slot()
					.Padding(8.f, 0.f, 0.f, 0.f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("WebRTCReceiverUseAutoTokenFetchLabel", "Use Auto Token Fetch"))
						.ToolTipText(LOCTEXT("WebRTCReceiverUseAutoTokenFetchTooltip", "Automatically fetch JWT tokens from a token generator endpoint instead of manually entering them. Requires a token server that implements the LiveKit token generation API."))
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverProfileLabel", "Credential Profile"))
					.ToolTipText(LOCTEXT("WebRTCReceiverProfileTooltip", "Names the stored credentials this source uses (saved with the source settings; the credentials are not). Leave empty for 'default'."))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SNew(SEditableTextBox)
					.Text(FText::FromString(InitialProfile))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleProfileCommitted)
					.HintText(LOCTEXT("WebRTCReceiverProfileHint", "default"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(WebRTCSecretsUI::SWebRTCSecretField, WebRTCSecretsUI::MakeReceiverBinding(SettingsObject, WebRTCConfig::TokenOptionKey))
						.Label(LOCTEXT("WebRTCReceiverTokenLabel", "Access Token"))
						.LabelToolTip(WebRTCSecretsUI::GetTokenLabelToolTip())
						.HintText(LOCTEXT("WebRTCReceiverTokenHint", "Paste a LiveKit access token"))
						.OnCommitted(WebRTCSecretsUI::FOnSecretCommitted::CreateSP(this, &SWebRTCReceiverSettingsPanel::HandleSecretCommitted))
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(STextBlock)
					.Text(WebRTCSecretsUI::GetMissingTokenWarning())
					.ColorAndOpacity(FLinearColor(1.f, 0.75f, 0.2f))
					.AutoWrapText(true)
					.Visibility_Lambda([this]() { return IsTokenMissing() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverTokenEndpointLabel", "Token Endpoint URL"))
					.ToolTipText(LOCTEXT("WebRTCReceiverTokenEndpointTooltip", "The HTTPS endpoint that issues LiveKit tokens (e.g., https://myserver.com/token). It receives room, identity and role, must authenticate the caller and decides the grants. Plain http:// is accepted only for localhost, 127.0.0.1 and ::1."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(TokenEndpointTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialTokenEndpoint))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleTokenEndpointCommitted)
					.HintText(LOCTEXT("WebRTCReceiverTokenEndpointHint", "https://myserver.com/token"))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(WebRTCSecretsUI::SWebRTCSecretField, WebRTCSecretsUI::MakeReceiverBinding(SettingsObject, WebRTCConfig::TokenEndpointAuthKey))
						.Label(LOCTEXT("WebRTCReceiverEndpointAuthLabel", "Token Endpoint Credential"))
						.LabelToolTip(WebRTCSecretsUI::GetEndpointAuthLabelToolTip())
						.HintText(LOCTEXT("WebRTCReceiverEndpointAuthHint", "Bearer credential for your token endpoint"))
						.OnCommitted(WebRTCSecretsUI::FOnSecretCommitted::CreateSP(this, &SWebRTCReceiverSettingsPanel::HandleSecretCommitted))
					]
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverRoomLabel", "Room"))
					.ToolTipText(LOCTEXT("WebRTCReceiverRoomTooltip", "LiveKit room requested from the token endpoint. Use the same room on the sender and the receiver. Required for Auto Token Fetch."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(RoomTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialRoom))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleRoomCommitted)
					.HintText(LOCTEXT("WebRTCReceiverRoomHint", "e.g., my-stage"))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverTokenRefreshLeadTimeLabel", "Token Refresh Lead Time (seconds)"))
					.ToolTipText(LOCTEXT("WebRTCReceiverTokenRefreshLeadTimeTooltip", "How many seconds before token expiry to trigger an automatic refresh. Default is 300 seconds (5 minutes)."))
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TokenRefreshLeadTimeSpinBox, SSpinBox<int32>)
					.Value(InitialRefreshLeadTime)
					.MinValue(WebRTCConfig::MinTokenRefreshLeadTimeSec)
					.MaxValue(WebRTCConfig::MaxTokenRefreshLeadTimeSec)
					.OnValueCommitted(this, &SWebRTCReceiverSettingsPanel::HandleTokenRefreshLeadTimeCommitted)
					.Visibility_Lambda([this]() { return GetUseAutoTokenFetchState() ? EVisibility::Visible : EVisibility::Collapsed; })
				];

			BuildPanel(PanelContent, InArgs._PanelWidthOverride);
		}

	private:
		FString GetUrlValue() const
		{
			if (!SettingsObject)
			{
				return FString();
			}

			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(WebRTCConfig::UrlOptionKey))
			{
				return *Existing;
			}

			return FString();
		}

		void SetUrlValue(const FString& Value)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::UrlOptionKey, Value.TrimStartAndEnd());
		}

		bool IsTokenMissing() const
		{
			if (GetUseAutoTokenFetchState() || !SettingsObject)
			{
				return false;
			}
			const FO3DSecretStatus Status = FO3DSecretStore::Get().Describe(WebRTCConfig::TransportName,
				O3DReceiver::GetCredentialProfile(SettingsObject->Settings), WebRTCConfig::TokenOptionKey, WebRTCUtils::TokenEnvVar);
			return Status.Source == EO3DSecretSource::None;
		}

		bool GetUseAutoTokenFetch() const
		{
			if (!SettingsObject)
			{
				return false;
			}
			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(WebRTCConfig::UseAutoTokenFetchKey))
			{
				return Existing->ToBool();
			}
			return false;
		}

		void SetUseAutoTokenFetch(bool bValue)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::UseAutoTokenFetchKey, bValue ? TEXT("true") : TEXT("false"));
		}

		bool GetUseAutoTokenFetchState() const
		{
			return UseAutoTokenFetchCheckBox.IsValid() && UseAutoTokenFetchCheckBox->IsChecked();
		}

		FString GetTokenEndpointUrl() const
		{
			if (!SettingsObject)
			{
				return FString();
			}
			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(WebRTCConfig::TokenEndpointUrlKey))
			{
				return *Existing;
			}
			return FString();
		}

		void SetTokenEndpointUrl(const FString& Value)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::TokenEndpointUrlKey, Value.TrimStartAndEnd());
		}

		FString GetRoomValue() const
		{
			return SettingsObject ? WebRTCConfig::GetReceiverOption(SettingsObject->Settings, WebRTCConfig::RoomOptionKey) : FString();
		}

		void SetRoomValue(const FString& Value)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::RoomOptionKey, Value.TrimStartAndEnd());
		}

		void HandleRoomCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetRoomValue(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		void HandleProfileCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, *WebRTCConfig::GetCredentialProfileKey(), NewText.ToString().TrimStartAndEnd());
			SubmitFromTextCommit(CommitType);
		}

		void HandleSecretCommitted(ETextCommit::Type CommitType)
		{
			SubmitFromTextCommit(CommitType);
		}

		int32 GetTokenRefreshLeadTime() const
		{
			if (!SettingsObject)
			{
				return WebRTCConfig::DefaultTokenRefreshLeadTimeSec;
			}
			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(WebRTCConfig::TokenRefreshLeadTimeKey))
			{
				return FCString::Atoi(**Existing);
			}
			return WebRTCConfig::DefaultTokenRefreshLeadTimeSec;
		}

		void SetTokenRefreshLeadTime(int32 Value)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::TokenRefreshLeadTimeKey, FString::FromInt(Value));
		}

		void HandleUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetUrlValue(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		void HandleUseAutoTokenFetchChanged(ECheckBoxState NewState)
		{
			const bool bNewValue = (NewState == ECheckBoxState::Checked);
			SetUseAutoTokenFetch(bNewValue);
		}

		void HandleTokenEndpointCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetTokenEndpointUrl(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		void HandleTokenRefreshLeadTimeCommitted(int32 NewValue, ETextCommit::Type CommitType)
		{
			SetTokenRefreshLeadTime(NewValue);
			SubmitFromTextCommit(CommitType);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> RoomTextBox;
		TSharedPtr<SEditableTextBox> UrlTextBox;
		TSharedPtr<SCheckBox> UseAutoTokenFetchCheckBox;
		TSharedPtr<SEditableTextBox> TokenEndpointTextBox;
		TSharedPtr<SSpinBox<int32>> TokenRefreshLeadTimeSpinBox;
	};
}
#endif // WITH_EDITOR

class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// livekit_ffi.dll is delay-loaded, so it must be loaded from the plugin before the first
		// call. Without it the transport is not registered at all: a factory would otherwise
		// hand out instances whose first FFI call fails on delay-load (TRF-14).
		Library = MakeShared<FO3DFfiLibrary, ESPMode::ThreadSafe>(MakeLiveKitLibraryDesc());
		if (!Library->Load())
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC transport not registered: %s"), *Library->GetStatusMessage());
			return;
		}

		// Every instance is tracked so ShutdownModule can stop it before unloading livekit_ffi.
		const TSharedRef<FO3DFfiLibrary, ESPMode::ThreadSafe> LibraryRef = Library.ToSharedRef();
		O3DTransport::RegisterSender(WebRTCConfig::TransportName, [LibraryRef]() -> TSharedPtr<IOpen3DSender>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCSender, ESPMode::ThreadSafe>());
		});
		O3DTransport::RegisterReceiver(WebRTCConfig::TransportName, [LibraryRef]() -> TSharedPtr<IOpen3DReceiver>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCReceiver, ESPMode::ThreadSafe>());
		});

		// Register WebRTC sender customization
		FO3DSenderTransportCustomization SenderCustomization;
		WebRTCConfig::DeclareSecrets(SenderCustomization.SecretOptionKeys, SenderCustomization.SecretEnvVars);
		SenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			Config.Transport = WebRTCConfig::TransportName;

			const FString UrlValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::UrlOptionKey);
			const FString UseAutoTokenFetchStr = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::UseAutoTokenFetchKey);
			const FString TokenEndpointUrlValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::TokenEndpointUrlKey);
			const FString TokenRefreshLeadTimeStr = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::TokenRefreshLeadTimeKey);
			const FString RoomValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::RoomOptionKey);

			Config.Uri = UrlValue;
			// Resolved from the secret store by the component (ADR 0004); never in AdvancedParams.
			Config.Token = WebRTCUtils::FindSecret(Config.Secrets, WebRTCConfig::TokenOptionKey);
			Config.Role = TEXT("publisher");

			// Configure auto-fetch fields
			Config.bUseAutoTokenFetch = UseAutoTokenFetchStr.ToBool();
			Config.TokenEndpointUrl = TokenEndpointUrlValue;
			Config.TokenRefreshLeadTimeSec = TokenRefreshLeadTimeStr.IsEmpty() ? WebRTCConfig::DefaultTokenRefreshLeadTimeSec : FCString::Atoi(*TokenRefreshLeadTimeStr);

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::RoomOptionKey, RoomValue.TrimStartAndEnd());
		};

#if WITH_EDITOR
		SenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			if (!SenderComponent)
			{
				return nullptr;
			}

			return SNew(WebRTCSender::SWebRTCSenderSettingsPanel)
				.SenderComponent(SenderComponent)
				.OnConfigChanged(OnConfigChanged);
		};
#endif // WITH_EDITOR
		O3DSender::RegisterTransportCustomization(WebRTCConfig::TransportName, MoveTemp(SenderCustomization));

		// Register WebRTC receiver customization
		FO3DReceiverTransportCustomization ReceiverCustomization;
		WebRTCConfig::DeclareSecrets(ReceiverCustomization.SecretOptionKeys, ReceiverCustomization.SecretEnvVars);
		ReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			Config.Transport = WebRTCConfig::TransportName;

			const FString UrlValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::UrlOptionKey);
			const FString UseAutoTokenFetchStr = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::UseAutoTokenFetchKey);
			const FString TokenEndpointUrlValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::TokenEndpointUrlKey);
			const FString TokenRefreshLeadTimeStr = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::TokenRefreshLeadTimeKey);
			const FString RoomValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::RoomOptionKey);

			Config.Uri = UrlValue;
			// Resolved from the secret store by the source (ADR 0004); never in AdvancedParams.
			Config.Token = WebRTCUtils::FindSecret(Config.Secrets, WebRTCConfig::TokenOptionKey);
			Config.StreamId = TEXT("WebRTCStream");
			Config.Role = TEXT("subscriber");

			// Configure auto-fetch fields
			Config.bUseAutoTokenFetch = UseAutoTokenFetchStr.ToBool();
			Config.TokenEndpointUrl = TokenEndpointUrlValue;
			Config.TokenRefreshLeadTimeSec = TokenRefreshLeadTimeStr.IsEmpty() ? WebRTCConfig::DefaultTokenRefreshLeadTimeSec : FCString::Atoi(*TokenRefreshLeadTimeStr);

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::RoomOptionKey, RoomValue.TrimStartAndEnd());

			Config.Audio.bEnableAudio = Settings.bEnableAudio;
			// Note: Audio stream label is now automatically derived from StreamId
		};

#if WITH_EDITOR
		ReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			if (!SettingsObject)
			{
				return nullptr;
			}

			return SNew(WebRTCReceiver::SWebRTCReceiverSettingsPanel)
				.SettingsObject(SettingsObject)
				.PanelWidthOverride(SO3DTransportConfigPanelBase::DefaultPanelWidth)
				.OnSubmit(OnSubmit);
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(WebRTCConfig::TransportName, MoveTemp(ReceiverCustomization));

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module started (LiveKit FFI backend)"));
	}

	virtual void ShutdownModule() override
	{
		// Unregister transport customizations
		O3DSender::UnregisterTransportCustomization(WebRTCConfig::TransportName);
		O3DReceiver::UnregisterTransportCustomization(WebRTCConfig::TransportName);

		// Unregister transport factories
		O3DTransport::UnregisterSender(WebRTCConfig::TransportName);
		O3DTransport::UnregisterReceiver(WebRTCConfig::TransportName);

		// TRF-14: stop instances that outlive the module (components, LiveLink sources), then
		// unload. FO3DFfiLibrary keeps the DLL loaded if an instance is still referenced.
		if (Library.IsValid())
		{
			Library->StopLiveInstances();
			Library->Unload();
		}

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module shut down"));
	}

private:
	/** livekit_ffi and the instances created from it. The registered factories hold a reference too. */
	TSharedPtr<FO3DFfiLibrary, ESPMode::ThreadSafe> Library;

	/**
	 * Location of livekit_ffi relative to the plugin that ships this module. WP-F11 moves the
	 * module to the Open3DBroadcastWebRTC add-on and changes OwningPluginName with it.
	 */
	static FO3DFfiLibraryDesc MakeLiveKitLibraryDesc()
	{
		FO3DFfiLibraryDesc Desc;
		Desc.DisplayName = TEXT("LiveKit FFI");
		Desc.OwningPluginName = TEXT("Open3DBroadcast");
#if PLATFORM_WINDOWS
		Desc.RelativePath = TEXT("Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.dll");
#else
#error "Unsupported platform for LiveKit FFI"
#endif
		return Desc;
	}
};

IMPLEMENT_MODULE(FOpen3DTransportWebRTCModule, Open3DTransportWebRTC)

#undef LOCTEXT_NAMESPACE
