// Copyright (c) Open3DStream Contributors

#include "IWebRTCConnector.h"
#include "WebRTCConnector.h"
#include "Open3DStreamSourceSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	class FLibDataChannelAdapter : public IWebRTCConnector
	{
	public:
		FLibDataChannelAdapter()
		{
			Inner = MakeShared<FWebRTCConnector>();
			FWebRTCConnector::SetActiveConnector(Inner);
			BindFromInner();
		}

		virtual bool Start(const FString& Url, bool bIsServer) override { return Inner->Start(Url, bIsServer); }
		virtual void Stop() override { Inner->Stop(); }
		virtual bool IsConnected() const override { return Inner->IsConnected(); }
		virtual void Tick() override { Inner->Tick(); }

		virtual bool SendDataReliable(const uint8* Data, int32 Size) override { return Inner->Send(Data, Size); }
		virtual bool SendDataLossy(const uint8* Data, int32 Size) override { return Inner->Send(Data, Size); }
		virtual void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) override { Inner->SetDataReceivedCallback(MoveTemp(Callback)); }

		virtual bool EnableAudioSend(const FAudioSendConfig& Cfg) override 
		{
#if O3DS_WITH_OPUS && !O3DS_OPUS_NO_HEADER
			FWebRTCConnector::FAudioConfig A; A.SampleRate = Cfg.SampleRate; A.NumChannels = Cfg.NumChannels; A.BitrateKbps = Cfg.BitrateKbps; A.FrameSizeMs =20;
			Inner->EnableAudioSend(A);
			EmitAnnounceIfNeeded(Cfg);
			return true;
#else
			return false;
#endif
		}
		virtual bool PushPcm(const FString& /*StreamLabel*/, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double /*TimestampSec*/) override 
		{
#if O3DS_WITH_OPUS && !O3DS_OPUS_NO_HEADER
			// Convert float [-1,1] to int16
			TempPcm16.Reset(NumFrames * NumChannels);
			TempPcm16.AddUninitialized(NumFrames * NumChannels);
			for (int32 i=0;i<NumFrames * NumChannels;++i)
			{
				float v = FMath::Clamp(Interleaved[i], -1.0f,1.0f);
				TempPcm16[i] = (int16)FMath::RoundToInt(v *32767.0f);
			}
			return Inner->PushAudioPCM16(TempPcm16.GetData(), TempPcm16.Num());
#else
			return false;
#endif
		}

		virtual FOnRemoteAudio& OnRemoteAudio() override { return RemoteAudio; }
		virtual FString GetLastError() const override { return Inner->GetLastError(); }

		void BindFromInner()
		{
			Inner->OnRemoteAudio().AddLambda([this](const FString& Subject, const FString& Stream, const int16* PCM, int32 NumSamples, int32 NumChannels)
			{
				// Convert to float and broadcast
				TempPcmFloat.Reset(NumSamples);
				TempPcmFloat.AddUninitialized(NumSamples);
				for (int32 i=0;i<NumSamples;++i)
				{
					TempPcmFloat[i] = (float)PCM[i] /32768.0f;
				}
				RemoteAudio.Broadcast(Stream, Subject, TempPcmFloat.GetData(), NumSamples / NumChannels, NumChannels,48000);
			});
		}

	private:
		void EmitAnnounceIfNeeded(const FAudioSendConfig& Cfg)
		{
			if (AnnouncedStreams.Contains(Cfg.StreamLabel))
			{
				return;
			}
			AnnouncedStreams.Add(Cfg.StreamLabel);

			// Build simple announce JSON per docs
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("o3ds.audio.announce"));
			TArray<TSharedPtr<FJsonValue>> Tracks;
			TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("stream"), Cfg.StreamLabel);
			T->SetStringField(TEXT("track"), TEXT(""));
			T->SetStringField(TEXT("subject"), Cfg.SubjectName);
			T->SetStringField(TEXT("source"), Cfg.SourceType);
			T->SetNumberField(TEXT("sr"), Cfg.SampleRate);
			T->SetNumberField(TEXT("ch"), Cfg.NumChannels);
			T->SetNumberField(TEXT("br"), Cfg.BitrateKbps);
			Tracks.Add(MakeShared<FJsonValueObject>(T));
			Root->SetArrayField(TEXT("tracks"), Tracks);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Root, Writer);

			FTCHARToUTF8 Conv(*OutStr);
			SendDataReliable(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
		}

	private:
		TSharedPtr<FWebRTCConnector> Inner;
		IWebRTCConnector::FOnRemoteAudio RemoteAudio;
		TArray<int16> TempPcm16;
		TArray<float> TempPcmFloat;
		TSet<FString> AnnouncedStreams;
	};
}

TSharedPtr<IWebRTCConnector> CreateWebRTCConnector(EO3DSWebRtcBackendReceiver Backend, const FLiveKitConfig* /*LiveKitConfig*/)
{
	if (Backend == EO3DSWebRtcBackendReceiver::LibDataChannel)
	{
		return MakeShared<FLibDataChannelAdapter>();
	}
	// LiveKit stub to follow in later PR
	return nullptr;
}
