#include "SOpen3DStreamFactory.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"

#include "Open3DStreamSource.h"
#include "o3ds/o3ds_version.h" // for version

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "Open3DStream"

FText SOpen3DStreamFactory::LastUrl;
int SOpen3DStreamFactory::LastFamilyId =0;
int SOpen3DStreamFactory::LastModeId =0;
int SOpen3DStreamFactory::LastBackendComboId =0;

void SOpen3DStreamFactory::Construct(const FArguments& Args)
{
	OnSelectedEvent = Args._OnSelectedEvent;

	// Transport Family options
	FamilyOptions.Add(MakeShareable(new FString(TEXT("TCP"))));
	FamilyOptions.Add(MakeShareable(new FString(TEXT("UDP"))));
	FamilyOptions.Add(MakeShareable(new FString(TEXT("NNG"))));
	FamilyOptions.Add(MakeShareable(new FString(TEXT("WebRTC"))));
	FamilyOptions.Add(MakeShareable(new FString(TEXT("Loopback (In-Editor Test)"))));

	// WebRTC backend options
	BackendOptions.Add(MakeShareable(new FString("Peer-to-Peer (libdatachannel)")));
	BackendOptions.Add(MakeShareable(new FString("LiveKit SFU")));

	if (SOpen3DStreamFactory::LastUrl.IsEmpty())
	{
		LastUrl = LOCTEXT("Open3DStreamUrlValue", "tcp://meta.o3ds.net:9001");
	}

	CurrentFamily = FamilyOptions.IsValidIndex(LastFamilyId) ? FamilyOptions[LastFamilyId] : FamilyOptions[0];
	RebuildModeOptions();
	CurrentMode = ModeOptions.IsValidIndex(LastModeId) ? ModeOptions[LastModeId] : (ModeOptions.Num() >0 ? ModeOptions[0] : nullptr);
	CurrentBackend = BackendOptions[LastBackendComboId];

	mUrl = LastUrl;
	const char* verstr = O3DS::getVersion();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamUrl", "Url")).MinDesiredWidth(175)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(mUrl)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetUrl)
				.OnTextCommitted(this, &SOpen3DStreamFactory::OnUrlCommitted)
			]
		]
		// Transport Family
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamFamily", "Transport Family")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[
				SNew(SComboBox<FComboItemType>)
				.OptionsSource(&FamilyOptions)
				.OnSelectionChanged(this, &SOpen3DStreamFactory::OnFamilyChanged)
				.OnGenerateWidget(this, &SOpen3DStreamFactory::MakeWidgetForOption)
				.InitiallySelectedItem(CurrentFamily)
				[
					SNew(STextBlock)
					.Text(this, &SOpen3DStreamFactory::GetCurrentFamily)
				]
			]
		]
		// Transport Mode (varies per family)
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(ModeRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetModeVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamMode", "Transport Mode")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[
				SAssignNew(ModeCombo, SComboBox<FComboItemType>)
				.OptionsSource(&ModeOptions)
				.OnSelectionChanged(this, &SOpen3DStreamFactory::OnModeChanged)
				.OnGenerateWidget(this, &SOpen3DStreamFactory::MakeWidgetForOption)
				.InitiallySelectedItem(CurrentMode)
				[
					SNew(STextBlock)
					.Text(this, &SOpen3DStreamFactory::GetCurrentMode)
				]
			]
		]
		// WebRTC Backend
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(WebRtcBackendRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetWebRtcBackendVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamBackend", "Backend")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SComboBox<FComboItemType>)
				.OptionsSource(&BackendOptions)
				.OnSelectionChanged(this, &SOpen3DStreamFactory::OnBackendChanged)
				.OnGenerateWidget(this, &SOpen3DStreamFactory::MakeWidgetForOption)
				.InitiallySelectedItem(CurrentBackend)
				[
					SNew(STextBlock)
					.Text(this, &SOpen3DStreamFactory::GetCurrentBackend)
				]
			]
		]
		// WebRTC Room (common)
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(WebRtcRoomRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetWebRtcBackendVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("WebRtcRoom", "Room")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetLiveKitRoom)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetLiveKitRoom)
			]
		]
		// LiveKit Token
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(LiveKitTokenRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetLiveKitFieldsVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("LiveKitToken", "LiveKit Token")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetLiveKitToken)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetLiveKitToken)
			]
		]
		// WebRTC Audio Enable
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(WebRtcAudioRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetWebRtcBackendVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("EnableWebRTCAudio", "Enable WebRTC Audio")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			.HAlign(HAlign_Left)
			[
				SNew(SCheckBox)
				.IsChecked(this, &SOpen3DStreamFactory::GetEnableWebRTCAudioState)
				.OnCheckStateChanged(this, &SOpen3DStreamFactory::OnEnableWebRTCAudioChanged)
			]
		]
		// Audio Playout Delay
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(AudioDelayRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetWebRtcBackendVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("AudioPlayoutDelay", "Audio Playout Delay (ms)")).MinDesiredWidth(200)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[
				SNew(SSpinBox<int32>)
				.MinValue(0)
				.MaxValue(500)
				.Value(this, &SOpen3DStreamFactory::GetAudioPlayoutDelay)
				.OnValueChanged(this, &SOpen3DStreamFactory::OnAudioPlayoutDelayChanged)
			]
		]
		// Key
		//+ SVerticalBox::Slot()
		//.Padding(5)
		//[
		//	SNew(SHorizontalBox)
		//	+ SHorizontalBox::Slot()
		//	.FillWidth(0.3f)
		//	[
		//		SNew(STextBlock).Text(LOCTEXT("Open3DStreamKey", "Key")).MinDesiredWidth(200)
		//	]
		//	+ SHorizontalBox::Slot()
		//	.FillWidth(0.7f)
		//	[	
		//		SNew(SEditableTextBox)
		//		.Text(this, &SOpen3DStreamFactory::GetKey)
		//		.OnTextChanged(this, &SOpen3DStreamFactory::SetKey)
		//		.OnTextCommitted(this, &SOpen3DStreamFactory::OnKeyCommitted)
		//	]
		//]
		// Buttons
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			+ SHorizontalBox::Slot()
			.FillWidth(0.4f)
			[
				SNew(SButton)
				.OnClicked(this, &SOpen3DStreamFactory::OnSource)
				.Content()
				[
					SNew(STextBlock)
					.MinDesiredWidth(100)
					.Justification(ETextJustify::Center)
					.Text(LOCTEXT("OkayButton", "Okay"))
				]
			]
 		+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
		]
		+ SVerticalBox::Slot()
			.Padding(5)
			[
				SNew(STextBlock).Text(FText::FromString(ANSI_TO_TCHAR(verstr)))
			]
	];
}

// Pressing Enter commits the text boxes and we treat it like clicking Okay
void SOpen3DStreamFactory::OnUrlCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	SetUrl(NewText);
	if (CommitType == ETextCommit::OnEnter)
	{
		OnSource();
	}
}

void SOpen3DStreamFactory::OnKeyCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	SetKey(NewText);
	if (CommitType == ETextCommit::OnEnter)
	{
		OnSource();
	}
}

FReply SOpen3DStreamFactory::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Enter || InKeyEvent.GetKey() == EKeys::Virtual_Accept)
	{
		return OnSource();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

static bool HasSchemePrefix(const FString& In)
{
	return In.StartsWith(TEXT("tcp://"), ESearchCase::IgnoreCase)
		|| In.StartsWith(TEXT("udp://"), ESearchCase::IgnoreCase)
		|| In.StartsWith(TEXT("nng://"), ESearchCase::IgnoreCase)
		|| In.StartsWith(TEXT("webrtc://"), ESearchCase::IgnoreCase);
}

bool SOpen3DStreamFactory::LooksLikeHostPort(const FString& In)
{
	for (TCHAR C : In)
	{
		if ((C >= '0' && C <= '9') || C == '.' || C == ':' ) { continue; }
		return false;
	}
	return !In.IsEmpty();
}

FString SOpen3DStreamFactory::NormalizeUrlForFamily(const FString& In, const FString& Family, const FString& Mode)
{
	FString Out = In;
	if (!HasSchemePrefix(Out))
	{
		if (Family.Equals(TEXT("TCP"), ESearchCase::IgnoreCase))
		{
			if (LooksLikeHostPort(Out)) { Out = FString::Printf(TEXT("tcp://%s"), *Out); }
		}
		else if (Family.Equals(TEXT("UDP"), ESearchCase::IgnoreCase))
		{
			if (LooksLikeHostPort(Out)) { Out = FString::Printf(TEXT("udp://%s"), *Out); }
		}
		else if (Family.Equals(TEXT("NNG"), ESearchCase::IgnoreCase))
		{
			// Default NNG transport uses TCP addresses
			if (LooksLikeHostPort(Out)) { Out = FString::Printf(TEXT("tcp://%s"), *Out); }
		}
		else if (Family.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
		{
			if (LooksLikeHostPort(Out)) { Out = FString::Printf(TEXT("webrtc://%s"), *Out); }
		}
	}

	// Inject WebRTC role param if missing
	if (Family.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
	{
		if (!Out.Contains(TEXT("role="), ESearchCase::IgnoreCase))
		{
			const TCHAR* RoleStr = (Mode.Contains(TEXT("Server"))) ? TEXT("server") : TEXT("client");
			Out += Out.Contains(TEXT("?")) ? FString::Printf(TEXT("&role=%s"), RoleStr)
					 : FString::Printf(TEXT("?role=%s"), RoleStr);
		}
	}
	return Out;
}

FString SOpen3DStreamFactory::ComputeProtocolFor(const FString& Family, const FString& Mode)
{
	if (Family.Equals(TEXT("Loopback (In-Editor Test)"), ESearchCase::IgnoreCase))
	{
		return TEXT("Loopback (In-Editor Test)");
	}
	if (Family.Equals(TEXT("TCP"), ESearchCase::IgnoreCase))
	{
		// Receiver supports TCP client only
		return TEXT("TCP Client");
	}
	if (Family.Equals(TEXT("UDP"), ESearchCase::IgnoreCase))
	{
		return TEXT("UDP Server");
	}
	if (Family.Equals(TEXT("NNG"), ESearchCase::IgnoreCase))
	{
		if (Mode.Contains(TEXT("Subscribe"))) return TEXT("NNG Subscribe");
		if (Mode.Contains(TEXT("Client"))) return TEXT("NNG Client");
		if (Mode.Contains(TEXT("Server"))) return TEXT("NNG Server");
	}
	if (Family.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
	{
		return Mode.Contains(TEXT("Server")) ? TEXT("WebRTC Server") : TEXT("WebRTC Client");
	}
	return TEXT("TCP Client");
}

FReply SOpen3DStreamFactory::OnSource()
{	
	TSharedPtr<FOpen3DStreamSettings, ESPMode::ThreadSafe> Settings = MakeShared<FOpen3DStreamSettings, ESPMode::ThreadSafe>();
	Settings->TimeOffset =0;

	const FString FamilyStr = CurrentFamily.IsValid() ? *CurrentFamily : TEXT("TCP");
	const FString ModeStr = CurrentMode.IsValid() ? *CurrentMode : TEXT("");
	const FString InputUrl = mUrl.ToString();
	const FString Room = mLiveKitRoom.ToString();

	FString EffectiveUrl = NormalizeUrlForFamily(InputUrl, FamilyStr, ModeStr);
	const FString ProtocolName = ComputeProtocolFor(FamilyStr, ModeStr);

	// For WebRTC: do NOT append room to URL for LibDataChannel backend in server mode.
	// LibDataChannelConnector will handle path construction from Config.Room.
	// For LiveKit or other backends, query params may be needed.
	// For now, keep URL clean and pass Room separately via Settings->WebRtcRoom.
	if (FamilyStr.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
	{
		if (EffectiveUrl.EndsWith(TEXT("/"))) { EffectiveUrl.LeftChopInline(1); }
		// Room will be passed separately in Settings->WebRtcRoom
		// LibDataChannelConnector handles URL construction based on Role
	}

	Settings->Url = FText::FromString(EffectiveUrl);
	Settings->Protocol = FText::FromString(ProtocolName);

	// Populate WebRTC backend and LiveLink configuration
	if (FamilyStr.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
	{
		Settings->WebRtcRoom = Room;
		FString CurrentBackendStr = CurrentBackend.IsValid() ? *CurrentBackend : TEXT("");
		if (CurrentBackendStr.Contains(TEXT("LiveKit")))
		{
			Settings->WebRtcBackend = EO3DSWebRtcBackendReceiver::LiveKit;
			Settings->LiveKitToken = mLiveKitToken.ToString();
		}
		else
		{
			Settings->WebRtcBackend = EO3DSWebRtcBackendReceiver::LibDataChannel;
		}
	}
	else
	{
		Settings->WebRtcBackend = EO3DSWebRtcBackendReceiver::LibDataChannel;
	}

	// Populate audio settings
	Settings->bEnableWebRTCAudio = bEnableWebRTCAudio;
	Settings->WebRTCAudioPlayoutDelayMs = WebRTCAudioPlayoutDelayMs;
	
	OnSelectedEvent.ExecuteIfBound(Settings);
	return FReply::Handled();
}

TSharedRef<SWidget> SOpen3DStreamFactory::MakeWidgetForOption(FComboItemType InOption)
{
	return SNew(STextBlock).Text(FText::FromString(*InOption));
}

void SOpen3DStreamFactory::OnFamilyChanged(FComboItemType NewValue, ESelectInfo::Type)
{
	CurrentFamily = NewValue;
	for (int i =0; i < FamilyOptions.Num(); i++)
	{
		if (FamilyOptions[i] == NewValue)
		{
			LastFamilyId = i;
			break;
		}
	}
	RebuildModeOptions();
	// Reset mode selection when family changes
	CurrentMode = ModeOptions.Num() >0 ? ModeOptions[0] : nullptr;
	if (ModeCombo.IsValid())
	{
		ModeCombo->SetSelectedItem(CurrentMode);
	}
	LastModeId =0;
}

void SOpen3DStreamFactory::OnModeChanged(FComboItemType NewValue, ESelectInfo::Type)
{
	CurrentMode = NewValue;
	for (int i =0; i < ModeOptions.Num(); i++)
	{
		if (ModeOptions[i] == NewValue)
		{
			LastModeId = i;
			break;
		}
	}
}

FText SOpen3DStreamFactory::GetCurrentFamily() const
{
	if (CurrentFamily.IsValid())
	{
		return FText::FromString(*CurrentFamily);
	}
	return LOCTEXT("InvalidComboEntryText", "<<Invalid option>>");
}

FText SOpen3DStreamFactory::GetCurrentMode() const
{
	if (CurrentMode.IsValid())
	{
		return FText::FromString(*CurrentMode);
	}
	// Fallback: show the first available option as the default until a choice is made
	if (ModeOptions.Num() >0 && ModeOptions[0].IsValid())
	{
		return FText::FromString(*ModeOptions[0]);
	}
	return LOCTEXT("InvalidComboEntryText", "<<Invalid option>>");
}

EVisibility SOpen3DStreamFactory::GetModeVisibility() const
{
	// Hide when no modes are available (e.g., Loopback)
	return (ModeOptions.Num() >0) ? EVisibility::Visible : EVisibility::Collapsed;
}

void SOpen3DStreamFactory::OnBackendChanged(FComboItemType NewValue, ESelectInfo::Type)
{
	CurrentBackend = NewValue;
	for (int i =0; i < BackendOptions.Num(); i++)
	{
		if (BackendOptions[i] == NewValue)
		{
			LastBackendComboId = i;
			break;
		}
	}
}

EVisibility SOpen3DStreamFactory::GetWebRtcBackendVisibility() const
{
	const FString Fam = CurrentFamily.IsValid() ? *CurrentFamily : TEXT("");
	if (Fam.Contains(TEXT("WebRTC")))
	{
		return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

EVisibility SOpen3DStreamFactory::GetLiveKitFieldsVisibility() const
{
	const FString Fam = CurrentFamily.IsValid() ? *CurrentFamily : TEXT("");
	const FString BackendStr = CurrentBackend.IsValid() ? *CurrentBackend : TEXT("");
	if (Fam.Contains(TEXT("WebRTC")) && BackendStr.Contains(TEXT("LiveKit")))
	{
		return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

FText SOpen3DStreamFactory::GetCurrentBackend() const
{
	if (CurrentBackend.IsValid())
	{
		return FText::FromString(*CurrentBackend);
	}

	return LOCTEXT("InvalidComboEntryText", "<<Invalid option>>");
}

void SOpen3DStreamFactory::RebuildModeOptions()
{
	ModeOptions.Reset();
	const FString Fam = CurrentFamily.IsValid() ? *CurrentFamily : TEXT("TCP");
	if (Fam.Equals(TEXT("TCP"), ESearchCase::IgnoreCase))
	{
		// Receiver supports only client mode for TCP
		ModeOptions.Add(MakeShareable(new FString(TEXT("Client"))));
	}
	else if (Fam.Equals(TEXT("UDP"), ESearchCase::IgnoreCase))
	{
		ModeOptions.Add(MakeShareable(new FString(TEXT("Server"))));
	}
	else if (Fam.Equals(TEXT("NNG"), ESearchCase::IgnoreCase))
	{
		ModeOptions.Add(MakeShareable(new FString(TEXT("Subscribe (to Publish)"))));
		ModeOptions.Add(MakeShareable(new FString(TEXT("Client (to Pair Server)"))));
		ModeOptions.Add(MakeShareable(new FString(TEXT("Server (to Pair Client)"))));
	}
	else if (Fam.Equals(TEXT("WebRTC"), ESearchCase::IgnoreCase))
	{
		ModeOptions.Add(MakeShareable(new FString(TEXT("Client"))));
		ModeOptions.Add(MakeShareable(new FString(TEXT("Server"))));
	}
	else if (Fam.Contains(TEXT("Loopback")))
	{
		// No mode for loopback
	}

	// Ensure CurrentMode is always set to a valid default option after rebuild
	if (ModeOptions.Num() >0)
	{
		if (!CurrentMode.IsValid() || !ModeOptions.Contains(CurrentMode))
		{
			CurrentMode = ModeOptions[0];
		}
		if (ModeCombo.IsValid())
		{
			ModeCombo->RefreshOptions();
			ModeCombo->SetSelectedItem(CurrentMode);
		}
	}
}

#undef LOCTEXT_NAMESPACE