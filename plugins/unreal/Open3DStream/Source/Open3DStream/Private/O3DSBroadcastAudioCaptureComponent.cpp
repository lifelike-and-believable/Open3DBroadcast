#include "O3DSBroadcastAudioCaptureComponent.h"
#include "AudioDevice.h"
#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h"

static TSharedPtr<IWebRTCConnector> GetOrCreateConnector()
{
	static TSharedPtr<IWebRTCConnector> Cached;
	if (!Cached)
	{
		Cached = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel, nullptr);
	}
	return Cached;
}

UO3DSBroadcastAudioCaptureComponent::UO3DSBroadcastAudioCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSBroadcastAudioCaptureComponent::BeginPlay()
{
	Super::BeginPlay();
	StreamLabel = SubjectName.IsNone() ? FString(TEXT("o3ds:mix")) : FString::Printf(TEXT("o3ds:subject/%s"), *SubjectName.ToString());
	EnsureConnector();
	// Submix capture hookup to be added in a follow-up (requires AudioMixer APIs per platform)
}

void UO3DSBroadcastAudioCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void UO3DSBroadcastAudioCaptureComponent::EnsureConnector()
{
	if (!Connector)
	{
		Connector = GetOrCreateConnector();
		if (Connector)
		{
			IWebRTCConnector::FAudioSendConfig A;
			A.bEnable = true;
			A.SampleRate = Config.SampleRate;
			A.NumChannels = Config.NumChannels;
			A.BitrateKbps = Config.BitrateKbps;
			A.StreamLabel = StreamLabel;
			A.SubjectName = SubjectName.ToString();
			A.SourceType = SubjectName.IsNone() ? TEXT("mix") : TEXT("mic");
			Connector->EnableAudioSend(A);
		}
	}
}

void UO3DSBroadcastAudioCaptureComponent::PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	EnsureConnector();
	if (!Connector) return;
	Connector->PushPcm(StreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}
