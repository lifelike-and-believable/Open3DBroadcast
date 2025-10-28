// Copyright (c) Open3DStream Contributors

#include "IWebRTCConnector.h"
#include "WebRTCConnector.h"
#include "HAL/PlatformTime.h"
#include "Open3DStreamSourceSettings.h" // ADD: defines EO3DSWebRtcBackendReceiver (LibDataChannel, LiveKit)

namespace
{
	class FLibDataChannelConnectorAdapter final : public IWebRTCConnector
	{
	public:
		virtual ~FLibDataChannelConnectorAdapter() override
		{
			if (Impl) { Impl->Stop(); Impl.Reset(); }
			if (ForwarderHandle.IsValid() && SourceWeak.IsValid())
			{
				SourceWeak.Pin()->OnRemoteAudio().Remove(ForwarderHandle);
			}
		}

		// Lifecycle
		bool Start(const FString& Url, bool bIsServer) override
		{
			if (!Impl.IsValid())
			{
				Impl = MakeShared<FWebRTCConnector>();
				FWebRTCConnector::SetActiveConnector(Impl);
			}
			const bool bOk = Impl->Start(Url, bIsServer);
			if (!bOk) { LastError = Impl->GetLastError(); return false; }

			// Forward remote audio
			SourceWeak = Impl;
			ForwarderHandle = Impl->OnRemoteAudio().AddLambda(
				[this](const FString& StreamLabel, const FString& Subject, const float* PCM, int32 NumFrames, int32 NumChannels, int32 SampleRate)
				{
					RemoteAudioDelegate.Broadcast(StreamLabel, Subject, PCM, NumFrames, NumChannels, SampleRate);
				});
			return true;
		}

		void Stop() override
		{
			if (Impl) { Impl->Stop(); }
		}

		bool IsConnected() const override { return Impl ? Impl->IsConnected() : false; }
		void Tick() override { if (Impl) Impl->Tick(); }

		// Data
		bool SendDataReliable(const uint8* Data, int32 Size) override { return Impl ? Impl->Send(Data, Size) : false; }
		bool SendDataLossy(const uint8* Data, int32 Size) override { return Impl ? Impl->Send(Data, Size) : false; }
		void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) override
		{
			if (Impl) Impl->SetDataReceivedCallback(MoveTemp(Callback));
		}

		// Audio
		bool EnableAudioSend(const FAudioSendConfig& A) override
		{
#if O3DS_WITH_OPUS
			if (!Impl) return false;
			FWebRTCConnector::FAudioConfig C;
			C.SampleRate  = A.SampleRate > 0 ? A.SampleRate : 48000;
			C.NumChannels = (A.NumChannels == 2) ? 2 : 1;
			C.BitrateKbps = A.BitrateKbps > 0 ? A.BitrateKbps : 32;
			C.FrameSizeMs = 20;
			C.StreamLabel = A.StreamLabel; // "o3ds:mix" or "o3ds:subject/<Name>"
			ConfiguredSR  = C.SampleRate;
			ConfiguredCH  = C.NumChannels;
			Impl->EnableAudioSend(C);
			return true;
#else
			LastError = TEXT("Opus disabled at build time (O3DS_WITH_OPUS==0)");
			return false;
#endif
		}

		bool PushPcm(const FString& /*StreamLabel*/, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double /*TimestampSec*/) override
		{
#if O3DS_WITH_OPUS
			if (!Impl || !Interleaved || NumFrames <= 0 || NumChannels <= 0) return false;
			if (ConfiguredSR > 0 && SampleRate != ConfiguredSR) return false;     // no resampler here
			if (ConfiguredCH > 0 && NumChannels != ConfiguredCH) return false;    // no channel mixer

			const int32 Count = NumFrames * NumChannels;
			TArray<int16> PCM16; PCM16.SetNumUninitialized(Count);
			for (int32 i = 0; i < Count; ++i)
			{
				const float f = FMath::Clamp(Interleaved[i], -1.0f, 1.0f);
				const int32 v = FMath::RoundToInt(f * 32767.0f);
				PCM16[i] = (int16)FMath::Clamp(v, -32768, 32767);
			}
			return Impl->PushAudioPCM16(PCM16.GetData(), Count);
#else
			return false;
#endif
		}

		FOnRemoteAudio& OnRemoteAudio() override { return RemoteAudioDelegate; }
		FString GetLastError() const override { return !LastError.IsEmpty() ? LastError : (Impl ? Impl->GetLastError() : FString()); }

	private:
		TSharedPtr<FWebRTCConnector> Impl;
		mutable FString LastError;

		// Forwarder
		TWeakPtr<FWebRTCConnector> SourceWeak;
		FDelegateHandle ForwarderHandle;
		IWebRTCConnector::FOnRemoteAudio RemoteAudioDelegate;

		// Expected capture format (must match encoder)
		int32 ConfiguredSR = 0;
		int32 ConfiguredCH = 0;
	};
}

// Factory (backend-agnostic entry)
TSharedPtr<IWebRTCConnector> CreateWebRTCConnector(EO3DSWebRtcBackendReceiver Backend, const FLiveKitConfig* /*LiveKitConfig*/)
{
	switch (Backend)
	{
		case EO3DSWebRtcBackendReceiver::LibDataChannel:
			return MakeShared<FLibDataChannelConnectorAdapter>();
		case EO3DSWebRtcBackendReceiver::LiveKit:
			// TODO: replace with LiveKit connector when implemented
			UE_LOG(LogTemp, Warning, TEXT("LiveKit backend not implemented; using libdatachannel connector."));
			return MakeShared<FLibDataChannelConnectorAdapter>();
		default:
			break;
	}
	return nullptr;
}
