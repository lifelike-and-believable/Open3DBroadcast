#pragma once
#include "Engine/Engine.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Open3DStreamSourceSettings.h"

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

	FOpen3DStreamSettings GetSourceData() const { return Result; }

	FOpen3DStreamSettings Result;

	FOnOpen3DStreamSelected OnSelectedEvent;

	// Combo option shared type
	typedef TSharedPtr<FString> FComboItemType;
	TSharedRef<SWidget> MakeWidgetForOption(FComboItemType InOption);

	// Transport Family Combo
	void OnFamilyChanged(FComboItemType NewValue, ESelectInfo::Type);
	FText GetCurrentFamily() const;

	// Transport Mode Combo (varies with family)
	void OnModeChanged(FComboItemType NewValue, ESelectInfo::Type);
	FText GetCurrentMode() const;
	EVisibility GetModeVisibility() const;

	// Backend Combo (WebRTC only)
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

	// Helpers
	void RebuildModeOptions();
	static bool LooksLikeHostPort(const FString& In);
	static FString NormalizeUrlForFamily(const FString& In, const FString& Family, const FString& Mode);
	static FString ComputeProtocolFor(const FString& Family, const FString& Mode);

	FText mUrl;
	FText mKey;
	FText mLiveKitServerUrl = FText::FromString(TEXT("wss://livekit.example.com"));
	FText mLiveKitRoom = FText::FromString(TEXT("room1"));
	FText mLiveKitToken;

	// Family + Mode combos
	TArray<FComboItemType> FamilyOptions;
	FComboItemType CurrentFamily;

	TArray<FComboItemType> ModeOptions;
	FComboItemType CurrentMode;

	// WebRTC backend
	TArray<FComboItemType> BackendOptions;
	FComboItemType CurrentBackend;

	// Slate widgets for conditional visibility
	TSharedPtr<SHorizontalBox> ModeRow;
	TSharedPtr<SComboBox<FComboItemType>> ModeCombo;
	TSharedPtr<SHorizontalBox> WebRtcBackendRow;
	TSharedPtr<SHorizontalBox> WebRtcRoomRow;
	TSharedPtr<SHorizontalBox> LiveKitServerUrlRow;
	TSharedPtr<SHorizontalBox> LiveKitRoomRow;
	TSharedPtr<SHorizontalBox> LiveKitTokenRow;

	static FText LastUrl;
	static int LastFamilyId;
	static int LastModeId;
	static int LastBackendComboId;

};