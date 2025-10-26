#pragma once
#include "Engine/Engine.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Open3DStreamSourceSettings.h"

// E:\Unreal\UE_4.25\Engine\Plugins\Animation\LiveLink\Source\LiveLink\Private\SLiveLinkMessageBusSourceFactory.h

DECLARE_DELEGATE_OneParam(FOnOpen3DStreamSelected, FOpen3DStreamSettingsPtr);

class OPEN3DSTREAM_API SOpen3DStreamFactory : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SOpen3DStreamFactory) {}
		SLATE_EVENT(FOnOpen3DStreamSelected, OnSelectedEvent)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args);

	void SetUrl(const FText &InUrl)	{
		mUrl = InUrl; 
		SOpen3DStreamFactory::LastUrl = InUrl;
	}
	FText GetUrl() const {
		return mUrl;
	}

	void SetKey(const FText &InKey) { mKey = InKey; }
	FText GetKey() const { return mKey; }

	FOpen3DStreamSettings GetSourceData() const { return Result;  }

	FOpen3DStreamSettings Result;

	FOnOpen3DStreamSelected OnSelectedEvent;

	// Protocol Combo
	typedef TSharedPtr<FString> FComboItemType;
	TSharedRef<SWidget> MakeWidgetForOption(FComboItemType InOption);
	void OnProtocolChanged(FComboItemType NewValue, ESelectInfo::Type);
	FText GetCurrentProtocol() const;

	// Backend Combo
	void OnBackendChanged(FComboItemType NewValue, ESelectInfo::Type);
	FText GetCurrentBackend() const;

	// LiveKit configuration
	void SetLiveKitServerUrl(const FText& InText) { mLiveKitServerUrl = InText; }
	FText GetLiveKitServerUrl() const { return mLiveKitServerUrl; }
	
	void SetLiveKitRoom(const FText& InText) { mLiveKitRoom = InText; }
	FText GetLiveKitRoom() const { return mLiveKitRoom; }
	
	void SetLiveKitToken(const FText& InText) { mLiveKitToken = InText; }
	FText GetLiveKitToken() const { return mLiveKitToken; }

	// Visibility helpers
	EVisibility GetWebRtcBackendVisibility() const;
	EVisibility GetLiveKitFieldsVisibility() const;

	// Ensure we can handle Enter even when focus is not in a text box
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	FReply OnSource();
	// Commit handlers so Enter acts like Okay
	void OnUrlCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnKeyCommitted(const FText& NewText, ETextCommit::Type CommitType);

	FText mUrl;
	FText mKey;
	FText mLiveKitServerUrl = FText::FromString(TEXT("wss://livekit.example.com"));
	FText mLiveKitRoom = FText::FromString(TEXT("room1"));
	FText mLiveKitToken;

	TArray<FComboItemType> Options;
	FComboItemType CurrentProtocol;

	TArray<FComboItemType> BackendOptions;
	FComboItemType CurrentBackend;

	// Slate widgets for conditional visibility
	TSharedPtr<SHorizontalBox> WebRtcBackendRow;
	TSharedPtr<SHorizontalBox> LiveKitServerUrlRow;
	TSharedPtr<SHorizontalBox> LiveKitRoomRow;
	TSharedPtr<SHorizontalBox> LiveKitTokenRow;

	static FText LastUrl;
	static int LastComboId;
	static int LastBackendComboId;

};