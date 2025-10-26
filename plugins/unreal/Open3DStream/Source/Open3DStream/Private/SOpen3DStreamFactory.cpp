#include "SOpen3DStreamFactory.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#include "Open3DStreamSource.h"
#include "o3ds/o3ds_version.h"  // for version

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "Open3DStream"

FText SOpen3DStreamFactory::LastUrl;
int SOpen3DStreamFactory::LastComboId = 0;
int SOpen3DStreamFactory::LastBackendComboId = 0;

void SOpen3DStreamFactory::Construct(const FArguments& Args)
{
	//LastTickTime = 0.0;
	OnSelectedEvent = Args._OnSelectedEvent;

	Options.Add(MakeShareable(new FString("NNG Subscribe (to NNG Publish)")));
	Options.Add(MakeShareable(new FString("NNG Client (to NNG Server)")));
	Options.Add(MakeShareable(new FString("NNG Server (to NNG Client)")));
	Options.Add(MakeShareable(new FString("TCP Client")));
	Options.Add(MakeShareable(new FString("UDP Server")));
	Options.Add(MakeShareable(new FString("WebRTC Client")));
	Options.Add(MakeShareable(new FString("WebRTC Server")));
	Options.Add(MakeShareable(new FString("Loopback (In-Editor Test)")));

	// WebRTC backend options
	BackendOptions.Add(MakeShareable(new FString("Peer-to-Peer (libdatachannel)")));
	BackendOptions.Add(MakeShareable(new FString("LiveKit SFU")));

	if (SOpen3DStreamFactory::LastUrl.IsEmpty())
	{
		LastUrl = LOCTEXT("Open3DStreamUrlValue", "tcp://meta.o3ds.net:9001");
	}

	CurrentProtocol = Options[LastComboId];
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
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamUrl", "Url")).MinDesiredWidth(150)
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
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamProtocol", "Protocol")).MinDesiredWidth(150)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SComboBox<FComboItemType>)
				.OptionsSource(&Options)
				.OnSelectionChanged(this, &SOpen3DStreamFactory::OnProtocolChanged)
				.OnGenerateWidget(this, &SOpen3DStreamFactory::MakeWidgetForOption)
				.InitiallySelectedItem(CurrentProtocol)
				[
					SNew(STextBlock)
					.Text(this, &SOpen3DStreamFactory::GetCurrentProtocol)
				]
			]
		]
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(WebRtcBackendRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetWebRtcBackendVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamBackend", "Backend")).MinDesiredWidth(150)
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
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(LiveKitServerUrlRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetLiveKitFieldsVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("LiveKitServerUrl", "LiveKit Server URL")).MinDesiredWidth(150)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetLiveKitServerUrl)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetLiveKitServerUrl)
			]
		]
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(LiveKitRoomRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetLiveKitFieldsVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("LiveKitRoom", "LiveKit Room")).MinDesiredWidth(150)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetLiveKitRoom)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetLiveKitRoom)
			]
		]
		+ SVerticalBox::Slot()
		.Padding(5)
		.AutoHeight()
		[
			SAssignNew(LiveKitTokenRow, SHorizontalBox)
			.Visibility(this, &SOpen3DStreamFactory::GetLiveKitFieldsVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("LiveKitToken", "LiveKit Token (JWT)")).MinDesiredWidth(150)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetLiveKitToken)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetLiveKitToken)
			]
		]
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			[
				SNew(STextBlock).Text(LOCTEXT("Open3DStreamKey", "Key")).MinDesiredWidth(150)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.7f)
			[	
				SNew(SEditableTextBox)
				.Text(this, &SOpen3DStreamFactory::GetKey)
				.OnTextChanged(this, &SOpen3DStreamFactory::SetKey)
				.OnTextCommitted(this, &SOpen3DStreamFactory::OnKeyCommitted)
			]
		]
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

FReply SOpen3DStreamFactory::OnSource()
{	
	FString s = mUrl.ToString();
	const TCHAR* url = s.operator*();
	TSharedPtr<FOpen3DStreamSettings, ESPMode::ThreadSafe> Settings = MakeShared<FOpen3DStreamSettings, ESPMode::ThreadSafe>();
	Settings->TimeOffset = 0;
	Settings->Url = mUrl;
	Settings->Protocol = GetCurrentProtocol();
	
	// Populate WebRTC backend and LiveKit configuration
	FString CurrentBackendStr = CurrentBackend.IsValid() ? *CurrentBackend : TEXT("");
	if (CurrentBackendStr.Contains(TEXT("LiveKit")))
	{
		Settings->WebRtcBackend = EO3DSWebRtcBackendReceiver::LiveKit;
		Settings->LiveKitServerUrl = mLiveKitServerUrl.ToString();
		Settings->LiveKitRoom = mLiveKitRoom.ToString();
		Settings->LiveKitToken = mLiveKitToken.ToString();
	}
	else
	{
		Settings->WebRtcBackend = EO3DSWebRtcBackendReceiver::LibDataChannel;
	}
	
	OnSelectedEvent.ExecuteIfBound(Settings);
	return FReply::Handled();
}

TSharedRef<SWidget> SOpen3DStreamFactory::MakeWidgetForOption(FComboItemType InOption)
{
	return SNew(STextBlock).Text(FText::FromString(*InOption));
}

void SOpen3DStreamFactory::OnProtocolChanged(FComboItemType NewValue, ESelectInfo::Type)
{
	CurrentProtocol = NewValue;
	for (int i = 0; i < Options.Num(); i++)
	{
		if (Options[i] == NewValue)
		{
			LastComboId = i; 
			break;
		}
	}
}

void SOpen3DStreamFactory::OnBackendChanged(FComboItemType NewValue, ESelectInfo::Type)
{
	CurrentBackend = NewValue;
	for (int i = 0; i < BackendOptions.Num(); i++)
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
	FString ProtocolStr = CurrentProtocol.IsValid() ? *CurrentProtocol : TEXT("");
	if (ProtocolStr.Contains(TEXT("WebRTC")))
	{
		return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

EVisibility SOpen3DStreamFactory::GetLiveKitFieldsVisibility() const
{
	FString ProtocolStr = CurrentProtocol.IsValid() ? *CurrentProtocol : TEXT("");
	FString BackendStr = CurrentBackend.IsValid() ? *CurrentBackend : TEXT("");
	
	if (ProtocolStr.Contains(TEXT("WebRTC")) && BackendStr.Contains(TEXT("LiveKit")))
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

FText SOpen3DStreamFactory::GetCurrentProtocol() const
{
	if (CurrentProtocol.IsValid())
	{
		return FText::FromString(*CurrentProtocol);
	}

	return LOCTEXT("InvalidComboEntryText", "<<Invalid option>>");
}