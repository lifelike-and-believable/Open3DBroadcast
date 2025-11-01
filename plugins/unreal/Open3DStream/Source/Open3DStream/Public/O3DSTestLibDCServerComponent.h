#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include <memory>

#include "O3DSTestLibDCServerComponent.generated.h"

class IWebSocket;
class FJsonObject;

namespace rtc {
	struct Configuration;
	class PeerConnection;
	class DataChannel;
	class Track;
}

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class UO3DSTestLibDCServerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UO3DSTestLibDCServerComponent();

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString SignalingUrlBase = TEXT("ws://localhost:8080");

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString LocalId = TEXT("server");

	// If not empty, use this full URL verbatim.
	UPROPERTY(EditAnywhere, Category="LibDC Test")
	FString FullUrlOverride;

	// When true, append "/<LocalId>" to SignalingUrlBase; when false, use base as-is.
	UPROPERTY(EditAnywhere, Category="LibDC Test")
	bool bAppendLocalIdToUrl = true;

	UPROPERTY(EditAnywhere, Category="LibDC Test")
	bool bVerbose = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// Signaling socket
	TSharedPtr<IWebSocket> WS;

	// Peer connection
	std::shared_ptr<rtc::PeerConnection> PC;

	// Remember who we should reply to (captured from first offer)
	FString RemotePeerId;

	// Optional: keep a strong ref to the incoming DataChannel for debugging
	std::shared_ptr<rtc::DataChannel> ServerDC;

	// Keep a strong reference to the pre-added/received RecvOnly audio track
	std::shared_ptr<rtc::Track> RecvAudioTrack;

	// Attach callbacks to a receive audio track (pre-added or received via onTrack)
	void AttachRecvAudioCallbacks(const std::shared_ptr<rtc::Track>& Track);

	// Internals
	void OpenWebSocket();
	void SetupPeerConnection(const rtc::Configuration& Cfg);
	void SendJson(const TSharedPtr<FJsonObject>& Obj);
	void HandleIncomingJson(const FString& JsonStr);
};