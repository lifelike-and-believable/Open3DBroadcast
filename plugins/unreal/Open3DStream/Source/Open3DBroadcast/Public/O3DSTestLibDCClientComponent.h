#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include <memory>
#include <string>
#include <vector>

class IWebSocket; // forward declare UE WebSocket

namespace rtc { class PeerConnection; class DataChannel; class Track; struct Configuration; }

#include "O3DSTestLibDCClientComponent.generated.h"

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class UO3DSTestLibDCClientComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UO3DSTestLibDCClientComponent();

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString SignalingUrlBase = TEXT("ws://localhost:8080");

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString LocalId = TEXT("client");

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString RemoteId = TEXT("server");

	// If not empty, use this full URL verbatim.
	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString FullUrlOverride;

	// When true, append "/<LocalId>" to SignalingUrlBase; when false, use base as-is.
	UPROPERTY(EditAnywhere, Category="LibDC Test")
	bool bAppendLocalIdToUrl = true;

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	bool bSendAudio = true;

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	bool bVerbose = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TSharedPtr<IWebSocket> WS; // UE WebSocket
	std::shared_ptr<rtc::PeerConnection> PC;
	std::shared_ptr<rtc::DataChannel> DC;
	std::shared_ptr<rtc::Track> AudioTrack;

	std::atomic<bool> bAudioOpen{false};
	std::atomic<bool> bDCOpen{false};

	void OpenWebSocket();
	void SetupPeerConnection(const rtc::Configuration& Cfg);
	void SendJson(const TSharedPtr<FJsonObject>& Obj);
	void HandleIncomingJson(const FString& JsonStr);
	void CreateAudioTrackBeforeDC();
	void SendHelloAndTone();

	// Helpers
	static std::string ToStd(const FString& S) { FTCHARToUTF8 C(*S); return std::string(C.Get(), C.Length()); }
	static FString ToFStr(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }

	// Tone helpers
	static void GenerateSinePCM16(std::vector<uint8>& Out, double Freq, double DurationSec, int SR);
	void SendFakeOpusRtpTone(double DurationSec);
};