// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

//
// CTL-6: the control channel on WebRTC (docs/adr/0011-control-channel.md, items 4, 6 and 7).
//
// The sender and receiver run against a fake LiveKit function table (FLkFfiApi, ADR 0006 F2)
// whose lk_send_data_ex can relay each message to every other fake client's data callback, like
// a room. Nothing touches the network or livekit_ffi.dll.
//
// The control conformance cases (ControlRoundTrip, ControlRejectedWhenNotRunning,
// ControlStopWhileSending) live in Open3DBroadcastTests, which cannot reach this add-on: its
// WebRTC profile is deferred to an add-on test module (WP-T2e). The cases below cover the same
// contract for WebRTC until that module exists.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/WebRTCSender.h"
#include "../Receiver/WebRTCReceiver.h"
#include "../Shared/LiveKitFfiApi.h"
#include "../Shared/WebRTCUtils.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Containers/StringConv.h"

#include "O3DUnifiedMessage.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>
#include <string>

// Named namespace (not anonymous) so unity builds cannot collide with other test files.
namespace WebRTCCtl6Test
{
	/** One recorded lk_send_data_ex call. */
	struct FCtlSentMessage
	{
		std::string LabelUtf8;
		LkReliability Reliability = LkReliable;
		int32 Ordered = 0;
		TArray<uint8> Bytes;
	};

	/** State behind one fake LkClientHandle. Owned by FCtlFakeLiveKit, so it outlives lk_client_destroy. */
	struct FCtlFakeClient
	{
		LkConnectionCallback ConnectionCallback = nullptr;
		void* ConnectionUser = nullptr;
		LkDataCallbackEx DataCallbackEx = nullptr;
		void* DataUserEx = nullptr;
		LkDataCallback DataCallback = nullptr;
		void* DataUser = nullptr;
		TArray<LkRole> ConnectRoles;
		std::atomic<bool> bDestroyed{ false };

		void FireConnection(LkConnectionState State)
		{
			if (ConnectionCallback)
			{
				ConnectionCallback(ConnectionUser, State, 0, nullptr);
			}
		}

		/** Calls the registered data callback as LiveKit would (labelled, else the fallback). */
		void Deliver(const char* LabelUtf8, LkReliability Reliability, const uint8* Bytes, size_t Len)
		{
			if (DataCallbackEx)
			{
				DataCallbackEx(DataUserEx, LabelUtf8, Reliability, Bytes, Len);
			}
			else if (DataCallback)
			{
				DataCallback(DataUser, Bytes, Len);
			}
		}

		void Deliver(const char* LabelUtf8, const TArray<uint8>& Bytes)
		{
			Deliver(LabelUtf8, LkReliable, Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
		}
	};

	/**
	 * Fake LiveKit for the control tests. lk_client_create has no user-data argument, so the fake
	 * functions find the test's fake through a pointer that exists only while one test runs.
	 * Clients are created on the game thread before any send starts; Sends is guarded by Mutex
	 * because the stop-while-sending case sends from several threads.
	 */
	class FCtlFakeLiveKit
	{
	public:
		FCtlFakeLiveKit()
		{
			check(Active == nullptr);
			Active = this;
		}

		~FCtlFakeLiveKit()
		{
			Active = nullptr;
		}

		FCtlFakeLiveKit(const FCtlFakeLiveKit&) = delete;
		FCtlFakeLiveKit& operator=(const FCtlFakeLiveKit&) = delete;

		TArray<TUniquePtr<FCtlFakeClient>> Clients;
		int32 DataCallbackExResult = 0;
		std::atomic<int32> SendDataResult{ 0 };
		/** Relay each successful send to every other live client's data callback (a room). */
		bool bRelay = false;
		/** Keep each sent message; when false only the counters move. */
		bool bRecordSends = true;

		FCriticalSection Mutex;
		TArray<FCtlSentMessage> Sends;
		std::atomic<int32> SendCalls{ 0 };
		/** A send on a client that lk_client_destroy already released (must stay 0). */
		std::atomic<int32> SendsAfterDestroy{ 0 };

		FCtlFakeClient* LastClient() const
		{
			return Clients.Num() > 0 ? Clients.Last().Get() : nullptr;
		}

		TArray<FCtlSentMessage> GetSends()
		{
			FScopeLock Lock(&Mutex);
			return Sends;
		}

		static FLkFfiApi MakeApi();

		static FCtlFakeLiveKit* Active;
	};

	FCtlFakeLiveKit* FCtlFakeLiveKit::Active = nullptr;

	static LkResult CtlMakeResult(int32 Code)
	{
		LkResult Result;
		Result.code = Code;
		Result.message = nullptr;
		return Result;
	}

	static FCtlFakeClient* CtlAsClient(LkClientHandle* Handle)
	{
		return reinterpret_cast<FCtlFakeClient*>(Handle);
	}

	static void CtlFake_free_str(char*) {}

	static LkClientHandle* CtlFake_client_create()
	{
		if (!FCtlFakeLiveKit::Active)
		{
			return nullptr;
		}
		TUniquePtr<FCtlFakeClient> Client = MakeUnique<FCtlFakeClient>();
		FCtlFakeClient* Raw = Client.Get();
		FCtlFakeLiveKit::Active->Clients.Add(MoveTemp(Client));
		return reinterpret_cast<LkClientHandle*>(Raw);
	}

	static void CtlFake_client_destroy(LkClientHandle* Handle)
	{
		if (FCtlFakeClient* Client = CtlAsClient(Handle))
		{
			Client->bDestroyed.store(true);
		}
	}

	static LkResult CtlFake_set_data_callback(LkClientHandle* Handle, LkDataCallback Cb, void* User)
	{
		FCtlFakeClient* Client = CtlAsClient(Handle);
		Client->DataCallback = Cb;
		Client->DataUser = User;
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_data_callback_ex(LkClientHandle* Handle, LkDataCallbackEx Cb, void* User)
	{
		if (Cb && FCtlFakeLiveKit::Active && FCtlFakeLiveKit::Active->DataCallbackExResult != 0)
		{
			return CtlMakeResult(FCtlFakeLiveKit::Active->DataCallbackExResult);
		}
		FCtlFakeClient* Client = CtlAsClient(Handle);
		Client->DataCallbackEx = Cb;
		Client->DataUserEx = User;
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_audio_callback_ex(LkClientHandle*, LkAudioCallbackEx, void*)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_connection_callback(LkClientHandle* Handle, LkConnectionCallback Cb, void* User)
	{
		FCtlFakeClient* Client = CtlAsClient(Handle);
		Client->ConnectionCallback = Cb;
		Client->ConnectionUser = User;
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_connect_with_role_async(LkClientHandle* Handle, const char*, const char*, LkRole Role)
	{
		CtlAsClient(Handle)->ConnectRoles.Add(Role);
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_ok_client(LkClientHandle*)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_refresh_token(LkClientHandle*, const char*)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_audio_publish_options(LkClientHandle*, int32_t, int32_t, int32_t)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_audio_output_format(LkClientHandle*, int32_t, int32_t)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_audio_track_create(LkClientHandle*, const LkAudioTrackConfig*, LkAudioTrackHandle** OutTrack)
	{
		if (OutTrack)
		{
			*OutTrack = nullptr;
		}
		return CtlMakeResult(1); // audio is not used by these tests
	}

	static LkResult CtlFake_audio_track_destroy(LkAudioTrackHandle*)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_audio_track_publish_pcm_i16(LkAudioTrackHandle*, const int16_t*, size_t)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_send_data_ex(LkClientHandle* Handle, const uint8_t* Bytes, size_t Len, LkReliability Reliability, int32_t Ordered, const char* Label)
	{
		FCtlFakeLiveKit* Fake = FCtlFakeLiveKit::Active;
		FCtlFakeClient* Client = CtlAsClient(Handle);
		if (!Fake || !Client)
		{
			return CtlMakeResult(1);
		}
		Fake->SendCalls.fetch_add(1);
		if (Client->bDestroyed.load())
		{
			Fake->SendsAfterDestroy.fetch_add(1);
		}

		const int32 Code = Fake->SendDataResult.load();
		if (Code != 0)
		{
			return CtlMakeResult(Code);
		}

		if (Fake->bRecordSends)
		{
			FCtlSentMessage Message;
			Message.LabelUtf8 = Label ? std::string(Label) : std::string();
			Message.Reliability = Reliability;
			Message.Ordered = Ordered;
			Message.Bytes = TArray<uint8>(Bytes, static_cast<int32>(Len));
			FScopeLock Lock(&Fake->Mutex);
			Fake->Sends.Add(MoveTemp(Message));
		}

		if (Fake->bRelay)
		{
			for (const TUniquePtr<FCtlFakeClient>& Peer : Fake->Clients)
			{
				if (Peer.Get() != Client && !Peer->bDestroyed.load())
				{
					Peer->Deliver(Label, Reliability, Bytes, Len);
				}
			}
		}
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_default_data_labels(LkClientHandle*, const char*, const char*)
	{
		return CtlMakeResult(0);
	}

	static LkResult CtlFake_set_log_level(LkClientHandle*, LkLogLevel)
	{
		return CtlMakeResult(0);
	}

	FLkFfiApi FCtlFakeLiveKit::MakeApi()
	{
		FLkFfiApi Api;
		Api.lk_free_str = &CtlFake_free_str;
		Api.lk_client_create = &CtlFake_client_create;
		Api.lk_client_destroy = &CtlFake_client_destroy;
		Api.lk_client_set_data_callback = &CtlFake_set_data_callback;
		Api.lk_client_set_data_callback_ex = &CtlFake_set_data_callback_ex;
		Api.lk_client_set_audio_callback_ex = &CtlFake_set_audio_callback_ex;
		Api.lk_set_connection_callback = &CtlFake_set_connection_callback;
		Api.lk_connect_with_role_async = &CtlFake_connect_with_role_async;
		Api.lk_disconnect = &CtlFake_ok_client;
		Api.lk_refresh_token = &CtlFake_refresh_token;
		Api.lk_set_audio_publish_options = &CtlFake_set_audio_publish_options;
		Api.lk_set_audio_output_format = &CtlFake_set_audio_output_format;
		Api.lk_audio_track_create = &CtlFake_audio_track_create;
		Api.lk_audio_track_destroy = &CtlFake_audio_track_destroy;
		Api.lk_audio_track_publish_pcm_i16 = &CtlFake_audio_track_publish_pcm_i16;
		Api.lk_send_data_ex = &CtlFake_send_data_ex;
		Api.lk_set_default_data_labels = &CtlFake_set_default_data_labels;
		Api.lk_set_log_level = &CtlFake_set_log_level;
		return Api;
	}

	class FCtlRecordingConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InStreamId, const TArray<uint8>& InPayload, double) override
		{
			FScopeLock Lock(&Mutex);
			Labels.Add(InStreamId);
			Payloads.Add(InPayload);
		}

		int32 Num() const
		{
			FScopeLock Lock(&Mutex);
			return Payloads.Num();
		}

		mutable FCriticalSection Mutex;
		TArray<FString> Labels;
		TArray<TArray<uint8>> Payloads;
	};

	class FCtlRecordingControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double) override
		{
			FScopeLock Lock(&Mutex);
			Payloads.Emplace(Payload.GetData(), Payload.Num());
			StreamIds.Add(StreamId);
		}

		int32 Num() const
		{
			FScopeLock Lock(&Mutex);
			return Payloads.Num();
		}

		TArray<TArray<uint8>> Get() const
		{
			FScopeLock Lock(&Mutex);
			return Payloads;
		}

		mutable FCriticalSection Mutex;
		TArray<TArray<uint8>> Payloads;
		TArray<FString> StreamIds;
	};

	using FCtlSinkRef = TSharedRef<FCtlRecordingControlSink, ESPMode::ThreadSafe>;

	/** A JWT-shaped token whose payload is {"exp":<one hour from now>}; the signature is not checked. */
	static FString CtlMakeTestJwt()
	{
		const FString Payload = FString::Printf(TEXT("{\"exp\":%lld}"), FDateTime::UtcNow().ToUnixTimestamp() + 3600);
		const FTCHARToUTF8 PayloadUtf8(*Payload);
		const FString PayloadBase64 = FBase64::Encode(reinterpret_cast<const uint8*>(PayloadUtf8.Get()), static_cast<uint32>(PayloadUtf8.Length()));
		return FString(TEXT("eyJhbGciOiJIUzI1NiJ9.")) + PayloadBase64 + TEXT(".c2ln");
	}

	static FO3DTransportConfig CtlMakeConfig()
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("127.0.0.1:7880"); // only the fake sees it
		Config.Secrets.Add(WebRTCUtils::TokenOptionKey, CtlMakeTestJwt());
		Config.StreamId = TEXT("ctl6-stream");
		// Disable the receiver's no-data watchdog so tests stay deterministic.
		Config.AdvancedParams.Add(TEXT("webrtc.reconnect_timeout"), TEXT("0"));
		return Config;
	}

	/** A control payload whose bytes identify it. */
	static TArray<uint8> CtlMakePayload(int32 Index, int32 Size)
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(Size);
		for (int32 Byte = 0; Byte < Size; ++Byte)
		{
			Payload[Byte] = static_cast<uint8>((Index * 131 + Byte * 7 + 3) & 0xFF);
		}
		return Payload;
	}

	static TArray<uint8> CtlMakeEnvelope(const TArray<uint8>& Payload)
	{
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);
		return Envelope;
	}

	/** Bytes the transport treats as mocap: they do not start with the envelope magic. */
	static TArray<uint8> CtlMakeMocapBytes(int32 Index)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(48);
		for (int32 Byte = 0; Byte < Bytes.Num(); ++Byte)
		{
			Bytes[Byte] = static_cast<uint8>(Index + Byte);
		}
		Bytes[0] = 1; // like a SubjectList, whose first word is 1
		return Bytes;
	}

	/** Initializes, starts and "connects" a sender on the fake. Returns its fake client. */
	static FCtlFakeClient* CtlStartSender(FAutomationTestBase& Test, FCtlFakeLiveKit& Fake, FO3DWebRTCSender& Sender)
	{
		if (!Test.TestTrue(TEXT("Sender Initialize"), Sender.Initialize(CtlMakeConfig()).IsOk()))
		{
			return nullptr;
		}
		FCtlFakeClient* Client = Fake.LastClient();
		if (!Test.TestNotNull(TEXT("Sender client created"), Client) || !Test.TestTrue(TEXT("Sender Start"), Sender.Start().IsOk()))
		{
			return nullptr;
		}
		Client->FireConnection(LkConnConnected);
		return Client;
	}

	/** Initializes, starts and "connects" a receiver on the fake, with an optional control sink. */
	static FCtlFakeClient* CtlStartReceiver(FAutomationTestBase& Test, FCtlFakeLiveKit& Fake, FO3DWebRTCReceiver& Receiver,
		const TSharedPtr<ISerializedFrameConsumer>& Consumer, const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink)
	{
		if (!Test.TestTrue(TEXT("Receiver Initialize"), Receiver.Initialize(CtlMakeConfig()).IsOk()))
		{
			return nullptr;
		}
		FCtlFakeClient* Client = Fake.LastClient();
		Receiver.SetConsumer(Consumer);
		if (Sink.IsValid())
		{
			Receiver.SetControlSink(Sink); // before Start, as the interface requires
		}
		if (!Test.TestNotNull(TEXT("Receiver client created"), Client) || !Test.TestTrue(TEXT("Receiver Start"), Receiver.Start().IsOk()))
		{
			return nullptr;
		}
		Client->FireConnection(LkConnConnected);
		return Client;
	}
}

// ---------------------------------------------------------------------------------------------
// Round trip: control on `__o3d.ctl`, reliable and ordered, interleaved with mocap
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCCtl6RoundTripTest,
	"Open3DBroadcast.Transport.WebRTC.Control.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCCtl6RoundTripTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCCtl6Test;
	FCtlFakeLiveKit Fake;
	Fake.bRelay = true;

	const TSharedRef<FCtlRecordingConsumer> Consumer = MakeShared<FCtlRecordingConsumer>();
	const FCtlSinkRef Sink = MakeShared<FCtlRecordingControlSink, ESPMode::ThreadSafe>();
	{
		FO3DWebRTCReceiver Receiver(FCtlFakeLiveKit::MakeApi());
		FO3DWebRTCSender Sender(FCtlFakeLiveKit::MakeApi());
		TestTrue(TEXT("Receiver supports control"), Receiver.SupportsControl());
		TestTrue(TEXT("Sender supports control"), Sender.SupportsControl());

		if (!CtlStartReceiver(*this, Fake, Receiver, Consumer, Sink))
		{
			return false;
		}
		FCtlFakeClient* SenderClient = CtlStartSender(*this, Fake, Sender);
		if (!SenderClient)
		{
			return false;
		}
		if (SenderClient->ConnectRoles.Num() == 1)
		{
			// The sender always joins as a publisher, so it is never subscriber-only (ADR 0011 item 7).
			TestEqual(TEXT("Sender joins as a publisher"), static_cast<int32>(SenderClient->ConnectRoles[0]), static_cast<int32>(LkRolePublisher));
		}

		// Interleave: mocap, control, mocap, control ... The largest payload the envelope allows is included.
		constexpr int32 NumControl = 3;
		const int32 PayloadSizes[NumControl] = { 16, 300, O3DS::UnifiedMaxControlPayloadSize };
		TArray<TArray<uint8>> SentControl;
		int64 MocapBytes = 0;
		int32 MocapFrames = 0;
		for (int32 Index = 0; Index < NumControl; ++Index)
		{
			const TArray<uint8> Frame = CtlMakeMocapBytes(Index);
			TestTrue(*FString::Printf(TEXT("Mocap %d sent"), Index), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Alice"), 0.0)) == EO3DSendResult::Queued);
			MocapBytes += Frame.Num();
			++MocapFrames;

			const TArray<uint8> Payload = CtlMakePayload(Index, PayloadSizes[Index]);
			const TArray<uint8> Envelope = CtlMakeEnvelope(Payload);
			TestTrue(*FString::Printf(TEXT("Control %d accepted"), Index), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);
			SentControl.Add(Payload);
		}

		// What went on the wire.
		const TArray<FCtlSentMessage> Sends = Fake.GetSends();
		int32 ControlSends = 0;
		for (const FCtlSentMessage& Message : Sends)
		{
			const bool bControl = WebRTCUtils::IsControlKindEnvelope(Message.Bytes.GetData(), static_cast<size_t>(Message.Bytes.Num()));
			if (bControl)
			{
				++ControlSends;
				TestTrue(TEXT("Control uses the __o3d.ctl label"), Message.LabelUtf8 == std::string(WebRTCUtils::ControlDataLabelUtf8));
				TestEqual(TEXT("Control is reliable"), static_cast<int32>(Message.Reliability), static_cast<int32>(LkReliable));
				TestEqual(TEXT("Control is ordered"), Message.Ordered, 1);
				TestTrue(TEXT("Control is within the 1,100-byte budget"), Message.Bytes.Num() <= WebRTCUtils::MaxControlEnvelopeBytes);
			}
			else
			{
				TestTrue(TEXT("Mocap keeps the subject label"), Message.LabelUtf8 == std::string("Alice"));
			}
		}
		TestEqual(TEXT("Every control envelope sent once"), ControlSends, SentControl.Num());

		const FO3DTransportStats SenderStats = Sender.GetStats();
		TestEqual(TEXT("Control is not counted as a sent frame"), SenderStats.FramesSent, static_cast<int64>(MocapFrames));
		TestEqual(TEXT("Control is not counted as sent bytes"), SenderStats.BytesSent, MocapBytes);
		TestEqual(TEXT("No dropped frames"), SenderStats.DroppedFrames, static_cast<int64>(0));

		// Delivery happens on Poll (the game-thread hand-off).
		TestEqual(TEXT("Nothing reaches the sink before Poll"), Sink->Num(), 0);
		const int32 Processed = Receiver.Poll();
		TestEqual(TEXT("Poll counts mocap frames only"), Processed, MocapFrames);

		const TArray<TArray<uint8>> Received = Sink->Get();
		TestEqual(TEXT("Every control payload arrived exactly once"), Received.Num(), SentControl.Num());
		bool bExact = Received.Num() == SentControl.Num();
		for (int32 Index = 0; bExact && Index < Received.Num(); ++Index)
		{
			bExact = Received[Index] == SentControl[Index];
		}
		TestTrue(TEXT("Control payloads byte-exact and in order"), bExact);
		for (const FString& StreamId : Sink->StreamIds)
		{
			TestEqual(TEXT("Control carries the receiver's stream id"), StreamId, FString(TEXT("ctl6-stream")));
		}

		TestEqual(TEXT("Only mocap reaches the frame consumer"), Consumer->Num(), MocapFrames);
		for (const FString& Label : Consumer->Labels)
		{
			TestEqual(TEXT("Mocap subject is the data label"), Label, FString(TEXT("Alice")));
		}
		const FO3DTransportStats ReceiverStats = Receiver.GetStats();
		TestEqual(TEXT("Receiver counts mocap frames only"), ReceiverStats.FramesReceived, static_cast<int64>(MocapFrames));
		TestEqual(TEXT("Receiver counts mocap bytes only"), ReceiverStats.BytesReceived, MocapBytes);

		Sender.Stop();
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Receive classification: by envelope, before the "label = subject" mocap path
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCCtl6ClassificationTest,
	"Open3DBroadcast.Transport.WebRTC.Control.LabelClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCCtl6ClassificationTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCCtl6Test;
	FCtlFakeLiveKit Fake;

	const TArray<uint8> ControlPayload = CtlMakePayload(7, 40);
	const TArray<uint8> ControlEnvelope = CtlMakeEnvelope(ControlPayload);
	const TArray<uint8> Mocap = CtlMakeMocapBytes(3);

	{
		const TSharedRef<FCtlRecordingConsumer> Consumer = MakeShared<FCtlRecordingConsumer>();
		const FCtlSinkRef Sink = MakeShared<FCtlRecordingControlSink, ESPMode::ThreadSafe>();
		FO3DWebRTCReceiver Receiver(FCtlFakeLiveKit::MakeApi());
		FCtlFakeClient* Client = CtlStartReceiver(*this, Fake, Receiver, Consumer, Sink);
		if (!Client)
		{
			return false;
		}

		// 1. A control envelope on a subject's label is control, not that subject's mocap.
		Client->Deliver("Alice", ControlEnvelope);
		// 2. Plain mocap on the control label stays mocap (ADR 0011 Verification, WebRTC).
		Client->Deliver(WebRTCUtils::ControlDataLabelUtf8, Mocap);
		// 3. A control envelope on the control label, lossy, from an FFI (background) thread.
		Async(EAsyncExecution::Thread, [Client, &ControlEnvelope]()
		{
			Client->Deliver(WebRTCUtils::ControlDataLabelUtf8, LkLossy, ControlEnvelope.GetData(), static_cast<size_t>(ControlEnvelope.Num()));
		}).Wait();
		// 4. Kind Control with the wrong codec is malformed: dropped, never mocap.
		TArray<uint8> WrongCodec;
		O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::O3DS, ControlPayload.GetData(), ControlPayload.Num(), 1.0, WrongCodec);
		Client->Deliver("Bob", WrongCodec);
		// 5. Kind Control whose payload is over the budget: dropped, never mocap.
		const TArray<uint8> BigPayload = CtlMakePayload(9, O3DS::UnifiedMaxControlPayloadSize + 1);
		TArray<uint8> Oversize;
		O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::O3DControl, BigPayload.GetData(), BigPayload.Num(), 1.0, Oversize);
		Client->Deliver(WebRTCUtils::ControlDataLabelUtf8, Oversize);
		// 6. A well-formed envelope followed by trailing bytes: only the envelope is delivered.
		TArray<uint8> Trailing = ControlEnvelope;
		Trailing.Add(0xAB);
		Client->Deliver("Carol", Trailing);

		const int32 Processed = Receiver.Poll();
		TestEqual(TEXT("Poll counts the one mocap frame"), Processed, 1);
		TestEqual(TEXT("One mocap frame reached the consumer"), Consumer->Num(), 1);
		if (Consumer->Num() == 1)
		{
			TestEqual(TEXT("Mocap on the control label keeps that label as its subject"), Consumer->Labels[0], FString(UTF8_TO_TCHAR(WebRTCUtils::ControlDataLabelUtf8)));
			TestTrue(TEXT("Mocap bytes unchanged"), Consumer->Payloads[0] == Mocap);
		}

		const TArray<TArray<uint8>> Received = Sink->Get();
		TestEqual(TEXT("Three control payloads reached the sink"), Received.Num(), 3);
		for (const TArray<uint8>& Payload : Received)
		{
			TestTrue(TEXT("Control payload byte-exact"), Payload == ControlPayload);
		}
		TestEqual(TEXT("Control is not counted as a received frame"), Receiver.GetStats().FramesReceived, static_cast<int64>(1));
		TestEqual(TEXT("Control is not counted as a dropped frame"), Receiver.GetStats().DroppedFrames, static_cast<int64>(0));
		Receiver.Stop();
	}

	// Without a control sink, control is dropped, still never handed to the consumer.
	{
		const TSharedRef<FCtlRecordingConsumer> Consumer = MakeShared<FCtlRecordingConsumer>();
		FO3DWebRTCReceiver Receiver(FCtlFakeLiveKit::MakeApi());
		FCtlFakeClient* Client = CtlStartReceiver(*this, Fake, Receiver, Consumer, nullptr);
		if (!Client)
		{
			return false;
		}
		Client->Deliver(WebRTCUtils::ControlDataLabelUtf8, ControlEnvelope);
		Client->Deliver("Alice", ControlEnvelope);
		TestEqual(TEXT("No sink: Poll processes no frame"), Receiver.Poll(), 0);
		TestEqual(TEXT("No sink: control never reaches the consumer"), Consumer->Num(), 0);
		Receiver.Stop();
	}

	// The unlabeled fallback callback classifies the same way.
	Fake.DataCallbackExResult = 401;
	{
		const TSharedRef<FCtlRecordingConsumer> Consumer = MakeShared<FCtlRecordingConsumer>();
		const FCtlSinkRef Sink = MakeShared<FCtlRecordingControlSink, ESPMode::ThreadSafe>();
		FO3DWebRTCReceiver Receiver(FCtlFakeLiveKit::MakeApi());
		FCtlFakeClient* Client = CtlStartReceiver(*this, Fake, Receiver, Consumer, Sink);
		if (!Client)
		{
			return false;
		}
		TestTrue(TEXT("Fallback callback registered"), Client->DataCallbackEx == nullptr && Client->DataCallback != nullptr);
		Client->Deliver(nullptr, ControlEnvelope);
		Client->Deliver(nullptr, Mocap);
		Receiver.Poll();
		TestEqual(TEXT("Fallback: control reaches the sink"), Sink->Num(), 1);
		TestEqual(TEXT("Fallback: mocap reaches the consumer"), Consumer->Num(), 1);
		Receiver.Stop();
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Sender refusals: oversize, not an envelope, not running, FFI failure
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCCtl6SenderRefusalsTest,
	"Open3DBroadcast.Transport.WebRTC.Control.SenderRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCCtl6SenderRefusalsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCCtl6Test;
	FCtlFakeLiveKit Fake;

	const TArray<uint8> Largest = CtlMakeEnvelope(CtlMakePayload(1, O3DS::UnifiedMaxControlPayloadSize));
	TestTrue(TEXT("The largest control envelope fits the budget"), Largest.Num() > 0 && Largest.Num() <= WebRTCUtils::MaxControlEnvelopeBytes);

	FO3DWebRTCSender Sender(FCtlFakeLiveKit::MakeApi());
	TestTrue(TEXT("SendControl before Initialize returns NotRunning"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::NotRunning);
	if (!TestTrue(TEXT("Initialize"), Sender.Initialize(CtlMakeConfig()).IsOk()))
	{
		return false;
	}
	FCtlFakeClient* Client = Fake.LastClient();
	TestTrue(TEXT("SendControl before Start returns NotRunning"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::NotRunning);
	TestTrue(TEXT("Start"), Sender.Start().IsOk());
	TestTrue(TEXT("SendControl before LiveKit connects returns NotConnected"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::NotConnected);
	if (!TestNotNull(TEXT("Client created"), Client))
	{
		return false;
	}
	Client->FireConnection(LkConnConnected);

	TestTrue(TEXT("The largest control envelope is sent"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::Queued);

	// Oversize: kind Control with one payload byte over the limit, and a payload far over it.
	const TArray<uint8> OneOver = CtlMakePayload(2, O3DS::UnifiedMaxControlPayloadSize + 1);
	TArray<uint8> OneOverEnvelope;
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::O3DControl, OneOver.GetData(), OneOver.Num(), 1.0, OneOverEnvelope);
	TestTrue(TEXT("A payload one byte over the limit is not a control envelope (Invalid)"), Sender.SendControl(OneOverEnvelope.GetData(), OneOverEnvelope.Num()) == EO3DSendResult::Invalid);
	const TArray<uint8> Huge = CtlMakePayload(3, 4000);
	TArray<uint8> HugeEnvelope;
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::O3DControl, Huge.GetData(), Huge.Num(), 1.0, HugeEnvelope);
	TestTrue(TEXT("An envelope over 1,100 bytes is refused (Invalid)"), Sender.SendControl(HugeEnvelope.GetData(), HugeEnvelope.Num()) == EO3DSendResult::Invalid);
	TestFalse(TEXT("WriteControlEnvelope refuses it too"), O3DS::WriteControlEnvelope(OneOver, 1.0, OneOverEnvelope));

	// Not exactly one control envelope.
	TArray<uint8> Trailing = Largest;
	Trailing.Add(0);
	TestTrue(TEXT("Trailing bytes are Invalid"), Sender.SendControl(Trailing.GetData(), Trailing.Num()) == EO3DSendResult::Invalid);
	const TArray<uint8> Mocap = CtlMakeMocapBytes(0);
	TestTrue(TEXT("Bytes that are not a control envelope are Invalid"), Sender.SendControl(Mocap.GetData(), Mocap.Num()) == EO3DSendResult::Invalid);
	TestTrue(TEXT("Null is Invalid"), Sender.SendControl(nullptr, 0) == EO3DSendResult::Invalid);

	TestEqual(TEXT("Only the accepted envelope reached LiveKit"), Fake.GetSends().Num(), 1);

	// An FFI failure is refused (the publisher retries) and logged once per throttle interval.
	AddExpectedError(TEXT("Failed to send a control envelope"), EAutomationExpectedMessageFlags::Contains, 1);
	Fake.SendDataResult.store(7);
	TestTrue(TEXT("An lk_send_data_ex failure is refused (DroppedBackpressure)"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::DroppedBackpressure);
	TestTrue(TEXT("A second failure is refused"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::DroppedBackpressure);
	Fake.SendDataResult.store(0);

	const FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("No frame counted as sent"), Stats.FramesSent, static_cast<int64>(0));
	TestEqual(TEXT("No byte counted as sent"), Stats.BytesSent, static_cast<int64>(0));
	TestEqual(TEXT("No frame counted as dropped"), Stats.DroppedFrames, static_cast<int64>(0));

	Client->FireConnection(LkConnReconnecting);
	TestTrue(TEXT("SendControl while LiveKit reconnects returns NotConnected"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::NotConnected);
	Client->FireConnection(LkConnConnected);

	Sender.Stop();
	TestTrue(TEXT("SendControl after Stop returns NotRunning"), Sender.SendControl(Largest.GetData(), Largest.Num()) == EO3DSendResult::NotRunning);
	TestEqual(TEXT("No send after the client was destroyed"), Fake.SendsAfterDestroy.load(), 0);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Receiver: the sink is held strongly and released in Stop
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCCtl6SinkReleasedOnStopTest,
	"Open3DBroadcast.Transport.WebRTC.Control.SinkReleasedOnStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCCtl6SinkReleasedOnStopTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCCtl6Test;
	FCtlFakeLiveKit Fake;
	const TArray<uint8> ControlEnvelope = CtlMakeEnvelope(CtlMakePayload(4, 64));

	FO3DWebRTCReceiver Receiver(FCtlFakeLiveKit::MakeApi());
	TWeakPtr<FCtlRecordingControlSink, ESPMode::ThreadSafe> WeakSink;
	FCtlFakeClient* Client = nullptr;
	{
		TSharedPtr<FCtlRecordingControlSink, ESPMode::ThreadSafe> Sink = MakeShared<FCtlRecordingControlSink, ESPMode::ThreadSafe>();
		WeakSink = Sink;
		Client = CtlStartReceiver(*this, Fake, Receiver, MakeShared<FCtlRecordingConsumer>(), Sink);
		if (!Client)
		{
			return false;
		}
	}
	TestTrue(TEXT("The receiver holds the sink strongly"), WeakSink.IsValid());

	// A message queued before Stop and a callback that arrives after it are both discarded.
	const LkDataCallbackEx Callback = Client->DataCallbackEx;
	void* const CallbackUser = Client->DataUserEx;
	Client->Deliver(WebRTCUtils::ControlDataLabelUtf8, ControlEnvelope);

	Receiver.Stop();
	TestFalse(TEXT("Stop released the sink"), WeakSink.IsValid());
	TestTrue(TEXT("Stop cleared the data callback"), Client->DataCallbackEx == nullptr);

	if (Callback)
	{
		Callback(CallbackUser, WebRTCUtils::ControlDataLabelUtf8, LkReliable, ControlEnvelope.GetData(), static_cast<size_t>(ControlEnvelope.Num()));
	}
	TestEqual(TEXT("Poll after Stop processes nothing"), Receiver.Poll(), 0);

	// SetControlSink(nullptr) releases the sink too.
	{
		const FCtlSinkRef Sink = MakeShared<FCtlRecordingControlSink, ESPMode::ThreadSafe>();
		WeakSink = Sink;
		Receiver.SetControlSink(Sink);
	}
	TestTrue(TEXT("Held again"), WeakSink.IsValid());
	Receiver.SetControlSink(nullptr);
	TestFalse(TEXT("SetControlSink(nullptr) released the sink"), WeakSink.IsValid());
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Stop while several threads call SendControl
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCCtl6StopWhileSendingTest,
	"Open3DBroadcast.Transport.WebRTC.Control.StopWhileSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCCtl6StopWhileSendingTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCCtl6Test;
	constexpr int32 NumThreads = 4;
	constexpr int32 MaxCallsPerThread = 20000;
	constexpr int32 Cycles = 20;

	FCtlFakeLiveKit Fake;
	Fake.bRecordSends = false;
	const TArray<uint8> Envelope = CtlMakeEnvelope(CtlMakePayload(5, 200));

	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		FO3DWebRTCSender Sender(FCtlFakeLiveKit::MakeApi());
		if (!CtlStartSender(*this, Fake, Sender))
		{
			return false;
		}

		std::atomic<bool> bStop{ false };
		std::atomic<int32> Calls{ 0 };
		std::atomic<int32> Accepted{ 0 };
		TArray<TFuture<void>> Workers;
		for (int32 WorkerIndex = 0; WorkerIndex < NumThreads; ++WorkerIndex)
		{
			Workers.Add(Async(EAsyncExecution::Thread, [&Sender, &Envelope, &bStop, &Calls, &Accepted]()
			{
				for (int32 Call = 0; Call < MaxCallsPerThread && !bStop.load(); ++Call)
				{
					if (Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued)
					{
						Accepted.fetch_add(1);
					}
					Calls.fetch_add(1);
				}
			}));
		}

		// Stop once every thread is likely sending, so Stop overlaps the calls.
		const double WaitStart = FPlatformTime::Seconds();
		while (Calls.load() < NumThreads * 10 && FPlatformTime::Seconds() - WaitStart < 5.0)
		{
			FPlatformProcess::YieldThread();
		}
		const double StopStart = FPlatformTime::Seconds();
		Sender.Stop();
		TestTrue(TEXT("Stop returns promptly while control sends are in flight"), FPlatformTime::Seconds() - StopStart < 5.0);

		bStop.store(true);
		for (TFuture<void>& Worker : Workers)
		{
			Worker.Wait();
		}
		TestTrue(TEXT("The send threads made calls"), Calls.load() > 0);
		TestTrue(TEXT("SendControl after Stop returns NotRunning"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotRunning);
		TestEqual(TEXT("Control is never counted as a frame"), Sender.GetStats().FramesSent, static_cast<int64>(0));
	}
	TestEqual(TEXT("No send reached a destroyed client"), Fake.SendsAfterDestroy.load(), 0);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
