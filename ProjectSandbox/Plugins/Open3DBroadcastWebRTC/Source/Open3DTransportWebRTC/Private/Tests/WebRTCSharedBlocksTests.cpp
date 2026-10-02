// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

//
// WP-A1 PR 4f: the WebRTC add-on on the shared transport blocks (docs/adr/0007, addendum PR 4f).
//
// The receiver hands data-channel frames and control from LiveKit's threads to Poll through
// FO3DSendQueue (frames: RefuseNewest, 16 MiB) and delivers them through FO3DUnifiedReceiveDemux;
// audio goes from LiveKit's audio callback straight to the audio sink. The sender keeps its
// synchronous lk_send_data_ex path and its gated audio sink.
//
// Everything runs against a fake LiveKit function table (FLkFfiApi, ADR 0006 F2); nothing touches
// the network or livekit_ffi.dll. Like the real library, the fake makes no callback after the
// call that clears it, or lk_client_destroy, has returned (livekit_ffi.h), so the Stop tests below
// exercise the same contract the add-on relies on.
//

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

#include "O3DAudioFrameCodec.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

// Named namespace (not anonymous) so unity builds cannot collide with other test files.
namespace WebRTCSharedBlocksTest
{
	/** A fake LkAudioTrackHandle. Owned by FSbtFakeLiveKit, so it outlives lk_audio_track_destroy. */
	struct FSbtFakeTrack
	{
		FString Name;
		std::atomic<bool> bDestroyed{ false };
	};

	/**
	 * State behind one fake LkClientHandle; owned by FSbtFakeLiveKit, so it outlives
	 * lk_client_destroy. CallbackMutex is held while a callback runs and while one is set or
	 * cleared, so a callback never runs after the call that cleared it has returned (the
	 * livekit_ffi.h contract). Callbacks never call back into the fake, so this cannot deadlock.
	 */
	struct FSbtFakeClient
	{
		FCriticalSection CallbackMutex;
		LkConnectionCallback ConnectionCallback = nullptr;
		void* ConnectionUser = nullptr;
		LkDataCallbackEx DataCallbackEx = nullptr;
		void* DataUserEx = nullptr;
		LkAudioCallbackEx AudioCallbackEx = nullptr;
		void* AudioUserEx = nullptr;
		std::atomic<bool> bDestroyed{ false };

		void FireConnection(LkConnectionState State)
		{
			FScopeLock Lock(&CallbackMutex);
			if (ConnectionCallback)
			{
				ConnectionCallback(ConnectionUser, State, 0, nullptr);
			}
		}

		/** Calls the labelled data callback as LiveKit would. False when none is registered. */
		bool DeliverData(const char* LabelUtf8, const uint8* Bytes, size_t Len)
		{
			FScopeLock Lock(&CallbackMutex);
			if (!DataCallbackEx || bDestroyed.load())
			{
				return false;
			}
			DataCallbackEx(DataUserEx, LabelUtf8, LkReliable, Bytes, Len);
			return true;
		}

		/** Calls the audio callback as LiveKit would after decoding Opus. False when none is registered. */
		bool DeliverAudio(const char* TrackNameUtf8, const int16* Pcm, size_t FramesPerChannel, int32 Channels, int32 SampleRate)
		{
			FScopeLock Lock(&CallbackMutex);
			if (!AudioCallbackEx || bDestroyed.load())
			{
				return false;
			}
			AudioCallbackEx(AudioUserEx, Pcm, FramesPerChannel, Channels, SampleRate, "sbt-participant", TrackNameUtf8);
			return true;
		}
	};

	/**
	 * Fake LiveKit for these tests. lk_client_create has no user-data argument, so the fake
	 * functions find the test's fake through a pointer that exists only while one test runs.
	 * Clients and tracks are created under Mutex because audio tracks are created on send threads.
	 */
	class FSbtFakeLiveKit
	{
	public:
		FSbtFakeLiveKit()
		{
			check(Active == nullptr);
			Active = this;
		}

		~FSbtFakeLiveKit()
		{
			Active = nullptr;
		}

		FSbtFakeLiveKit(const FSbtFakeLiveKit&) = delete;
		FSbtFakeLiveKit& operator=(const FSbtFakeLiveKit&) = delete;

		FCriticalSection Mutex;
		TArray<TUniquePtr<FSbtFakeClient>> Clients;
		TArray<TUniquePtr<FSbtFakeTrack>> Tracks;

		/** lk_send_data_ex result; non-zero is LiveKit refusing the message (its buffer is full). */
		std::atomic<int32> SendDataResult{ 0 };
		std::atomic<int32> SendCalls{ 0 };
		std::atomic<int32> AudioPublishCalls{ 0 };
		/** A send on a destroyed client, or a publish on a destroyed track or client (must stay 0). */
		std::atomic<int32> CallsAfterDestroy{ 0 };

		FSbtFakeClient* LastClient()
		{
			FScopeLock Lock(&Mutex);
			return Clients.Num() > 0 ? Clients.Last().Get() : nullptr;
		}

		int32 NumTracks()
		{
			FScopeLock Lock(&Mutex);
			return Tracks.Num();
		}

		static FLkFfiApi MakeApi();

		static FSbtFakeLiveKit* Active;
	};

	FSbtFakeLiveKit* FSbtFakeLiveKit::Active = nullptr;

	static LkResult SbtMakeResult(int32 Code)
	{
		LkResult Result;
		Result.code = Code;
		Result.message = nullptr;
		return Result;
	}

	static FSbtFakeClient* SbtAsClient(LkClientHandle* Handle)
	{
		return reinterpret_cast<FSbtFakeClient*>(Handle);
	}

	static void SbtFake_free_str(char*) {}

	static LkClientHandle* SbtFake_client_create()
	{
		FSbtFakeLiveKit* Fake = FSbtFakeLiveKit::Active;
		if (!Fake)
		{
			return nullptr;
		}
		TUniquePtr<FSbtFakeClient> Client = MakeUnique<FSbtFakeClient>();
		FSbtFakeClient* Raw = Client.Get();
		FScopeLock Lock(&Fake->Mutex);
		Fake->Clients.Add(MoveTemp(Client));
		return reinterpret_cast<LkClientHandle*>(Raw);
	}

	static void SbtFake_client_destroy(LkClientHandle* Handle)
	{
		if (FSbtFakeClient* Client = SbtAsClient(Handle))
		{
			// Waits for a running callback, like the real library.
			FScopeLock Lock(&Client->CallbackMutex);
			Client->bDestroyed.store(true);
			Client->ConnectionCallback = nullptr;
			Client->DataCallbackEx = nullptr;
			Client->AudioCallbackEx = nullptr;
		}
	}

	static LkResult SbtFake_set_data_callback(LkClientHandle*, LkDataCallback, void*)
	{
		return SbtMakeResult(0); // the labelled callback is always accepted, so this one stays unused
	}

	static LkResult SbtFake_set_data_callback_ex(LkClientHandle* Handle, LkDataCallbackEx Cb, void* User)
	{
		FSbtFakeClient* Client = SbtAsClient(Handle);
		FScopeLock Lock(&Client->CallbackMutex);
		Client->DataCallbackEx = Cb;
		Client->DataUserEx = User;
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_set_audio_callback_ex(LkClientHandle* Handle, LkAudioCallbackEx Cb, void* User)
	{
		FSbtFakeClient* Client = SbtAsClient(Handle);
		FScopeLock Lock(&Client->CallbackMutex);
		Client->AudioCallbackEx = Cb;
		Client->AudioUserEx = User;
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_set_connection_callback(LkClientHandle* Handle, LkConnectionCallback Cb, void* User)
	{
		FSbtFakeClient* Client = SbtAsClient(Handle);
		FScopeLock Lock(&Client->CallbackMutex);
		Client->ConnectionCallback = Cb;
		Client->ConnectionUser = User;
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_connect_with_role_async(LkClientHandle*, const char*, const char*, LkRole)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_ok_client(LkClientHandle*)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_refresh_token(LkClientHandle*, const char*)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_set_audio_publish_options(LkClientHandle*, int32_t, int32_t, int32_t)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_set_audio_output_format(LkClientHandle*, int32_t, int32_t)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_audio_track_create(LkClientHandle* Handle, const LkAudioTrackConfig* Config, LkAudioTrackHandle** OutTrack)
	{
		FSbtFakeLiveKit* Fake = FSbtFakeLiveKit::Active;
		if (!Fake || !OutTrack || !Config)
		{
			return SbtMakeResult(1);
		}
		if (SbtAsClient(Handle)->bDestroyed.load())
		{
			Fake->CallsAfterDestroy.fetch_add(1);
		}
		TUniquePtr<FSbtFakeTrack> Track = MakeUnique<FSbtFakeTrack>();
		Track->Name = UTF8_TO_TCHAR(Config->track_name ? Config->track_name : "");
		*OutTrack = reinterpret_cast<LkAudioTrackHandle*>(Track.Get());
		FScopeLock Lock(&Fake->Mutex);
		Fake->Tracks.Add(MoveTemp(Track));
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_audio_track_destroy(LkAudioTrackHandle* Handle)
	{
		if (Handle)
		{
			reinterpret_cast<FSbtFakeTrack*>(Handle)->bDestroyed.store(true);
		}
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_audio_track_publish_pcm_i16(LkAudioTrackHandle* Handle, const int16_t*, size_t)
	{
		FSbtFakeLiveKit* Fake = FSbtFakeLiveKit::Active;
		if (!Fake || !Handle)
		{
			return SbtMakeResult(1);
		}
		if (reinterpret_cast<FSbtFakeTrack*>(Handle)->bDestroyed.load())
		{
			Fake->CallsAfterDestroy.fetch_add(1);
		}
		Fake->AudioPublishCalls.fetch_add(1);
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_send_data_ex(LkClientHandle* Handle, const uint8_t*, size_t, LkReliability, int32_t, const char*)
	{
		FSbtFakeLiveKit* Fake = FSbtFakeLiveKit::Active;
		FSbtFakeClient* Client = SbtAsClient(Handle);
		if (!Fake || !Client)
		{
			return SbtMakeResult(1);
		}
		Fake->SendCalls.fetch_add(1);
		if (Client->bDestroyed.load())
		{
			Fake->CallsAfterDestroy.fetch_add(1);
		}
		return SbtMakeResult(Fake->SendDataResult.load());
	}

	static LkResult SbtFake_set_default_data_labels(LkClientHandle*, const char*, const char*)
	{
		return SbtMakeResult(0);
	}

	static LkResult SbtFake_set_log_level(LkClientHandle*, LkLogLevel)
	{
		return SbtMakeResult(0);
	}

	FLkFfiApi FSbtFakeLiveKit::MakeApi()
	{
		FLkFfiApi Api;
		Api.lk_free_str = &SbtFake_free_str;
		Api.lk_client_create = &SbtFake_client_create;
		Api.lk_client_destroy = &SbtFake_client_destroy;
		Api.lk_client_set_data_callback = &SbtFake_set_data_callback;
		Api.lk_client_set_data_callback_ex = &SbtFake_set_data_callback_ex;
		Api.lk_client_set_audio_callback_ex = &SbtFake_set_audio_callback_ex;
		Api.lk_set_connection_callback = &SbtFake_set_connection_callback;
		Api.lk_connect_with_role_async = &SbtFake_connect_with_role_async;
		Api.lk_disconnect = &SbtFake_ok_client;
		Api.lk_refresh_token = &SbtFake_refresh_token;
		Api.lk_set_audio_publish_options = &SbtFake_set_audio_publish_options;
		Api.lk_set_audio_output_format = &SbtFake_set_audio_output_format;
		Api.lk_audio_track_create = &SbtFake_audio_track_create;
		Api.lk_audio_track_destroy = &SbtFake_audio_track_destroy;
		Api.lk_audio_track_publish_pcm_i16 = &SbtFake_audio_track_publish_pcm_i16;
		Api.lk_send_data_ex = &SbtFake_send_data_ex;
		Api.lk_set_default_data_labels = &SbtFake_set_default_data_labels;
		Api.lk_set_log_level = &SbtFake_set_log_level;
		return Api;
	}

	class FSbtRecordingConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InSubject, const TArray<uint8>& InPayload, double) override
		{
			FScopeLock Lock(&Mutex);
			Subjects.Add(InSubject);
			// The first four bytes carry the frame index (SbtMakeFrame).
			Indices.Add(InPayload.Num() >= 4 ? *reinterpret_cast<const int32*>(InPayload.GetData()) : -1);
		}

		int32 Num() const
		{
			FScopeLock Lock(&Mutex);
			return Indices.Num();
		}

		mutable FCriticalSection Mutex;
		TArray<FString> Subjects;
		TArray<int32> Indices;
	};

	class FSbtRecordingAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8*, int32 NumBytes) override
		{
			FScopeLock Lock(&Mutex);
			Labels.Add(Meta.StreamLabel);
			Bytes.Add(NumBytes);
		}

		int32 Num() const
		{
			FScopeLock Lock(&Mutex);
			return Labels.Num();
		}

		mutable FCriticalSection Mutex;
		TArray<FString> Labels;
		TArray<int32> Bytes;
	};

	class FSbtCountingControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8>, const FString&, double) override
		{
			Count.fetch_add(1);
		}

		std::atomic<int32> Count{ 0 };
	};

	/** A JWT-shaped token whose payload is {"exp":<one hour from now>}; the signature is not checked. */
	static FString SbtMakeTestJwt()
	{
		const FString Payload = FString::Printf(TEXT("{\"exp\":%lld}"), FDateTime::UtcNow().ToUnixTimestamp() + 3600);
		const FTCHARToUTF8 PayloadUtf8(*Payload);
		const FString PayloadBase64 = FBase64::Encode(reinterpret_cast<const uint8*>(PayloadUtf8.Get()), static_cast<uint32>(PayloadUtf8.Length()));
		return FString(TEXT("eyJhbGciOiJIUzI1NiJ9.")) + PayloadBase64 + TEXT(".c2ln");
	}

	static FO3DTransportConfig SbtMakeConfig()
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("127.0.0.1:7880"); // only the fake sees it
		Config.Secrets.Add(WebRTCUtils::TokenOptionKey, SbtMakeTestJwt());
		Config.StreamId = TEXT("sbt-stream");
		// Disable the receiver's no-data watchdog so the tests stay deterministic.
		Config.AdvancedParams.Add(TEXT("webrtc.reconnect_timeout"), TEXT("0"));
		return Config;
	}

	static FO3DTransportAudioConfig SbtMakeAudioConfig()
	{
		FO3DTransportAudioConfig Audio;
		Audio.bEnableAudio = true;
		Audio.SampleRate = 48000;
		Audio.NumChannels = 2;
		return Audio;
	}

	/** Mocap-looking bytes (first word is not the envelope magic) whose first four bytes are Index. */
	static TArray<uint8> SbtMakeFrame(int32 Index, int32 Size)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(FMath::Max(Size, 8));
		*reinterpret_cast<int32*>(Bytes.GetData()) = Index;
		return Bytes;
	}

	static FSbtFakeClient* SbtStartSender(FAutomationTestBase& Test, FSbtFakeLiveKit& Fake, FO3DWebRTCSender& Sender)
	{
		if (!Test.TestTrue(TEXT("Sender Initialize"), Sender.Initialize(SbtMakeConfig()).IsOk()))
		{
			return nullptr;
		}
		FSbtFakeClient* Client = Fake.LastClient();
		if (!Test.TestNotNull(TEXT("Sender client created"), Client) || !Test.TestTrue(TEXT("Sender Start"), Sender.Start().IsOk()))
		{
			return nullptr;
		}
		Client->FireConnection(LkConnConnected);
		return Client;
	}

	static FSbtFakeClient* SbtStartReceiver(FAutomationTestBase& Test, FSbtFakeLiveKit& Fake, FO3DWebRTCReceiver& Receiver,
		const TSharedPtr<ISerializedFrameConsumer>& Consumer,
		const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& AudioSink,
		const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& ControlSink)
	{
		if (!Test.TestTrue(TEXT("Receiver Initialize"), Receiver.Initialize(SbtMakeConfig()).IsOk()))
		{
			return nullptr;
		}
		FSbtFakeClient* Client = Fake.LastClient();
		Receiver.SetConsumer(Consumer);
		if (AudioSink.IsValid())
		{
			Receiver.SetAudioSink(AudioSink, SbtMakeAudioConfig());
		}
		if (ControlSink.IsValid())
		{
			Receiver.SetControlSink(ControlSink); // before Start, as the interface requires
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
// Receiver hand-off queue: frames are bounded (RefuseNewest, 16 MiB) and delivered in order
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSharedBlocksReceiverQueuePolicyTest,
	"Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverQueuePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSharedBlocksReceiverQueuePolicyTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCSharedBlocksTest;
	// The queue's only consumer is Poll, which runs on this thread, so nothing can drain it until
	// the test calls Poll: the deterministic stand-in for a pause hook (pitfalls 14 and 15) needs
	// no hook and no timing at all.
	constexpr int32 FrameBytes = 64 * 1024;
	constexpr int32 Capacity = static_cast<int32>(FWebRTCReceiverLink::MaxPendingFrameBytes / FrameBytes); // 256
	constexpr int32 Extra = 44;
	constexpr int32 Delivered = Capacity + Extra;

	FSbtFakeLiveKit Fake;
	const TSharedRef<FSbtRecordingConsumer> Consumer = MakeShared<FSbtRecordingConsumer>();
	FO3DWebRTCReceiver Receiver(FSbtFakeLiveKit::MakeApi());
	FSbtFakeClient* Client = SbtStartReceiver(*this, Fake, Receiver, Consumer, nullptr, nullptr);
	if (!Client)
	{
		return false;
	}

	// Two subjects, interleaved: the queue keeps global arrival order, so each subject's order too.
	for (int32 Index = 0; Index < Delivered; ++Index)
	{
		const TArray<uint8> Frame = SbtMakeFrame(Index, FrameBytes);
		Client->DeliverData((Index % 2) == 0 ? "SubjectA" : "SubjectB", Frame.GetData(), static_cast<size_t>(Frame.Num()));
	}

	TestEqual(TEXT("Nothing reaches the consumer before Poll"), Consumer->Num(), 0);
	TestEqual(TEXT("The frames over the 16 MiB budget are refused and counted as dropped"), Receiver.GetStats().DroppedFrames, static_cast<int64>(Extra));
	TestEqual(TEXT("The accepted frames wait for Poll"), Receiver.GetStats().PendingFrames, static_cast<int64>(Capacity));

	TestEqual(TEXT("Poll delivers every accepted frame"), Receiver.Poll(), Capacity);
	TestEqual(TEXT("The consumer received the accepted frames"), Consumer->Num(), Capacity);
	bool bInOrder = true;
	bool bSubjectsRight = true;
	for (int32 Index = 0; Index < Consumer->Num(); ++Index)
	{
		bInOrder &= Consumer->Indices[Index] == Index; // the oldest are kept, the newest refused
		bSubjectsRight &= Consumer->Subjects[Index] == ((Index % 2) == 0 ? TEXT("SubjectA") : TEXT("SubjectB"));
	}
	TestTrue(TEXT("Frames arrive in arrival order"), bInOrder);
	TestTrue(TEXT("The data label is the subject"), bSubjectsRight);
	TestEqual(TEXT("FramesReceived counts delivered frames"), Receiver.GetStats().FramesReceived, static_cast<int64>(Capacity));
	TestEqual(TEXT("Nothing waits after Poll"), Receiver.GetStats().PendingFrames, static_cast<int64>(0));

	// Poll freed the budget: the next frame is accepted again.
	const TArray<uint8> Next = SbtMakeFrame(Delivered, FrameBytes);
	Client->DeliverData("SubjectA", Next.GetData(), static_cast<size_t>(Next.Num()));
	TestEqual(TEXT("A frame after Poll is accepted"), Receiver.Poll(), 1);
	TestEqual(TEXT("No more drops"), Receiver.GetStats().DroppedFrames, static_cast<int64>(Extra));

	// Frames queued when Stop runs are discarded, not delivered later.
	Client->DeliverData("SubjectA", Next.GetData(), static_cast<size_t>(Next.Num()));
	Receiver.Stop();
	TestEqual(TEXT("Poll after Stop delivers nothing"), Receiver.Poll(), 0);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Audio does not wait behind the frame queue (receiver) or the data channel (sender)
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSharedBlocksReceiverAudioIndependentTest,
	"Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverAudioIndependentOfFrameQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSharedBlocksReceiverAudioIndependentTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCSharedBlocksTest;
	constexpr int32 FrameBytes = 1024 * 1024;
	constexpr int32 FramesToFill = static_cast<int32>(FWebRTCReceiverLink::MaxPendingFrameBytes / FrameBytes) + 2;

	FSbtFakeLiveKit Fake;
	const TSharedRef<FSbtRecordingConsumer> Consumer = MakeShared<FSbtRecordingConsumer>();
	const TSharedRef<FSbtRecordingAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FSbtRecordingAudioSink, ESPMode::ThreadSafe>();
	FO3DWebRTCReceiver Receiver(FSbtFakeLiveKit::MakeApi());
	FSbtFakeClient* Client = SbtStartReceiver(*this, Fake, Receiver, Consumer, AudioSink, nullptr);
	if (!Client)
	{
		return false;
	}

	// Fill the frame queue past its budget and do not Poll.
	for (int32 Index = 0; Index < FramesToFill; ++Index)
	{
		const TArray<uint8> Frame = SbtMakeFrame(Index, FrameBytes);
		Client->DeliverData("SubjectA", Frame.GetData(), static_cast<size_t>(Frame.Num()));
	}
	TestTrue(TEXT("The frame queue is full (frames refused)"), Receiver.GetStats().DroppedFrames > 0);

	// Audio is handed to the sink on LiveKit's thread, without Poll and past the full frame queue.
	TArray<int16> Pcm;
	Pcm.SetNumZeroed(480 * 2);
	TestTrue(TEXT("The audio callback is registered"), Client->DeliverAudio("SubjectA", Pcm.GetData(), 480, 2, 48000));
	TestEqual(TEXT("Audio reached the sink while the frame queue was full"), AudioSink->Num(), 1);
	if (AudioSink->Num() == 1)
	{
		TestEqual(TEXT("The track name is the audio subject"), AudioSink->Labels[0], FString(TEXT("SubjectA")));
		TestEqual(TEXT("The PCM bytes are passed through"), AudioSink->Bytes[0], 480 * 2 * static_cast<int32>(sizeof(int16)));
	}
	TestEqual(TEXT("No frame reached the consumer"), Consumer->Num(), 0);

	Receiver.Poll();
	TestTrue(TEXT("Poll then delivers the queued frames"), Consumer->Num() > 0);
	TestEqual(TEXT("Poll does not replay audio"), AudioSink->Num(), 1);

	Receiver.Stop();
	TestFalse(TEXT("No audio callback after Stop"), Client->DeliverAudio("SubjectA", Pcm.GetData(), 480, 2, 48000));
	TestEqual(TEXT("The sink saw nothing after Stop"), AudioSink->Num(), 1);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSharedBlocksSenderAudioIndependentTest,
	"Open3DBroadcast.Transport.WebRTC.SharedBlocks.SenderAudioIndependentOfDataChannel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSharedBlocksSenderAudioIndependentTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCSharedBlocksTest;
	FSbtFakeLiveKit Fake;
	{
		FO3DWebRTCSender Sender(FSbtFakeLiveKit::MakeApi());
		if (!SbtStartSender(*this, Fake, Sender))
		{
			return false;
		}
		const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender.CreateAudioSink(SbtMakeAudioConfig());
		if (!TestTrue(TEXT("Audio sink created"), AudioSink.IsValid()))
		{
			return false;
		}

		// LiveKit's data channel is full: it refuses every message.
		Fake.SendDataResult.store(201);
		const uint8 Small[8] = { 1, 0, 0, 0, 2, 0, 0, 0 };
		TestTrue(TEXT("The data channel refuses mocap"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Small, 8, TEXT("SubjectA"))) == EO3DSendResult::DroppedBackpressure);

		// Audio goes to its own track, so it is published regardless.
		TArray<float> Pcm;
		Pcm.SetNumZeroed(480 * 2);
		TestTrue(TEXT("Audio is published while the data channel refuses"), AudioSink->SubmitPcm(TEXT("SubjectA"), Pcm.GetData(), 480, 2, 48000, 0.0));
		TestTrue(TEXT("Audio is published again"), AudioSink->SubmitPcm(TEXT("SubjectA"), Pcm.GetData(), 480, 2, 48000, 0.01));
		TestEqual(TEXT("Both audio frames reached LiveKit"), Fake.AudioPublishCalls.load(), 2);
		TestEqual(TEXT("One track for the subject"), Fake.NumTracks(), 1);
		TestEqual(TEXT("Audio is not counted as a frame"), Sender.GetStats().FramesSent, static_cast<int64>(0));

		Sender.Stop();
		TestFalse(TEXT("Audio after Stop is refused"), AudioSink->SubmitPcm(TEXT("SubjectA"), Pcm.GetData(), 480, 2, 48000, 0.02));
	}
	TestEqual(TEXT("Nothing reached a destroyed client or track"), Fake.CallsAfterDestroy.load(), 0);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

// ---------------------------------------------------------------------------------------------
// Stop while mocap, control and audio are in flight (sender) or arriving (receiver)
// ---------------------------------------------------------------------------------------------

// 200 cycles: each starts five OS threads and a transport, so this stays near a second on a CI
// runner. Control.StopWhileSending already covers SendControl alone for 20 cycles; this adds
// mocap and audio and a new hand-off on the receiver. Every cycle forces the overlap (Stop runs
// only once every thread has made calls), so the count is not what makes the race likely.
static constexpr int32 WebRTCSharedBlocksStopCycles = 200;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSharedBlocksSenderStopWhileSendingTest,
	"Open3DBroadcast.Transport.WebRTC.SharedBlocks.SenderStopWhileSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSharedBlocksSenderStopWhileSendingTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCSharedBlocksTest;
	constexpr int32 MaxCallsPerThread = 5000;

	FSbtFakeLiveKit Fake;
	TArray<uint8> Envelope;
	{
		TArray<uint8> ControlPayload;
		ControlPayload.SetNumZeroed(64);
		O3DS::WriteControlEnvelope(ControlPayload, 1.0, Envelope);
	}
	const TArray<uint8> Frame = SbtMakeFrame(1, 256);
	TArray<float> Pcm;
	Pcm.SetNumZeroed(480 * 2);

	for (int32 Cycle = 0; Cycle < WebRTCSharedBlocksStopCycles; ++Cycle)
	{
		FO3DWebRTCSender Sender(FSbtFakeLiveKit::MakeApi());
		if (!SbtStartSender(*this, Fake, Sender))
		{
			return false;
		}
		const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender.CreateAudioSink(SbtMakeAudioConfig());
		if (!TestTrue(TEXT("Audio sink created"), AudioSink.IsValid()))
		{
			return false;
		}

		std::atomic<bool> bStop{ false };
		std::atomic<int32> ThreadsCalling{ 0 };
		std::atomic<int32> Calls{ 0 };
		TArray<TFuture<void>> Workers;
		auto Run = [&bStop, &ThreadsCalling, &Calls](TFunction<void()> Call)
		{
			bool bCounted = false;
			for (int32 Index = 0; Index < MaxCallsPerThread && !bStop.load(); ++Index)
			{
				Call();
				Calls.fetch_add(1);
				if (!bCounted)
				{
					ThreadsCalling.fetch_add(1);
					bCounted = true;
				}
			}
		};
		for (int32 Mocap = 0; Mocap < 2; ++Mocap)
		{
			Workers.Add(Async(EAsyncExecution::Thread, [&Run, &Sender, &Frame, Mocap]()
			{
				const FString Subject = Mocap == 0 ? TEXT("SubjectA") : TEXT("SubjectB");
				Run([&Sender, &Frame, &Subject]() { Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), Subject)); });
			}));
		}
		Workers.Add(Async(EAsyncExecution::Thread, [&Run, &Sender, &Envelope]()
		{
			Run([&Sender, &Envelope]() { Sender.SendControl(Envelope.GetData(), Envelope.Num()); });
		}));
		for (int32 Audio = 0; Audio < 2; ++Audio)
		{
			Workers.Add(Async(EAsyncExecution::Thread, [&Run, &AudioSink, &Pcm, Audio]()
			{
				const FString Label = Audio == 0 ? TEXT("SubjectA") : TEXT("SubjectB");
				Run([&AudioSink, &Pcm, &Label]() { AudioSink->SubmitPcm(Label, Pcm.GetData(), 480, 2, 48000, 0.0); });
			}));
		}

		// Stop once every thread has made calls, so Stop overlaps all three kinds.
		const double WaitStart = FPlatformTime::Seconds();
		while (ThreadsCalling.load() < Workers.Num() && FPlatformTime::Seconds() - WaitStart < 5.0)
		{
			FPlatformProcess::YieldThread();
		}
		const double StopStart = FPlatformTime::Seconds();
		Sender.Stop();
		const bool bStoppedPromptly = FPlatformTime::Seconds() - StopStart < 5.0;

		bStop.store(true);
		for (TFuture<void>& Worker : Workers)
		{
			Worker.Wait();
		}

		if (!TestTrue(TEXT("Stop returns promptly while sends are in flight"), bStoppedPromptly)
			|| !TestTrue(TEXT("The send threads made calls"), Calls.load() > 0)
			|| !TestTrue(TEXT("SendSerialized after Stop returns NotRunning"), Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("SubjectA"))) == EO3DSendResult::NotRunning)
			|| !TestTrue(TEXT("SendControl after Stop returns NotRunning"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotRunning)
			|| !TestFalse(TEXT("Audio after Stop is refused"), AudioSink->SubmitPcm(TEXT("SubjectA"), Pcm.GetData(), 480, 2, 48000, 0.0)))
		{
			break;
		}
	}
	TestEqual(TEXT("Nothing reached a destroyed client or track"), Fake.CallsAfterDestroy.load(), 0);
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSharedBlocksReceiverStopWhileReceivingTest,
	"Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverStopWhileReceiving",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSharedBlocksReceiverStopWhileReceivingTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace WebRTCSharedBlocksTest;
	constexpr int32 MaxCallsPerThread = 5000;

	FSbtFakeLiveKit Fake;
	TArray<uint8> Envelope;
	{
		TArray<uint8> ControlPayload;
		ControlPayload.SetNumZeroed(64);
		O3DS::WriteControlEnvelope(ControlPayload, 1.0, Envelope);
	}
	const TArray<uint8> Frame = SbtMakeFrame(1, 256);
	TArray<int16> Pcm;
	Pcm.SetNumZeroed(480 * 2);

	for (int32 Cycle = 0; Cycle < WebRTCSharedBlocksStopCycles; ++Cycle)
	{
		const TSharedRef<FSbtRecordingConsumer> Consumer = MakeShared<FSbtRecordingConsumer>();
		const TSharedRef<FSbtRecordingAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FSbtRecordingAudioSink, ESPMode::ThreadSafe>();
		{
			FO3DWebRTCReceiver Receiver(FSbtFakeLiveKit::MakeApi());
			TSharedPtr<FSbtCountingControlSink, ESPMode::ThreadSafe> ControlSink = MakeShared<FSbtCountingControlSink, ESPMode::ThreadSafe>();
			const TWeakPtr<FSbtCountingControlSink, ESPMode::ThreadSafe> WeakControlSink = ControlSink;
			FSbtFakeClient* Client = SbtStartReceiver(*this, Fake, Receiver, Consumer, AudioSink, ControlSink);
			// From here only the receiver holds the control sink, so Stop must release it.
			ControlSink.Reset();
			if (!Client)
			{
				return false;
			}

			// LiveKit's threads: two deliver mocap, one control, one audio.
			std::atomic<bool> bStop{ false };
			std::atomic<int32> ThreadsCalling{ 0 };
			TArray<TFuture<void>> Workers;
			auto Run = [&bStop, &ThreadsCalling](TFunction<void()> Call)
			{
				bool bCounted = false;
				for (int32 Index = 0; Index < MaxCallsPerThread && !bStop.load(); ++Index)
				{
					Call();
					if (!bCounted)
					{
						ThreadsCalling.fetch_add(1);
						bCounted = true;
					}
				}
			};
			for (int32 Mocap = 0; Mocap < 2; ++Mocap)
			{
				Workers.Add(Async(EAsyncExecution::Thread, [&Run, Client, &Frame, Mocap]()
				{
					const char* Label = Mocap == 0 ? "SubjectA" : "SubjectB";
					Run([Client, &Frame, Label]() { Client->DeliverData(Label, Frame.GetData(), static_cast<size_t>(Frame.Num())); });
				}));
			}
			Workers.Add(Async(EAsyncExecution::Thread, [&Run, Client, &Envelope]()
			{
				Run([Client, &Envelope]() { Client->DeliverData(WebRTCUtils::ControlDataLabelUtf8, Envelope.GetData(), static_cast<size_t>(Envelope.Num())); });
			}));
			Workers.Add(Async(EAsyncExecution::Thread, [&Run, Client, &Pcm]()
			{
				Run([Client, &Pcm]() { Client->DeliverAudio("SubjectA", Pcm.GetData(), 480, 2, 48000); });
			}));

			// The game thread polls while the callbacks run, then stops in the middle of them.
			const double WaitStart = FPlatformTime::Seconds();
			while (ThreadsCalling.load() < Workers.Num() && FPlatformTime::Seconds() - WaitStart < 5.0)
			{
				Receiver.Poll();
				FPlatformProcess::YieldThread();
			}
			Receiver.Poll();
			const double StopStart = FPlatformTime::Seconds();
			Receiver.Stop();
			const bool bStoppedPromptly = FPlatformTime::Seconds() - StopStart < 5.0;
			const int32 FramesAtStop = Consumer->Num();
			const int32 AudioAtStop = AudioSink->Num();

			bStop.store(true);
			for (TFuture<void>& Worker : Workers)
			{
				Worker.Wait();
			}

			const bool bOk = TestTrue(TEXT("Stop returns promptly while callbacks are arriving"), bStoppedPromptly)
				&& TestEqual(TEXT("Poll after Stop delivers nothing"), Receiver.Poll(), 0)
				&& TestEqual(TEXT("No frame reached the consumer after Stop"), Consumer->Num(), FramesAtStop)
				&& TestEqual(TEXT("No audio reached the sink after Stop"), AudioSink->Num(), AudioAtStop)
				&& TestTrue(TEXT("Frames were delivered before Stop"), FramesAtStop > 0)
				&& TestFalse(TEXT("The control sink is released by Stop"), WeakControlSink.IsValid());
			if (!bOk)
			{
				break;
			}
		}
	}
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; skipped."));
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
