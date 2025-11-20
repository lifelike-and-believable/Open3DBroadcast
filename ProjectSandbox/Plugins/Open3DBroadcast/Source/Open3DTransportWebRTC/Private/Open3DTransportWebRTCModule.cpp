#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Sender/WebRTCSender.h"
#include "Receiver/WebRTCReceiver.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTransportConfigPanelBase.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"

#if WITH_EDITOR
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY(LogO3DWebRTCSender);
DEFINE_LOG_CATEGORY(LogO3DWebRTCReceiver);

#define LOCTEXT_NAMESPACE "Open3DTransportWebRTC"

namespace WebRTCConfig
{
	static constexpr TCHAR UrlOptionKey[] = TEXT("webrtc.url");
	static constexpr TCHAR TokenOptionKey[] = TEXT("webrtc.token");

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
			const FString InitialToken = ResolveTokenValue();

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCUrlLabel", "LiveKit Host"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(UrlTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialUrl))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleUrlCommitted)
					.HintText(LOCTEXT("WebRTCUrlHint", "e.g., livkit.example.com or 127.0.0.1"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCTokenLabel", "Access Token"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TokenTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialToken))
					.OnTextCommitted(this, &SWebRTCSenderSettingsPanel::HandleTokenCommitted)
					.HintText(LOCTEXT("WebRTCTokenHint", "LiveKit access token"))
					.IsPassword(true)
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

		FString ResolveTokenValue() const
		{
			return SenderComponent ? SenderComponent->GetTransportOption(WebRTCConfig::TokenOptionKey) : FString();
		}

		void SetTokenValue(const FString& NewValue)
		{
			if (!SenderComponent)
			{
				return;
			}

			const FString Sanitized = NewValue.TrimStartAndEnd();
			SenderComponent->SetTransportOption(WebRTCConfig::TokenOptionKey, Sanitized);
		}

		void HandleUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetUrlValue(NewText.ToString());
			NotifyConfigChanged();
		}

		void HandleTokenCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetTokenValue(NewText.ToString());
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
		TSharedPtr<SEditableTextBox> UrlTextBox;
		TSharedPtr<SEditableTextBox> TokenTextBox;
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
			const FString InitialToken = GetTokenValue();

			TSharedRef<SVerticalBox> PanelContent = SNew(SVerticalBox);

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverUrlLabel", "LiveKit Host"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 8.f)
				[
					SAssignNew(UrlTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialUrl))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleUrlCommitted)
					.HintText(LOCTEXT("WebRTCReceiverUrlHint", "e.g., livkit.example.com or 127.0.0.1"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WebRTCReceiverTokenLabel", "Access Token"))
				];

			PanelContent->AddSlot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SAssignNew(TokenTextBox, SEditableTextBox)
					.Text(FText::FromString(InitialToken))
					.OnTextCommitted(this, &SWebRTCReceiverSettingsPanel::HandleTokenCommitted)
					.HintText(LOCTEXT("WebRTCReceiverTokenHint", "LiveKit access token"))
					.IsPassword(true)
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

		FString GetTokenValue() const
		{
			if (!SettingsObject)
			{
				return FString();
			}

			if (const FString* Existing = SettingsObject->Settings.TransportOptions.Find(WebRTCConfig::TokenOptionKey))
			{
				return *Existing;
			}

			return FString();
		}

		void SetTokenValue(const FString& Value)
		{
			WebRTCConfig::SetReceiverOption(SettingsObject, WebRTCConfig::TokenOptionKey, Value.TrimStartAndEnd());
		}

		void HandleUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetUrlValue(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		void HandleTokenCommitted(const FText& NewText, ETextCommit::Type CommitType)
		{
			SetTokenValue(NewText.ToString());
			SubmitFromTextCommit(CommitType);
		}

		UO3DReceiverSettingsObject* SettingsObject = nullptr;
		TSharedPtr<SEditableTextBox> UrlTextBox;
		TSharedPtr<SEditableTextBox> TokenTextBox;
	};
}
#endif // WITH_EDITOR

class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Load the LiveKit FFI DLL from the plugin's ThirdParty directory
		LoadLiveKitFFI();

		// Register WebRTC sender factory
		O3DTransport::RegisterSender(TEXT("WebRTC"), []() { return MakeShared<FO3DWebRTCSender>(); });

		// Register WebRTC receiver factory
		O3DTransport::RegisterReceiver(TEXT("WebRTC"), []() { return MakeShared<FO3DWebRTCReceiver>(); });

		// Register WebRTC sender customization
		FO3DSenderTransportCustomization SenderCustomization;
		SenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("WebRTC");

			const FString UrlValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::UrlOptionKey);
			const FString TokenValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::TokenOptionKey);

			Config.Uri = UrlValue;
			Config.Token = TokenValue;
			Config.Role = TEXT("publisher");

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::TokenOptionKey, TokenValue);
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
		O3DSender::RegisterTransportCustomization(TEXT("WebRTC"), MoveTemp(SenderCustomization));

		// Register WebRTC receiver customization
		FO3DReceiverTransportCustomization ReceiverCustomization;
		ReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			Config.Transport = TEXT("WebRTC");

			const FString UrlValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::UrlOptionKey);
			const FString TokenValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::TokenOptionKey);

			Config.Uri = UrlValue;
			Config.Token = TokenValue;
			Config.StreamId = TEXT("WebRTCStream");
			Config.Role = TEXT("subscriber");

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::TokenOptionKey, TokenValue);

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
		O3DReceiver::RegisterTransportCustomization(TEXT("WebRTC"), MoveTemp(ReceiverCustomization));

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module started (LiveKit FFI backend)"));
	}

	virtual void ShutdownModule() override
	{
		// Unregister transport customizations
		O3DSender::UnregisterTransportCustomization(TEXT("WebRTC"));
		O3DReceiver::UnregisterTransportCustomization(TEXT("WebRTC"));

		// Unregister transport factories
		O3DTransport::UnregisterSender(TEXT("WebRTC"));
		O3DTransport::UnregisterReceiver(TEXT("WebRTC"));

		// Unload the LiveKit FFI DLL
		UnloadLiveKitFFI();

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module shut down"));
	}

private:
	void* LiveKitFFIHandle = nullptr;

	void LoadLiveKitFFI()
	{
		// Get the plugin base directory
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Open3DBroadcast"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to find Open3DBroadcast plugin"));
			return;
		}

		// Construct path to the DLL in ThirdParty/livekit_ffi/bin/Win64/
		FString DLLPath = FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Source"),
			TEXT("Open3DTransportWebRTC"),
			TEXT("ThirdParty"),
			TEXT("livekit_ffi"),
			TEXT("bin"),
#if PLATFORM_WINDOWS
			TEXT("Win64")
#else
#error "Unsupported platform for LiveKit FFI"
#endif
		);

		DLLPath = FPaths::Combine(DLLPath, TEXT("livekit_ffi.dll"));

		if (!FPaths::FileExists(DLLPath))
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("LiveKit FFI DLL not found at: %s"), *DLLPath);
			return;
		}

		// Load the DLL
		LiveKitFFIHandle = FPlatformProcess::GetDllHandle(*DLLPath);
		if (LiveKitFFIHandle == nullptr)
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to load LiveKit FFI DLL from: %s"), *DLLPath);
			return;
		}

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Successfully loaded LiveKit FFI DLL from: %s"), *DLLPath);
	}

	void UnloadLiveKitFFI()
	{
		if (LiveKitFFIHandle != nullptr)
		{
			FPlatformProcess::FreeDllHandle(LiveKitFFIHandle);
			LiveKitFFIHandle = nullptr;
			UE_LOG(LogO3DWebRTCSender, Log, TEXT("LiveKit FFI DLL unloaded"));
		}
	}
};

IMPLEMENT_MODULE(FOpen3DTransportWebRTCModule, Open3DTransportWebRTC)

#undef LOCTEXT_NAMESPACE
