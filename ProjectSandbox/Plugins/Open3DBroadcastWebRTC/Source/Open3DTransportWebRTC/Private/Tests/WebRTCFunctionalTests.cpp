// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

//
// WP-S7 functional tests for the WebRTC transport (TRF-2, TRF-3, TRF-4, TRF-16, TRF-23, TRF-31).
//
// The sender and receiver are constructed with a fake LiveKit function table (FLkFfiApi,
// ADR 0006 F2) and a fake token fetcher, so nothing touches the network or livekit_ffi.dll.
// Fake LiveKit calls record their arguments; tests fire the recorded callbacks themselves on the
// game thread, so every step is deterministic and there are no sleeps.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/WebRTCSender.h"
#include "../Receiver/WebRTCReceiver.h"
#include "../Shared/LiveKitFfiApi.h"
#include "../Shared/WebRTCTokenManager.h"
#include "../Shared/WebRTCUtils.h"

#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/Base64.h"
#include "Containers/StringConv.h"

#include "Transport/O3DTransportTypes.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportRegistry.h"
THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

// Named namespace (not anonymous) so unity builds cannot collide with other test files.
namespace WebRTCS7Test
{
	struct FFakeTrack
	{
		std::string NameUtf8;
		int32 SampleRate = 0;
		int32 Channels = 0;
		int32 PublishCalls = 0;
		bool bDestroyed = false;
	};

	/** State behind one fake LkClientHandle. Owned by FFakeLiveKit, so it outlives lk_client_destroy. */
	struct FFakeClient
	{
		LkConnectionCallback ConnectionCallback = nullptr;
		void* ConnectionUser = nullptr;
		LkDataCallbackEx DataCallbackEx = nullptr;
		void* DataUserEx = nullptr;
		LkDataCallback DataCallback = nullptr;
		void* DataUser = nullptr;
		LkAudioCallbackEx AudioCallbackEx = nullptr;
		void* AudioUserEx = nullptr;

		/** Registrations with a non-null callback. */
		int32 DataExRegistrations = 0;
		int32 DataRegistrations = 0;

		FString ConnectUrl;
		TArray<FString> ConnectTokens;
		TArray<LkRole> ConnectRoles;
		TArray<FString> RefreshTokens;
		TArray<std::string> SentLabelsUtf8;
		TArray<int32> SentSizes;
		TArray<TUniquePtr<FFakeTrack>> Tracks;
		bool bDestroyed = false;

		void FireConnection(LkConnectionState State)
		{
			if (ConnectionCallback)
			{
				ConnectionCallback(ConnectionUser, State, 0, nullptr);
			}
		}
	};

	/**
	 * Fake LiveKit. lk_client_create has no user-data argument, so the fake functions find the
	 * test's fake through a pointer that exists only in this test file and only while one test
	 * runs. Production code has no global mutable state for this seam.
	 */
	class FFakeLiveKit
	{
	public:
		FFakeLiveKit()
		{
			check(Active == nullptr);
			Active = this;
		}

		~FFakeLiveKit()
		{
			Active = nullptr;
		}

		FFakeLiveKit(const FFakeLiveKit&) = delete;
		FFakeLiveKit& operator=(const FFakeLiveKit&) = delete;

		TArray<TUniquePtr<FFakeClient>> Clients;
		int32 DataCallbackExResult = 0;
		int32 RefreshTokenResult = 0;
		int32 SendDataResult = 0;

		FFakeClient* LastClient() const
		{
			return Clients.Num() > 0 ? Clients.Last().Get() : nullptr;
		}

		static FLkFfiApi MakeApi();

		static FFakeLiveKit* Active;
	};

	FFakeLiveKit* FFakeLiveKit::Active = nullptr;

	static LkResult MakeResult(int32 Code)
	{
		LkResult Result;
		Result.code = Code;
		Result.message = nullptr;
		return Result;
	}

	static FFakeClient* AsClient(LkClientHandle* Handle)
	{
		return reinterpret_cast<FFakeClient*>(Handle);
	}

	static void Fake_free_str(char*) {}

	static LkClientHandle* Fake_client_create()
	{
		if (!FFakeLiveKit::Active)
		{
			return nullptr;
		}
		TUniquePtr<FFakeClient> Client = MakeUnique<FFakeClient>();
		FFakeClient* Raw = Client.Get();
		FFakeLiveKit::Active->Clients.Add(MoveTemp(Client));
		return reinterpret_cast<LkClientHandle*>(Raw);
	}

	static void Fake_client_destroy(LkClientHandle* Handle)
	{
		if (FFakeClient* Client = AsClient(Handle))
		{
			Client->bDestroyed = true;
		}
	}

	static LkResult Fake_set_data_callback(LkClientHandle* Handle, LkDataCallback Cb, void* User)
	{
		FFakeClient* Client = AsClient(Handle);
		Client->DataCallback = Cb;
		Client->DataUser = User;
		Client->DataRegistrations += Cb ? 1 : 0;
		return MakeResult(0);
	}

	static LkResult Fake_set_data_callback_ex(LkClientHandle* Handle, LkDataCallbackEx Cb, void* User)
	{
		FFakeClient* Client = AsClient(Handle);
		if (Cb && FFakeLiveKit::Active && FFakeLiveKit::Active->DataCallbackExResult != 0)
		{
			return MakeResult(FFakeLiveKit::Active->DataCallbackExResult);
		}
		Client->DataCallbackEx = Cb;
		Client->DataUserEx = User;
		Client->DataExRegistrations += Cb ? 1 : 0;
		return MakeResult(0);
	}

	static LkResult Fake_set_audio_callback_ex(LkClientHandle* Handle, LkAudioCallbackEx Cb, void* User)
	{
		FFakeClient* Client = AsClient(Handle);
		Client->AudioCallbackEx = Cb;
		Client->AudioUserEx = User;
		return MakeResult(0);
	}

	static LkResult Fake_set_connection_callback(LkClientHandle* Handle, LkConnectionCallback Cb, void* User)
	{
		FFakeClient* Client = AsClient(Handle);
		Client->ConnectionCallback = Cb;
		Client->ConnectionUser = User;
		return MakeResult(0);
	}

	static LkResult Fake_connect_with_role_async(LkClientHandle* Handle, const char* Url, const char* Token, LkRole Role)
	{
		FFakeClient* Client = AsClient(Handle);
		Client->ConnectUrl = UTF8_TO_TCHAR(Url);
		Client->ConnectTokens.Add(UTF8_TO_TCHAR(Token));
		Client->ConnectRoles.Add(Role);
		return MakeResult(0);
	}

	static LkResult Fake_disconnect(LkClientHandle*)
	{
		return MakeResult(0);
	}

	static LkResult Fake_refresh_token(LkClientHandle* Handle, const char* Token)
	{
		AsClient(Handle)->RefreshTokens.Add(UTF8_TO_TCHAR(Token));
		return MakeResult(FFakeLiveKit::Active ? FFakeLiveKit::Active->RefreshTokenResult : 0);
	}

	static LkResult Fake_set_audio_publish_options(LkClientHandle*, int32_t, int32_t, int32_t)
	{
		return MakeResult(0);
	}

	static LkResult Fake_set_audio_output_format(LkClientHandle*, int32_t, int32_t)
	{
		return MakeResult(0);
	}

	static LkResult Fake_audio_track_create(LkClientHandle* Handle, const LkAudioTrackConfig* Config, LkAudioTrackHandle** OutTrack)
	{
		FFakeClient* Client = AsClient(Handle);
		TUniquePtr<FFakeTrack> Track = MakeUnique<FFakeTrack>();
		// Copy the name now, while the caller's converter is alive (TRF-2).
		Track->NameUtf8 = Config->track_name ? std::string(Config->track_name) : std::string();
		Track->SampleRate = Config->sample_rate;
		Track->Channels = Config->channels;
		*OutTrack = reinterpret_cast<LkAudioTrackHandle*>(Track.Get());
		Client->Tracks.Add(MoveTemp(Track));
		return MakeResult(0);
	}

	static LkResult Fake_audio_track_destroy(LkAudioTrackHandle* Track)
	{
		if (Track)
		{
			reinterpret_cast<FFakeTrack*>(Track)->bDestroyed = true;
		}
		return MakeResult(0);
	}

	static LkResult Fake_audio_track_publish_pcm_i16(LkAudioTrackHandle* Track, const int16_t*, size_t)
	{
		reinterpret_cast<FFakeTrack*>(Track)->PublishCalls++;
		return MakeResult(0);
	}

	static LkResult Fake_send_data_ex(LkClientHandle* Handle, const uint8_t*, size_t Len, LkReliability, int32_t, const char* Label)
	{
		FFakeClient* Client = AsClient(Handle);
		const int32 Code = FFakeLiveKit::Active ? FFakeLiveKit::Active->SendDataResult : 0;
		if (Code == 0)
		{
			Client->SentLabelsUtf8.Add(Label ? std::string(Label) : std::string());
			Client->SentSizes.Add(static_cast<int32>(Len));
		}
		return MakeResult(Code);
	}

	static LkResult Fake_set_default_data_labels(LkClientHandle*, const char*, const char*)
	{
		return MakeResult(0);
	}

	static LkResult Fake_set_log_level(LkClientHandle*, LkLogLevel)
	{
		return MakeResult(0);
	}

	FLkFfiApi FFakeLiveKit::MakeApi()
	{
		FLkFfiApi Api;
		Api.lk_free_str = &Fake_free_str;
		Api.lk_client_create = &Fake_client_create;
		Api.lk_client_destroy = &Fake_client_destroy;
		Api.lk_client_set_data_callback = &Fake_set_data_callback;
		Api.lk_client_set_data_callback_ex = &Fake_set_data_callback_ex;
		Api.lk_client_set_audio_callback_ex = &Fake_set_audio_callback_ex;
		Api.lk_set_connection_callback = &Fake_set_connection_callback;
		Api.lk_connect_with_role_async = &Fake_connect_with_role_async;
		Api.lk_disconnect = &Fake_disconnect;
		Api.lk_refresh_token = &Fake_refresh_token;
		Api.lk_set_audio_publish_options = &Fake_set_audio_publish_options;
		Api.lk_set_audio_output_format = &Fake_set_audio_output_format;
		Api.lk_audio_track_create = &Fake_audio_track_create;
		Api.lk_audio_track_destroy = &Fake_audio_track_destroy;
		Api.lk_audio_track_publish_pcm_i16 = &Fake_audio_track_publish_pcm_i16;
		Api.lk_send_data_ex = &Fake_send_data_ex;
		Api.lk_set_default_data_labels = &Fake_set_default_data_labels;
		Api.lk_set_log_level = &Fake_set_log_level;
		return Api;
	}

	/** Records fetch requests; the test completes them explicitly. */
	class FFakeTokenFetcher : public IO3DTokenFetcher
	{
	public:
		virtual void FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete) override
		{
			Requests.Add(Request);
			Pending.Add(MoveTemp(OnComplete));
		}

		virtual void CancelPendingRequests() override
		{
			++CancelCount;
			Pending.Reset();
		}

		bool CompleteNext(const FString& Token, int64 ExpiresAtUnix)
		{
			if (Pending.Num() == 0)
			{
				return false;
			}
			TFunction<void(const FO3DTokenResult&)> Callback = MoveTemp(Pending[0]);
			Pending.RemoveAt(0);

			FO3DTokenResult Result;
			Result.bSuccess = true;
			Result.Token = Token;
			Result.ExpiresAt = ExpiresAtUnix;
			if (Callback)
			{
				Callback(Result);
			}
			return true;
		}

		TArray<FO3DTokenFetchRequest> Requests;
		TArray<TFunction<void(const FO3DTokenResult&)>> Pending;
		int32 CancelCount = 0;
	};

	using FFakeFetcherRef = TSharedRef<FFakeTokenFetcher, ESPMode::ThreadSafe>;

	static FO3DTokenFetcherFactory MakeFetcherFactory(const FFakeFetcherRef& Fetcher)
	{
		return [Fetcher]() -> TSharedRef<IO3DTokenFetcher, ESPMode::ThreadSafe>
		{
			return Fetcher;
		};
	}

	class FRecordingConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InStreamId, const TArray<uint8>& InPayload, double InTimestamp) override
		{
			StreamIds.Add(InStreamId);
			PayloadSizes.Add(InPayload.Num());
		}

		TArray<FString> StreamIds;
		TArray<int32> PayloadSizes;
	};

	class FRecordingAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes) override
		{
			FScopeLock Lock(&Mutex);
			Labels.Add(Meta.StreamLabel);
		}

		FCriticalSection Mutex;
		TArray<FString> Labels;
	};

	static int64 UnixNow()
	{
		return FDateTime::UtcNow().ToUnixTimestamp();
	}

	static FO3DTransportConfig MakeAutoFetchConfig()
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("127.0.0.1:7880");
		Config.bUseAutoTokenFetch = true;
		// Never contacted: the fake fetcher replaces HTTP.
		Config.TokenEndpointUrl = TEXT("http://127.0.0.1:1/token");
		Config.TokenRefreshLeadTimeSec = 300;
		Config.AdvancedParams.Add(WebRTCUtils::RoomOptionKey, TEXT("wp-s7-room"));
		// Disable the receiver's no-data watchdog so tests stay deterministic.
		Config.AdvancedParams.Add(TEXT("webrtc.reconnect_timeout"), TEXT("0"));
		return Config;
	}

	/** A JWT-shaped token whose payload is {"exp":<one hour from now>}; the signature is not checked. */
	static FString MakeTestJwt()
	{
		const FString Payload = FString::Printf(TEXT("{\"exp\":%lld}"), UnixNow() + 3600);
		const FTCHARToUTF8 PayloadUtf8(*Payload);
		const FString PayloadBase64 = FBase64::Encode(reinterpret_cast<const uint8*>(PayloadUtf8.Get()), static_cast<uint32>(PayloadUtf8.Length()));
		return FString(TEXT("eyJhbGciOiJIUzI1NiJ9.")) + PayloadBase64 + TEXT(".c2ln");
	}

	static FO3DTransportConfig MakeManualConfig()
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("127.0.0.1:7880");
		Config.Token = MakeTestJwt();
		Config.AdvancedParams.Add(TEXT("webrtc.reconnect_timeout"), TEXT("0"));
		return Config;
	}

	/** "Sub" + U+00E9 + "ject_" + U+89D2 + U+8272, built from code points so the source stays ASCII. */
	static FString MakeNonAsciiName()
	{
		FString Name(TEXT("Sub"));
		Name.AppendChar(static_cast<TCHAR>(0x00E9));
		Name += TEXT("ject_");
		Name.AppendChar(static_cast<TCHAR>(0x89D2));
		Name.AppendChar(static_cast<TCHAR>(0x8272));
		return Name;
	}

	/** UTF-8 bytes of MakeNonAsciiName(). */
	static const char* NonAsciiNameUtf8()
	{
		return "Sub\xC3\xA9ject_\xE8\xA7\x92\xE8\x89\xB2";
	}
}

// ---------------------------------------------------------------------------------------------
// TRF-3: auto-fetched token leads to a connect
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7SenderAutoFetchConnectsTest,
	"Open3DBroadcast.Transport.WebRTC.Token.SenderAutoFetchConnects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7SenderAutoFetchConnectsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FFakeFetcherRef Fetcher = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi(), MakeFetcherFactory(Fetcher));
		if (!TestTrue(TEXT("Initialize"), Sender.Initialize(MakeAutoFetchConfig()).IsOk()))
		{
			return false;
		}
		FFakeClient* Client = Fake.LastClient();
		if (!TestNotNull(TEXT("Client created"), Client))
		{
			return false;
		}

		TestTrue(TEXT("Start"), Sender.Start().IsOk());
		TestEqual(TEXT("One token request after Start"), Fetcher->Requests.Num(), 1);
		TestEqual(TEXT("No connect before a token exists"), Client->ConnectTokens.Num(), 0);
		if (Fetcher->Requests.Num() > 0)
		{
			TestEqual(TEXT("Room comes from webrtc.room"), Fetcher->Requests[0].RoomName, FString(TEXT("wp-s7-room")));
			TestEqual(TEXT("Publisher role"), Fetcher->Requests[0].Role, FString(TEXT("publisher")));
			TestTrue(TEXT("Identity has the sender prefix"), Fetcher->Requests[0].Identity.StartsWith(TEXT("sender-")));
		}

		Sender.Tick(0.f);
		TestEqual(TEXT("Waiting for the token does not refetch"), Fetcher->Requests.Num(), 1);

		TestTrue(TEXT("Complete fetch"), Fetcher->CompleteNext(TEXT("token-a"), UnixNow() + 3600));
		Sender.Tick(0.f);
		TestEqual(TEXT("Connected once after the token arrived"), Client->ConnectTokens.Num(), 1);
		if (Client->ConnectTokens.Num() == 1)
		{
			TestEqual(TEXT("Connect used the fetched token"), Client->ConnectTokens[0], FString(TEXT("token-a")));
			TestEqual(TEXT("Publisher role on connect"), static_cast<int32>(Client->ConnectRoles[0]), static_cast<int32>(LkRolePublisher));
			TestEqual(TEXT("Localhost URL gets ws://"), Client->ConnectUrl, FString(TEXT("ws://127.0.0.1:7880")));
		}

		Sender.Tick(0.f);
		TestEqual(TEXT("No second connect"), Client->ConnectTokens.Num(), 1);
		TestEqual(TEXT("No refetch for a fresh token"), Fetcher->Requests.Num(), 1);

		Sender.Stop();
		TestTrue(TEXT("Client destroyed on Stop"), Client->bDestroyed);
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7ReceiverAutoFetchConnectsTest,
	"Open3DBroadcast.Transport.WebRTC.Token.ReceiverAutoFetchConnects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7ReceiverAutoFetchConnectsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FFakeFetcherRef Fetcher = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi(), MakeFetcherFactory(Fetcher));
		if (!TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeAutoFetchConfig()).IsOk()))
		{
			return false;
		}
		Receiver.SetConsumer(MakeShared<FRecordingConsumer>());
		FFakeClient* Client = Fake.LastClient();
		if (!TestNotNull(TEXT("Client created"), Client))
		{
			return false;
		}

		TestTrue(TEXT("Start"), Receiver.Start().IsOk());
		TestEqual(TEXT("One token request after Start"), Fetcher->Requests.Num(), 1);
		TestEqual(TEXT("No connect before a token exists"), Client->ConnectTokens.Num(), 0);
		if (Fetcher->Requests.Num() > 0)
		{
			TestEqual(TEXT("Same room as the sender"), Fetcher->Requests[0].RoomName, FString(TEXT("wp-s7-room")));
			TestEqual(TEXT("Subscriber role"), Fetcher->Requests[0].Role, FString(TEXT("subscriber")));
			TestTrue(TEXT("Identity has the receiver prefix"), Fetcher->Requests[0].Identity.StartsWith(TEXT("receiver-")));
		}

		TestTrue(TEXT("Complete fetch"), Fetcher->CompleteNext(TEXT("token-r"), UnixNow() + 3600));
		Receiver.Poll();
		TestEqual(TEXT("Connected once after the token arrived"), Client->ConnectTokens.Num(), 1);
		if (Client->ConnectTokens.Num() == 1)
		{
			TestEqual(TEXT("Connect used the fetched token"), Client->ConnectTokens[0], FString(TEXT("token-r")));
			TestEqual(TEXT("Subscriber role on connect"), static_cast<int32>(Client->ConnectRoles[0]), static_cast<int32>(LkRoleSubscriber));
		}

		Receiver.Poll();
		TestEqual(TEXT("No second connect"), Client->ConnectTokens.Num(), 1);
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7IdentityUniqueTest,
	"Open3DBroadcast.Transport.WebRTC.Token.IdentityPerInstanceAndRoomRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7IdentityUniqueTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FFakeFetcherRef FetcherA = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	const FFakeFetcherRef FetcherB = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCSender SenderA(FFakeLiveKit::MakeApi(), MakeFetcherFactory(FetcherA));
		FO3DWebRTCSender SenderB(FFakeLiveKit::MakeApi(), MakeFetcherFactory(FetcherB));
		TestTrue(TEXT("Initialize A"), SenderA.Initialize(MakeAutoFetchConfig()).IsOk());
		TestTrue(TEXT("Initialize B"), SenderB.Initialize(MakeAutoFetchConfig()).IsOk());
		SenderA.Start();
		SenderB.Start();
		if (FetcherA->Requests.Num() == 1 && FetcherB->Requests.Num() == 1)
		{
			TestNotEqual(TEXT("Two senders in one process use different identities (TRF-25)"),
				FetcherA->Requests[0].Identity, FetcherB->Requests[0].Identity);
		}
		else
		{
			AddError(TEXT("Each sender should have requested one token"));
		}

		// Auto-fetch without a room fails fast.
		FO3DWebRTCSender NoRoom(FFakeLiveKit::MakeApi(), MakeFetcherFactory(FetcherA));
		FO3DTransportConfig Config = MakeAutoFetchConfig();
		Config.AdvancedParams.Remove(WebRTCUtils::RoomOptionKey);
		AddExpectedError(TEXT("no room set"), EAutomationExpectedMessageFlags::Contains, 1);
		TestFalse(TEXT("Initialize without webrtc.room fails in auto-fetch mode"), NoRoom.Initialize(Config).IsOk());
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// TRF-23: a refreshed token reaches lk_refresh_token
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7SenderRefreshTokenTest,
	"Open3DBroadcast.Transport.WebRTC.Token.SenderRefreshCallsLkRefreshToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7SenderRefreshTokenTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FFakeFetcherRef Fetcher = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi(), MakeFetcherFactory(Fetcher));
		if (!TestTrue(TEXT("Initialize"), Sender.Initialize(MakeAutoFetchConfig()).IsOk()))
		{
			return false;
		}
		FFakeClient* Client = Fake.LastClient();
		Sender.Start();

		// First token expires inside the 300 s refresh lead time.
		Fetcher->CompleteNext(TEXT("token-short"), UnixNow() + 10);
		Sender.Tick(0.f);
		TestEqual(TEXT("Connected with the first token"), Client->ConnectTokens.Num(), 1);
		Client->FireConnection(LkConnConnected);

		Sender.Tick(0.f);
		TestEqual(TEXT("A refresh fetch started"), Fetcher->Requests.Num(), 2);

		Fetcher->CompleteNext(TEXT("token-long"), UnixNow() + 3600);
		Sender.Tick(0.f);
		TestEqual(TEXT("lk_refresh_token called once"), Client->RefreshTokens.Num(), 1);
		if (Client->RefreshTokens.Num() == 1)
		{
			TestEqual(TEXT("lk_refresh_token got the new token"), Client->RefreshTokens[0], FString(TEXT("token-long")));
		}
		TestEqual(TEXT("Refresh does not reconnect"), Client->ConnectTokens.Num(), 1);

		Sender.Tick(0.f);
		TestEqual(TEXT("No repeated lk_refresh_token"), Client->RefreshTokens.Num(), 1);
		TestEqual(TEXT("No further fetch for a fresh token"), Fetcher->Requests.Num(), 2);
		Sender.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7ReceiverRefreshTokenTest,
	"Open3DBroadcast.Transport.WebRTC.Token.ReceiverRefreshCallsLkRefreshToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7ReceiverRefreshTokenTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FFakeFetcherRef Fetcher = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi(), MakeFetcherFactory(Fetcher));
		if (!TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeAutoFetchConfig()).IsOk()))
		{
			return false;
		}
		Receiver.SetConsumer(MakeShared<FRecordingConsumer>());
		FFakeClient* Client = Fake.LastClient();
		Receiver.Start();

		Fetcher->CompleteNext(TEXT("token-short"), UnixNow() + 10);
		Receiver.Poll();
		TestEqual(TEXT("Connected with the first token"), Client->ConnectTokens.Num(), 1);
		Client->FireConnection(LkConnConnected);

		Receiver.Poll();
		TestEqual(TEXT("A refresh fetch started"), Fetcher->Requests.Num(), 2);

		Fetcher->CompleteNext(TEXT("token-long"), UnixNow() + 3600);
		Receiver.Poll();
		TestEqual(TEXT("lk_refresh_token called once"), Client->RefreshTokens.Num(), 1);
		if (Client->RefreshTokens.Num() == 1)
		{
			TestEqual(TEXT("lk_refresh_token got the new token"), Client->RefreshTokens[0], FString(TEXT("token-long")));
		}
		TestEqual(TEXT("Successful refresh keeps the client"), Fake.Clients.Num(), 1);
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7ReceiverRefreshFallbackTest,
	"Open3DBroadcast.Transport.WebRTC.Token.ReceiverRefreshFailureReconnects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7ReceiverRefreshFallbackTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	Fake.RefreshTokenResult = 101; // lk_refresh_token unsupported
	const FFakeFetcherRef Fetcher = MakeShared<FFakeTokenFetcher, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi(), MakeFetcherFactory(Fetcher));
		if (!TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeAutoFetchConfig()).IsOk()))
		{
			return false;
		}
		Receiver.SetConsumer(MakeShared<FRecordingConsumer>());
		FFakeClient* FirstClient = Fake.LastClient();
		Receiver.Start();

		Fetcher->CompleteNext(TEXT("token-short"), UnixNow() + 10);
		Receiver.Poll();
		FirstClient->FireConnection(LkConnConnected);
		Receiver.Poll();
		Fetcher->CompleteNext(TEXT("token-long"), UnixNow() + 3600);
		Receiver.Poll();

		TestEqual(TEXT("lk_refresh_token was tried"), FirstClient->RefreshTokens.Num(), 1);
		TestTrue(TEXT("Old client destroyed by the fallback reconnect"), FirstClient->bDestroyed);
		TestEqual(TEXT("A new client was created"), Fake.Clients.Num(), 2);
		FFakeClient* SecondClient = Fake.LastClient();
		if (SecondClient && SecondClient != FirstClient)
		{
			TestEqual(TEXT("Reconnect connects once"), SecondClient->ConnectTokens.Num(), 1);
			if (SecondClient->ConnectTokens.Num() == 1)
			{
				TestEqual(TEXT("Reconnect uses the new token"), SecondClient->ConnectTokens[0], FString(TEXT("token-long")));
			}
		}
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// TRF-4 / TRF-19: send failures leave the caller's SubjectList intact
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7SendFailureNoDoubleFreeTest,
	"Open3DBroadcast.Transport.WebRTC.Sender.SendFailureKeepsCallerList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7SendFailureNoDoubleFreeTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	// The old path borrowed the caller's Transform pointers into a pooled Subject and deleted
	// them when serialization failed. SubjectList::Serialize cannot currently fail, so this test
	// drives the failure branches that remain (oversized payload, lk_send_data_ex error) and
	// checks that the caller's list still owns the same transforms and destructs normally.
	FFakeLiveKit Fake;
	FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi());
	if (!TestTrue(TEXT("Initialize"), Sender.Initialize(MakeManualConfig()).IsOk()))
	{
		return false;
	}
	FFakeClient* Client = Fake.LastClient();
	Sender.Start();
	Client->FireConnection(LkConnConnected);

	{
		// Oversized: more than 15000 bytes once serialized.
		O3DS::SubjectList List;
		O3DS::Subject* Subject = List.addSubject("Big");
		for (int32 Index = 0; Index < 600; ++Index)
		{
			Subject->addTransform(std::string("Bone_") + std::to_string(Index), Index - 1);
		}
		const std::vector<O3DS::Transform*> Before = Subject->mTransforms.mItems;

		AddExpectedError(TEXT("exceeds maximum"), EAutomationExpectedMessageFlags::Contains, 1);
		TestFalse(TEXT("Oversized list is rejected"), Sender.Send(List));
		TestEqual(TEXT("Caller keeps its subjects"), static_cast<int32>(List.mItems.size()), 1);
		TestTrue(TEXT("Caller keeps the same transform pointers"), Subject->mTransforms.mItems == Before);
		TestEqual(TEXT("Nothing reached LiveKit"), Client->SentSizes.Num(), 0);
		// List is destroyed here; a double free would crash or trip the allocator.
	}

	{
		O3DS::SubjectList List;
		O3DS::Subject* Subject = List.addSubject("Small");
		Subject->addTransform("Root", -1);
		Subject->addTransform("Spine", 0);
		const std::vector<O3DS::Transform*> Before = Subject->mTransforms.mItems;

		Fake.SendDataResult = 201;
		TestFalse(TEXT("lk_send_data_ex failure is reported"), Sender.Send(List));
		TestTrue(TEXT("Caller keeps the same transform pointers"), Subject->mTransforms.mItems == Before);

		Fake.SendDataResult = 0;
		TestTrue(TEXT("Send succeeds when LiveKit accepts"), Sender.Send(List));
		TestTrue(TEXT("Caller still owns its transforms after success"), Subject->mTransforms.mItems == Before);
		if (Client->SentLabelsUtf8.Num() == 1)
		{
			TestTrue(TEXT("Label is the subject name"), Client->SentLabelsUtf8[0] == std::string("Small"));
		}
	}

	const FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("One frame sent"), static_cast<int32>(Stats.FramesSent), 1);
	TestEqual(TEXT("Two frames dropped"), static_cast<int32>(Stats.DroppedFrames), 2);
	Sender.Stop();
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// TRF-16: exactly one data callback
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7OneDataCallbackTest,
	"Open3DBroadcast.Transport.WebRTC.Receiver.OneDataCallbackRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7OneDataCallbackTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
		TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeManualConfig()).IsOk());
		FFakeClient* Client = Fake.LastClient();
		if (TestNotNull(TEXT("Client created"), Client))
		{
			TestEqual(TEXT("Labeled data callback registered once"), Client->DataExRegistrations, 1);
			TestEqual(TEXT("Unlabeled data callback not registered"), Client->DataRegistrations, 0);
		}
		Receiver.Stop();
	}

	// Fallback: the unlabeled callback is used only when the labeled registration fails.
	Fake.DataCallbackExResult = 401;
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
		TestTrue(TEXT("Initialize with fallback"), Receiver.Initialize(MakeManualConfig()).IsOk());
		FFakeClient* Client = Fake.LastClient();
		if (TestNotNull(TEXT("Client created"), Client))
		{
			TestEqual(TEXT("Labeled registration failed"), Client->DataExRegistrations, 0);
			TestEqual(TEXT("Unlabeled fallback registered once"), Client->DataRegistrations, 1);
		}
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// TRF-2 / TRF-31: UTF-8 labels round-trip from sender to receiver
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7Utf8LabelRoundTripTest,
	"Open3DBroadcast.Transport.WebRTC.Labels.Utf8RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7Utf8LabelRoundTripTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	const FString Name = MakeNonAsciiName();
	const std::string ExpectedUtf8 = NonAsciiNameUtf8();

	// Sender: data label and audio track name are UTF-8 on the wire.
	std::string SentLabel;
	std::string TrackName;
	{
		FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi());
		FO3DTransportConfig Config = MakeManualConfig();
		Config.Audio.bEnableAudio = true;
		Config.Audio.NumChannels = 1;
		Config.Audio.SampleRate = 48000;
		if (!TestTrue(TEXT("Sender initialize"), Sender.Initialize(Config).IsOk()))
		{
			return false;
		}
		FFakeClient* Client = Fake.LastClient();
		Sender.Start();
		Client->FireConnection(LkConnConnected);

		const uint8 Payload[4] = { 1, 2, 3, 4 };
		TestTrue(TEXT("SendSerialized"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Payload, 4, Name, 0.0)) == EO3DSendResult::Queued);
		if (Client->SentLabelsUtf8.Num() == 1)
		{
			SentLabel = Client->SentLabelsUtf8[0];
		}
		TestTrue(TEXT("Data label is the UTF-8 encoding of the subject name"), SentLabel == ExpectedUtf8);

		FO3DTransportAudioConfig AudioConfig;
		AudioConfig.bEnableAudio = true;
		AudioConfig.NumChannels = 1;
		AudioConfig.SampleRate = 48000;
		const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink = Sender.CreateAudioSink(AudioConfig);
		TArray<float> Pcm;
		Pcm.SetNumZeroed(480);
		TestTrue(TEXT("Sink publishes while connected"), Sink.IsValid() && Sink->SubmitPcm(Name, Pcm.GetData(), 480, 1, 48000, 0.0));
		if (Client->Tracks.Num() == 1)
		{
			TrackName = Client->Tracks[0]->NameUtf8;
		}
		TestTrue(TEXT("Track name is the UTF-8 encoding of the subject name (TRF-2)"), TrackName == ExpectedUtf8);
		Sender.Stop();
		TestTrue(TEXT("Track destroyed on Stop"), Client->Tracks.Num() == 1 && Client->Tracks[0]->bDestroyed);
	}

	// Receiver: the same bytes decode back to the original name.
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
		if (!TestTrue(TEXT("Receiver initialize"), Receiver.Initialize(MakeManualConfig()).IsOk()))
		{
			return false;
		}
		const TSharedPtr<FRecordingConsumer> Consumer = MakeShared<FRecordingConsumer>();
		const TSharedPtr<FRecordingAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FRecordingAudioSink, ESPMode::ThreadSafe>();
		Receiver.SetConsumer(Consumer);
		FO3DTransportAudioConfig AudioConfig;
		AudioConfig.bEnableAudio = true;
		AudioConfig.NumChannels = 1;
		AudioConfig.SampleRate = 48000;
		Receiver.SetAudioSink(AudioSink, AudioConfig);
		TestTrue(TEXT("Receiver start"), Receiver.Start().IsOk());

		FFakeClient* Client = Fake.LastClient();
		if (!TestNotNull(TEXT("Receiver client"), Client) || !Client->DataCallbackEx || !Client->AudioCallbackEx)
		{
			AddError(TEXT("Receiver did not register its callbacks"));
			return false;
		}
		Client->FireConnection(LkConnConnected);

		const uint8 Bytes[3] = { 9, 8, 7 };
		Client->DataCallbackEx(Client->DataUserEx, SentLabel.c_str(), LkReliable, Bytes, 3);
		Receiver.Poll();
		TestEqual(TEXT("One frame delivered"), Consumer->StreamIds.Num(), 1);
		if (Consumer->StreamIds.Num() == 1)
		{
			TestEqual(TEXT("Consumer sees the original name"), Consumer->StreamIds[0], Name);
		}

		const int16 Samples[2] = { 0, 0 };
		Client->AudioCallbackEx(Client->AudioUserEx, Samples, 2, 1, 48000, "participant", TrackName.c_str());
		TestEqual(TEXT("One audio frame delivered"), AudioSink->Labels.Num(), 1);
		if (AudioSink->Labels.Num() == 1)
		{
			TestEqual(TEXT("Audio label is the original name"), AudioSink->Labels[0], Name);
		}

		// A callback that arrives after the receiver is gone resolves to nothing (opaque token).
		LkDataCallbackEx LateCallback = Client->DataCallbackEx;
		void* LateUser = Client->DataUserEx;
		Receiver.Stop();
		TestTrue(TEXT("Stop cleared the data callback"), Client->DataCallbackEx == nullptr);
		LateCallback(LateUser, SentLabel.c_str(), LkReliable, Bytes, 3);
		TestEqual(TEXT("Late frame after Stop is not delivered to a consumer"), Receiver.Poll(), 0);
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCS7LateCallbackAfterDestroyTest,
	"Open3DBroadcast.Transport.WebRTC.Receiver.LateCallbackAfterDestroyIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCS7LateCallbackAfterDestroyTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	LkDataCallbackEx DataCallback = nullptr;
	LkConnectionCallback ConnectionCallback = nullptr;
	void* User = nullptr;
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
		TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeManualConfig()).IsOk());
		FFakeClient* Client = Fake.LastClient();
		if (!TestNotNull(TEXT("Client"), Client))
		{
			return false;
		}
		DataCallback = Client->DataCallbackEx;
		ConnectionCallback = Client->ConnectionCallback;
		User = Client->DataUserEx;
		TestTrue(TEXT("user_data is not the receiver address"), User != static_cast<void*>(&Receiver));
	}

	// The receiver and its link are gone; the token must resolve to nothing.
	const uint8 Bytes[2] = { 1, 2 };
	if (DataCallback && ConnectionCallback)
	{
		DataCallback(User, "late", LkReliable, Bytes, 2);
		ConnectionCallback(User, LkConnDisconnected, 0, nullptr);
		TestTrue(TEXT("Late callbacks after destruction did not crash"), true);
	}
	else
	{
		AddError(TEXT("Callbacks were not registered"));
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// WP-A1 PR 3 (ADR 0007 items 3 and 4): result codes, connection state and capabilities. The
// WebRTC conformance profile waits for an add-on test module (WP-T2e), so these mirror the
// conformance cases on the fake LiveKit table.
// ---------------------------------------------------------------------------------------------

namespace WebRTCA1Pr3Test
{
	/** Records connection-state callbacks and whether each ran on the game thread. */
	struct FStateLog
	{
		TArray<EO3DConnectionState> States;
		TArray<EO3DTransportError> Codes;
		bool bAllOnGameThread = true;

		FO3DConnectionStateCallback MakeCallback()
		{
			return [this](EO3DConnectionState State, const FO3DTransportResult& Reason)
			{
				States.Add(State);
				Codes.Add(Reason.Code);
				bAllOnGameThread &= IsInGameThread();
			};
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCA1SendResultCodesTest,
	"Open3DBroadcast.Transport.WebRTC.Results.SendResultCodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCA1SendResultCodesTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi());
	const uint8 Small[4] = { 1, 2, 3, 4 };

	TestTrue(TEXT("SendSerialized before Initialize returns NotRunning"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::NotRunning);
	if (!TestTrue(TEXT("Initialize"), Sender.Initialize(MakeManualConfig()).IsOk()))
	{
		return false;
	}
	TestTrue(TEXT("SendSerialized before Start returns NotRunning"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::NotRunning);
	FFakeClient* Client = Fake.LastClient();
	TestTrue(TEXT("Start"), Sender.Start().IsOk());
	TestTrue(TEXT("SendSerialized before LiveKit connects returns NotConnected"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::NotConnected);
	if (!TestNotNull(TEXT("Client created"), Client))
	{
		return false;
	}
	Client->FireConnection(LkConnConnected);

	TestTrue(TEXT("A frame is Queued once connected"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::Queued);
	TestTrue(TEXT("An empty payload is Invalid"), Sender.SendSerialized(FO3DSendPayload()) == EO3DSendResult::Invalid);

	TArray<uint8> Oversize;
	Oversize.SetNumZeroed(WebRTCUtils::ReliableMaxDataBytes + 1);
	AddExpectedError(TEXT("exceeds maximum"), EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("A frame over the reliable data limit is TooLarge"), Sender.SendSerialized(FO3DSendPayload(MoveTemp(Oversize), TEXT("A"), 0.0)) == EO3DSendResult::TooLarge);
	TestEqual(TEXT("The capability names the same limit"), Sender.GetCapabilities().MaxPayloadBytes, WebRTCUtils::ReliableMaxDataBytes);

	Fake.SendDataResult = 201;
	TestTrue(TEXT("An lk_send_data_ex refusal is DroppedBackpressure"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::DroppedBackpressure);
	Fake.SendDataResult = 0;
	TestTrue(TEXT("The refusal is counted as a send error"), Sender.GetStats().SendErrors >= 1);

	Sender.Stop();
	TestTrue(TEXT("SendSerialized after Stop returns NotRunning"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 4, TEXT("A"))) == EO3DSendResult::NotRunning);
	TestEqual(TEXT("Exactly one frame sent"), static_cast<int32>(Sender.GetStats().FramesSent), 1);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCA1SenderStateTest,
	"Open3DBroadcast.Transport.WebRTC.State.SenderTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCA1SenderStateTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	using namespace WebRTCA1Pr3Test;
	FFakeLiveKit Fake;
	FStateLog Log;
	{
		FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi());
		Sender.SetStateChangedCallback(Log.MakeCallback());
		TestTrue(TEXT("Idle before Start"), Sender.GetConnectionState() == EO3DConnectionState::Idle);
		if (!TestTrue(TEXT("Initialize"), Sender.Initialize(MakeManualConfig()).IsOk()))
		{
			return false;
		}
		FFakeClient* Client = Fake.LastClient();
		TestTrue(TEXT("Start"), Sender.Start().IsOk());
		TestTrue(TEXT("Connecting after Start"), Sender.GetConnectionState() == EO3DConnectionState::Connecting);
		if (!TestNotNull(TEXT("Client created"), Client))
		{
			return false;
		}

		// LiveKit reports on its own thread in production; the sender applies it in Tick.
		Client->FireConnection(LkConnConnected);
		TestTrue(TEXT("Not applied before Tick"), Sender.GetConnectionState() == EO3DConnectionState::Connecting);
		Sender.Tick(0.0f);
		TestTrue(TEXT("Connected after Tick"), Sender.GetConnectionState() == EO3DConnectionState::Connected);
		TestTrue(TEXT("Stats.State follows"), Sender.GetStats().State == EO3DConnectionState::Connected);

		Client->FireConnection(LkConnReconnecting);
		Sender.Tick(0.0f);
		TestTrue(TEXT("Reconnecting while LiveKit reconnects"), Sender.GetConnectionState() == EO3DConnectionState::Reconnecting);
		Client->FireConnection(LkConnConnected);
		Sender.Tick(0.0f);
		TestTrue(TEXT("Connected again"), Sender.GetConnectionState() == EO3DConnectionState::Connected);

		Client->FireConnection(LkConnFailed);
		Sender.Tick(0.0f);
		TestTrue(TEXT("Failed when LiveKit gives up"), Sender.GetConnectionState() == EO3DConnectionState::Failed);

		Sender.Stop();
		TestTrue(TEXT("Idle after Stop"), Sender.GetConnectionState() == EO3DConnectionState::Idle);
		const int32 AfterStop = Log.States.Num();
		Sender.Tick(0.0f);
		TestEqual(TEXT("Nothing reported after Stop"), Log.States.Num(), AfterStop);
	}

	const TArray<EO3DConnectionState> Expected = {
		EO3DConnectionState::Connecting, EO3DConnectionState::Connected, EO3DConnectionState::Reconnecting,
		EO3DConnectionState::Connected, EO3DConnectionState::Failed, EO3DConnectionState::Idle };
	TestTrue(TEXT("Every change reported once, in order"), Log.States == Expected);
	TestTrue(TEXT("Every callback ran on the game thread"), Log.bAllOnGameThread);
	if (Log.Codes.Num() == Expected.Num())
	{
		TestTrue(TEXT("The Failed change carries ConnectFailed"), Log.Codes[4] == EO3DTransportError::ConnectFailed);
		TestTrue(TEXT("Connected carries no error"), Log.Codes[1] == EO3DTransportError::None);
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCA1ReceiverStateTest,
	"Open3DBroadcast.Transport.WebRTC.State.ReceiverTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCA1ReceiverStateTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	using namespace WebRTCA1Pr3Test;
	FFakeLiveKit Fake;
	FStateLog Log;
	{
		FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
		Receiver.SetStateChangedCallback(Log.MakeCallback());
		if (!TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeManualConfig()).IsOk()))
		{
			return false;
		}
		const FO3DTransportResult NoConsumer = Receiver.Start();
		TestTrue(TEXT("Start without a consumer returns NoConsumer"), NoConsumer.Code == EO3DTransportError::NoConsumer);
		TestTrue(TEXT("Still Idle"), Receiver.GetConnectionState() == EO3DConnectionState::Idle);

		Receiver.SetConsumer(MakeShared<FRecordingConsumer>());
		TestTrue(TEXT("Start"), Receiver.Start().IsOk());
		TestTrue(TEXT("Connecting after Start"), Receiver.GetConnectionState() == EO3DConnectionState::Connecting);
		FFakeClient* Client = Fake.LastClient();
		if (!TestNotNull(TEXT("Client created"), Client))
		{
			return false;
		}
		Client->FireConnection(LkConnConnected);
		Receiver.Poll();
		TestTrue(TEXT("Connected after Poll"), Receiver.GetConnectionState() == EO3DConnectionState::Connected);

		Client->FireConnection(LkConnReconnecting);
		Receiver.Poll();
		TestTrue(TEXT("Reconnecting while LiveKit reconnects"), Receiver.GetConnectionState() == EO3DConnectionState::Reconnecting);

		Receiver.Stop();
		TestTrue(TEXT("Idle after Stop"), Receiver.GetConnectionState() == EO3DConnectionState::Idle);
	}
	const TArray<EO3DConnectionState> Expected = {
		EO3DConnectionState::Connecting, EO3DConnectionState::Connected, EO3DConnectionState::Reconnecting, EO3DConnectionState::Idle };
	TestTrue(TEXT("Every change reported once, in order"), Log.States == Expected);
	TestTrue(TEXT("Every callback ran on the game thread"), Log.bAllOnGameThread);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCA1CapabilitiesTest,
	"Open3DBroadcast.Transport.WebRTC.Capabilities.DeliveryFollowsPreferLossy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCA1CapabilitiesTest::RunTest(const FString& Parameters)
{
	// The descriptor's query (no instance needed): ADR 0005 (iii) values.
	FO3DTransportConfig Reliable;
	FO3DTransportConfig Lossy;
	Lossy.AdvancedParams.Add(WebRTCUtils::PreferLossyOptionKey, TEXT("true"));
	const FO3DTransportCapabilities ReliableCaps = WebRTCUtils::GetCapabilities(Reliable);
	const FO3DTransportCapabilities LossyCaps = WebRTCUtils::GetCapabilities(Lossy);
	TestTrue(TEXT("Reliable data channel: ReliableOrdered"), ReliableCaps.Delivery == EO3DDeliveryGuarantee::ReliableOrdered);
	TestTrue(TEXT("webrtc.prefer_lossy: Unreliable"), LossyCaps.Delivery == EO3DDeliveryGuarantee::Unreliable);
	TestTrue(TEXT("Control, audio both ways, bidirectional"), ReliableCaps.bControl && ReliableCaps.bAudioSend && ReliableCaps.bAudioReceive && ReliableCaps.bBidirectional);
	TestFalse(TEXT("No new-peer signal yet (ADR 0005 Q4)"), ReliableCaps.bPeerJoinSignal);
	TestEqual(TEXT("Payload limit"), ReliableCaps.MaxPayloadBytes, WebRTCUtils::ReliableMaxDataBytes);

	FO3DTransportCapabilities Registered;
	if (FO3DTransportRegistry::Get().GetCapabilities(TEXT("WebRTC"), Lossy, Registered))
	{
		TestTrue(TEXT("The registered descriptor reports the same values"), Registered == LossyCaps);
	}

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCS7Test;
	FFakeLiveKit Fake;
	FO3DWebRTCSender Sender(FFakeLiveKit::MakeApi());
	FO3DTransportConfig Config = MakeManualConfig();
	Config.AdvancedParams.Add(WebRTCUtils::PreferLossyOptionKey, TEXT("true"));
	TestTrue(TEXT("Initialize"), Sender.Initialize(Config).IsOk());
	TestTrue(TEXT("The sender reports the descriptor's values for its config"), Sender.GetCapabilities() == LossyCaps);
	TestTrue(TEXT("SupportsControl forwards to bControl"), Sender.SupportsControl());
	TestTrue(TEXT("SupportsAudio forwards to bAudioSend"), Sender.SupportsAudio());
	Sender.Stop();

	FO3DWebRTCReceiver Receiver(FFakeLiveKit::MakeApi());
	TestTrue(TEXT("The receiver reports ReliableOrdered"), Receiver.GetCapabilities() == ReliableCaps);
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
