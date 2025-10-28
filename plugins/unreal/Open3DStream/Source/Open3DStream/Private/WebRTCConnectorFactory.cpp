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

			// Chain a data callback to parse announce before user callback
			Inner->SetDataReceivedCallback([this](const uint8* Data, int32 Size)
			{
				HandleIncomingData(Data, Size);
				// Forward to external if bound
				if (UserDataCallback)
				{
					UserDataCallback(Data, Size);
				}
			});
		}

		virtual bool Start(const FString& Url, bool bIsServer) override { return Inner->Start(Url, bIsServer); }
		virtual void Stop() override { Inner->Stop(); }
		virtual bool IsConnected() const override { return Inner->IsConnected(); }
		virtual void Tick() override { Inner->Tick(); }

		virtual bool SendDataReliable(const uint8* Data, int32 Size) override { return Inner->Send(Data, Size); }
		virtual bool SendDataLossy(const uint8* Data, int32 Size) override { return Inner->Send(Data, Size); }
		virtual void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) override { UserDataCallback = MoveTemp(Callback); }

		virtual bool EnableAudioSend(const FAudioSendConfig& Cfg) override 
		{
#if O3DS_WITH_OPUS && !O3DS_OPUS_NO_HEADER
			// Avoid repeated reconfiguration if the same stream label was already enabled
			if (!EnabledStreams.Contains(Cfg.StreamLabel))
			{
				FWebRTCConnector::FAudioConfig A; A.SampleRate = Cfg.SampleRate; A.NumChannels = Cfg.NumChannels; A.BitrateKbps = Cfg.BitrateKbps; A.FrameSizeMs =20; A.StreamLabel = Cfg.StreamLabel;
				UE_LOG(LogTemp, Verbose, TEXT("FLibDataChannelAdapter: EnableAudioSend stream=%s sr=%d ch=%d br=%d"), *A.StreamLabel, A.SampleRate, A.NumChannels, A.BitrateKbps);
				Inner->EnableAudioSend(A);
				EnabledStreams.Add(Cfg.StreamLabel);
				EmitAnnounceIfNeeded(Cfg);
				UE_LOG(LogTemp, Verbose, TEXT("FLibDataChannelAdapter: Announce emitted for stream=%s"), *Cfg.StreamLabel);
			}
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
			UE_LOG(LogTemp, Verbose, TEXT("FLibDataChannelAdapter: PushPcm frames=%d ch=%d sr=%d samples=%d"), NumFrames, NumChannels, SampleRate, TempPcm16.Num());
			const bool bOk = Inner->PushAudioPCM16(TempPcm16.GetData(), TempPcm16.Num());
			if (!bOk)
			{
				const double Now = FPlatformTime::Seconds();
				static double sLastWarn = 0.0;
				if (Now - sLastWarn > 0.5)
				{
					UE_LOG(LogTemp, Warning, TEXT("FLibDataChannelAdapter: PushAudioPCM16 failed"));
					sLastWarn = Now;
				}
			}
			return bOk;
#else
			return false;
#endif
		}

		virtual FOnRemoteAudio& OnRemoteAudio() override { return RemoteAudio; }
		virtual FString GetLastError() const override { return Inner->GetLastError(); }

		void BindFromInner()
		{
			Inner->OnRemoteAudio().AddLambda([this](const FString& StreamLabel, const FString& SubjectName, const float* PCM, int32 NumFrames, int32 NumChannels, int32 SampleRate)
			{
				// Already in float format from inner connector, just forward it
				RemoteAudio.Broadcast(StreamLabel, SubjectName, PCM, NumFrames, NumChannels, SampleRate);
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

		void HandleIncomingData(const uint8* Data, int32 Size)
		{
			// Try parse as JSON announce; ignore on failure
			FString JsonStr;
			FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Data), Size);
			JsonStr = FString(Conv.Length(), Conv.Get());

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
			if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
			{
				FString Type;
				if (Root->TryGetStringField(TEXT("type"), Type) && Type.Equals(TEXT("o3ds.audio.announce"), ESearchCase::IgnoreCase))
				{
					const TArray<TSharedPtr<FJsonValue>>* Tracks;
					if (Root->TryGetArrayField(TEXT("tracks"), Tracks))
					{
						for (const TSharedPtr<FJsonValue>& V : *Tracks)
						{
							TSharedPtr<FJsonObject> T = V->AsObject();
							if (!T.IsValid()) continue;
							FString Stream = T->GetStringField(TEXT("stream"));
							FString Subject = T->GetStringField(TEXT("subject"));
							// Inform inner for routing metadata
							Inner->SetRxAudioRouting(Stream, Subject);
						}
					}
				}
			}
		}

	private:
		TSharedPtr<FWebRTCConnector> Inner;
		IWebRTCConnector::FOnRemoteAudio RemoteAudio;
		TArray<int16> TempPcm16;
		TArray<float> TempPcmFloat;
		TSet<FString> AnnouncedStreams;
		TSet<FString> EnabledStreams;
		TFunction<void(const uint8*, int32)> UserDataCallback;
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
