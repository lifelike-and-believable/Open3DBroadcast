// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Receiver/NngReceiver.h"
#include "O3DRedact.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "O3DFfiContextRegistry.h"
#include "SerializedFrameConsumerRegistry.h"
#include "O3DUnifiedMessage.h"
#include "O3DAudioFrameCodec.h"

#if !defined(NNG_STATIC_LIB)
#define NNG_STATIC_LIB 1
#endif

THIRD_PARTY_INCLUDES_START
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pubsub0/sub.h>
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogO3DNngReceiver, Log, All);

namespace
{
    constexpr double InitialBackoffSeconds = 0.1;
    constexpr double MaxBackoffSeconds = 5.0;
    /** Largest message accepted. Also set as NNG_OPT_RECVMAXSZ, so NNG enforces it (TRB-42). */
    constexpr uint64 MaxPayloadBytes = 50ull * 1024ull * 1024ull;

    TO3DFfiContextRegistry<FNngReceiverPipeContext>& GetReceiverPipeContextRegistry()
    {
        static TO3DFfiContextRegistry<FNngReceiverPipeContext> Registry;
        return Registry;
    }

    /** NNG pipe callback. `Context` is an opaque token, never a receiver pointer (TRB-42). */
    static void ReceiverPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        const TSharedPtr<FNngReceiverPipeContext, ESPMode::ThreadSafe> Pipe = GetReceiverPipeContextRegistry().Resolve(Context);
        if (!Pipe.IsValid())
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_add(1) + 1;
            Pipe->bConnected.store(true);
            UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe added (count=%d)"), Count);
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_sub(1) - 1;
            if (Count <= 0)
            {
                Pipe->bConnected.store(Pipe->bConnectedWithoutPipes.load());
            }
            UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe removed (count=%d)"), FMath::Max(0, Count));
        }
    }
}

struct FO3DNngReceiver::FNngSocketWrapper
{
    nng_socket Socket{ NNG_SOCKET_INITIALIZER };

    ~FNngSocketWrapper()
    {
        if (Socket.id != 0)
        {
            nng_close(Socket);
            Socket.id = 0;
        }
    }
};

FO3DNngReceiver::FO3DNngReceiver()
    : PipeContext(MakeShared<FNngReceiverPipeContext, ESPMode::ThreadSafe>())
{
    PipeToken = GetReceiverPipeContextRegistry().Register(PipeContext);
}

/**
 * Initialize the NNG receiver from a transport configuration.
 *
 * Parses receiver-specific options and prepares internal state for operation.
 *
 * - Calls Stop() to ensure any previous receiver state is torn down.
 * - Parses receiver options via O3DNNG::ParseReceiverOptions(Config, Options, Error).
 *   On parse failure a warning is logged (UE_LOG) and the method returns false.
 * - On success updates ActiveConfig from Config and overrides:
 *     - ActiveConfig.Uri = Options.CanonicalUri
 *     - ActiveConfig.StreamId = Options.StreamId
 * - Copies audio settings into ActiveAudioConfig (audio stream label is derived from StreamId).
 * - Resets runtime counters/state: Stats, pipe context, BackoffAttempt, LastDialAttempt, LastErrorLogTimestamp.
 * - Marks the receiver initialized (bInitialized = true).
 *
 * @param Config  Transport configuration to use for initialization.
 * @return true if initialization succeeded; false if option parsing failed.
 *
 * Thread-safety: Not thread-safe. Caller must ensure no concurrent access to the receiver while initializing.
 */
bool FO3DNngReceiver::Initialize(const FO3DTransportConfig& Config)
{
    Stop();

    FString Error;
    if (!O3DNNG::ParseReceiverOptions(Config, Options, Error))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("Failed to parse NNG receiver config: %s"), *Error);
        return false;
    }

    ActiveConfig = Config;
    ActiveConfig.Uri = Options.CanonicalUri;
    ActiveConfig.StreamId = Options.StreamId;
    ActiveAudioConfig = Config.Audio;
    // Note: Audio stream label is now automatically derived from StreamId

    {
        FScopeLock Lock(&StatsMutex);
        Stats.Reset();
    }
    PipeContext->PipeCount.store(0);
    PipeContext->bConnectedWithoutPipes.store(Options.bListen);
    BackoffAttempt = 0;
    LastDialAttempt = 0.0;
    LastErrorLogTimestamp = 0.0;

    bInitialized = true;
    return true;
}

void FO3DNngReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
    Consumer = InConsumer;
}

void FO3DNngReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    AudioSink = Sink;
    if (Sink.IsValid())
    {
        ActiveAudioConfig = AudioConfig;
        // Note: Audio stream label is now automatically derived from StreamId
    }
}

FO3DNngReceiver::~FO3DNngReceiver()
{
    Stop();
    // After nng_close() a late pipe callback resolves the token to nothing (TRB-42).
    GetReceiverPipeContextRegistry().Unregister(PipeToken);
    PipeToken = nullptr;
}

bool FO3DNngReceiver::Start()
{
    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver Start called before Initialize"));
        return false;
    }

    if (bRunning.Load())
    {
        return true;
    }

    BackoffAttempt = 0;
    LastDialAttempt = 0.0;

    const bool bOpened = OpenSocket();
    if (!bOpened && Options.bListen)
    {
        // OpenSocket logged the reason (for example, the port is in use).
        return false;
    }
    bRunning = true;

    UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver started - Mode=%s Role=%s URI=%s"),
        *O3DNNG::ModeToString(Options.Mode),
        *O3DNNG::RoleToString(Options.Role),
        *O3DRedact::Url(Options.CanonicalUri));
    return true;
}

void FO3DNngReceiver::Stop()
{
    if (!bRunning.Load())
    {
        CloseSocket();
        return;
    }

    CloseSocket();
    bRunning = false;
    PipeContext->bConnected.store(false);
}

/**
 * Polls the NNG socket for incoming messages and processes up to FO3DNngReceiver::FramesPerPoll frames.
 *
 * Behavior:
 *  - Returns 0 immediately if the receiver is not running or if a required socket cannot be opened/dialed.
 *  - Ensures a dial or listen socket depending on Options.bListen (calls EnsureDialSocket() or OpenSocket()).
 *  - Receives messages using nng_recv with NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC.
 *    - If nng_recv returns NNG_EAGAIN or NNG_ETIMEDOUT, the poll loop stops (no more messages).
 *    - On other non-zero return values, HandleReceiveError(Ret) is called and the loop exits.
 *  - Zero-length messages are freed and skipped.
 *  - Messages exceeding MaxPayloadBytes are freed, counted as dropped (Stats.DroppedFrames++), and skipped.
 *    NNG_OPT_RECVMAXSZ is set to the same cap, so NNG normally rejects them first.
 *  - Valid messages are handed to ProcessReceivedPayload(...). The allocated buffer is freed with nng_free after processing.
 *    - If ProcessReceivedPayload returns true: increment FramesProcessed, increment Stats.FramesReceived, add to Stats.BytesReceived.
 *      This is the only place that counts a received frame, mocap or audio (TRB-42).
 *    - If it returns false: increment Stats.DroppedFrames.
 *  - All updates to Stats are performed under StatsMutex (FScopeLock).
 *
 * @return Number of frames successfully processed during this poll.
 */
int32 FO3DNngReceiver::Poll()
{
    if (!bRunning.Load())
    {
        return 0;
    }

    if (!Options.bListen)
    {
        if (!EnsureDialSocket())
        {
            return 0;
        }
    }
    else if (!Socket)
    {
        if (!OpenSocket())
        {
            return 0;
        }
    }

    if (!Socket)
    {
        return 0;
    }

    int32 FramesProcessed = 0;

    while (FramesProcessed < FO3DNngReceiver::FramesPerPoll)
    {
        void* Buffer = nullptr;
        size_t Size = 0;
        const int Ret = nng_recv(Socket->Socket, &Buffer, &Size, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);
        if (Ret == NNG_EAGAIN || Ret == NNG_ETIMEDOUT)
        {
            break;
        }

        if (Ret != 0)
        {
            HandleReceiveError(Ret);
            break;
        }

        if (Size == 0)
        {
            nng_free(Buffer, Size);
            continue;
        }

        if (Size > MaxPayloadBytes)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver payload %llu bytes exceeds safety cap; dropping."), static_cast<unsigned long long>(Size));
            nng_free(Buffer, Size);
            {
                FScopeLock Lock(&StatsMutex);
                Stats.DroppedFrames++;
            }
            continue;
        }

        // Process the payload (routes to mocap or audio based on unified header)
        const bool bProcessed = ProcessReceivedPayload(static_cast<const uint8*>(Buffer), static_cast<int32>(Size));
        nng_free(Buffer, Size);

        if (bProcessed)
        {
            FScopeLock Lock(&StatsMutex);
            Stats.FramesReceived++;
            Stats.BytesReceived += static_cast<int64>(Size);
            ++FramesProcessed;
        }
        else
        {
            FScopeLock Lock(&StatsMutex);
            Stats.DroppedFrames++;
        }
    }

    return FramesProcessed;
}

FO3DTransportStats FO3DNngReceiver::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

/**
 * OpenSocket
 *
 * Initialize and open an NNG socket based on the current Options and attach it to this receiver.
 *
 * Behavior:
 * - Closes any prior socket before proceeding.
 * - Allocates a new FNngSocketWrapper and attempts to open the appropriate NNG socket for Options.Mode:
 *     - ENngMode::Sub  : open SUB socket and subscribe to Options.Topic (subscribe to all if Topic is empty).
 *     - ENngMode::Pair : open PAIR socket.
 *     - ENngMode::Pull : open PULL socket.
 *     - Other modes     : log a warning, free the temporary socket and return false.
 * - For listening endpoints (Options.bListen == true) calls nng_listen; otherwise calls nng_dial with NNG_FLAG_NONBLOCK.
 * - A listening socket counts as connected (ready) once it listens; a dialing socket only after
 *   its first pipe event (TRB-42).
 * - On any open/configure failure logs a warning, deletes the temporary socket, sets Socket to nullptr, and if dialing
 *   updates LastDialAttempt and increments BackoffAttempt.
 * - Before listen/dial resets the pipe context, registers pipe add/remove notifications and sets
 *   NNG_OPT_RECVMAXSZ; on success assigns the new socket to Socket and updates LastDialAttempt.
 *
 * Side effects / member modifications:
 * - Socket            : set to the newly allocated FNngSocketWrapper on success, left/nullified on failure.
 * - PipeContext       : reset; bConnected set to true if a listen succeeds.
 * - LastDialAttempt   : set to current FPlatformTime::Seconds() on success and on dial failure.
 * - BackoffAttempt    : incremented on dial failure when not listening.
 *
 * Return:
 * - true  if the socket was successfully created, configured and attached to this receiver.
 * - false if any step failed or the mode is unsupported.
 */
bool FO3DNngReceiver::OpenSocket()
{
    CloseSocket();

    FNngSocketWrapper* NewSocket = new FNngSocketWrapper();
    int Ret = 0;

    switch (Options.Mode)
    {
    case O3DNNG::ENngMode::Sub:
        Ret = nng_sub0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            if (Options.Topic.IsEmpty())
            {
                Ret = nng_setopt(NewSocket->Socket, NNG_OPT_SUB_SUBSCRIBE, "", 0);
            }
            else
            {
                const FTCHARToUTF8 TopicUtf8(*Options.Topic);
                Ret = nng_setopt(NewSocket->Socket, NNG_OPT_SUB_SUBSCRIBE, TopicUtf8.Get(), static_cast<size_t>(TopicUtf8.Length()));
            }
        }
        if (Ret == 0)
        {
            // Receive buffer is an int counting messages (TRB-36). A sub socket drops messages
            // that arrive while this queue is full, and Poll() drains it only once per tick.
            constexpr int NngRecvBufMessages = 1024;
            const int SetRecvBufRet = nng_socket_set_int(NewSocket->Socket, NNG_OPT_RECVBUF, NngRecvBufMessages);
            if (SetRecvBufRet != 0)
            {
                UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not set receive buffer to %d messages (%d %s)"),
                    NngRecvBufMessages, SetRecvBufRet, UTF8_TO_TCHAR(nng_strerror(SetRecvBufRet)));
            }
        }
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        break;
    case O3DNNG::ENngMode::Pull:
        Ret = nng_pull0_open(&NewSocket->Socket);
        break;
    default:
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver unsupported mode %s"), *O3DNNG::ModeToString(Options.Mode));
        delete NewSocket;
        return false;
    }

    if (Ret == 0)
    {
        // TRB-42: without this, NNG applies its own default receive limit (1 MiB per the NNG
        // option docs) and drops a larger message before Poll() sees it, so the MaxPayloadBytes
        // check below was unreachable. recv-size-max is a size_t byte count.
        const int SetMaxSizeRet = nng_socket_set_size(NewSocket->Socket, NNG_OPT_RECVMAXSZ, static_cast<size_t>(MaxPayloadBytes));
        if (SetMaxSizeRet != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not set the maximum message size to %llu bytes (%d %s)"),
                static_cast<unsigned long long>(MaxPayloadBytes), SetMaxSizeRet, UTF8_TO_TCHAR(nng_strerror(SetMaxSizeRet)));
        }

        // Notifications are registered before listen/dial so the first pipe event is not missed.
        PipeContext->PipeCount.store(0);
        PipeContext->bConnected.store(false);
        const int AddNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, ReceiverPipeCallback, PipeToken);
        if (AddNotify != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver pipe notify add failed (%d) %s"), AddNotify, UTF8_TO_TCHAR(nng_strerror(AddNotify)));
        }
        const int RemoveNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, ReceiverPipeCallback, PipeToken);
        if (RemoveNotify != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver pipe notify remove failed (%d) %s"), RemoveNotify, UTF8_TO_TCHAR(nng_strerror(RemoveNotify)));
        }

        const FTCHARToUTF8 AddressUtf8(*Options.TcpAddress);
        if (Options.bListen)
        {
            Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
        }
        else
        {
            // Non-blocking dial: NNG retries in the background. "Connected" comes from pipe events.
            Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
        }
    }

    if (Ret != 0)
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not %s %s (%d) %s"),
            Options.bListen ? TEXT("listen on") : TEXT("dial"),
            *O3DRedact::Url(Options.CanonicalUri), Ret, UTF8_TO_TCHAR(nng_strerror(Ret)));
        delete NewSocket;
        Socket = nullptr;
        if (!Options.bListen)
        {
            LastDialAttempt = FPlatformTime::Seconds();
            BackoffAttempt = FMath::Min(BackoffAttempt + 1, 10);
        }
        return false;
    }

    if (Options.bListen)
    {
        PipeContext->bConnected.store(true);
    }

    Socket = NewSocket;
    LastDialAttempt = FPlatformTime::Seconds();
    return true;
}

void FO3DNngReceiver::CloseSocket()
{
    if (Socket)
    {
        delete Socket;
        Socket = nullptr;
    }
}

void FO3DNngReceiver::HandleReceiveError(int ErrorCode)
{
    const double Now = FPlatformTime::Seconds();
    if (Now - LastErrorLogTimestamp > 2.0)
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver recv failed (%d) %s"), ErrorCode, UTF8_TO_TCHAR(nng_strerror(ErrorCode)));
        LastErrorLogTimestamp = Now;
    }

    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;
    }

    if (!Options.bListen)
    {
        CloseSocket();
        BackoffAttempt = FMath::Min(BackoffAttempt + 1, 10);
        LastDialAttempt = Now;
        PipeContext->bConnected.store(false);
    }
}

bool FO3DNngReceiver::EnsureDialSocket()
{
    if (Socket)
    {
        return true;
    }

    const double Now = FPlatformTime::Seconds();
    const double Delay = FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Clamp(BackoffAttempt, 0, 8))));
    if ((Now - LastDialAttempt) >= Delay)
    {
        if (OpenSocket())
        {
            BackoffAttempt = 0;
            return true;
        }

        LastDialAttempt = Now;
    }

    return Socket != nullptr;
}

bool FO3DNngReceiver::ProcessReceivedPayload(const uint8* Data, int32 Size)
{
    if (!Data || Size <= 0)
    {
        return false;
    }

    // Try to parse as a unified message
    O3DS::FUnifiedHeader Header;
    const uint8* PayloadPtr = nullptr;
    int32 PayloadSize = 0;

    if (O3DS::ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize))
    {
        // Successfully parsed unified header - route based on message kind
        if (Header.GetKind() == O3DS::EUnifiedKind::Audio)
        {
            return ProcessAudioPayload(Header.GetCodec(), PayloadPtr, PayloadSize);
        }
        else if (Header.GetKind() == O3DS::EUnifiedKind::Mocap)
        {
            // Route mocap data to the frame consumer. TRB-37: hand over only the
            // payload after the 20-byte unified header, as the TCP and UDP receivers
            // do; the consumer expects a bare O3DS frame.
            if (!PayloadPtr || PayloadSize <= 0)
            {
                return false;
            }
            if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
            {
                TArray<uint8> Payload;
                Payload.SetNumUninitialized(PayloadSize);
                FMemory::Memcpy(Payload.GetData(), PayloadPtr, PayloadSize);
                ConsumerPinned->SubmitFrame(Options.StreamId, Payload, FPlatformTime::Seconds());
                return true;
            }
        }
    }
    else
    {
        // Not a unified message - assume it's raw mocap data for backward compatibility
        if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
        {
            TArray<uint8> Payload;
            Payload.SetNumUninitialized(Size);
            FMemory::Memcpy(Payload.GetData(), Data, Size);
            ConsumerPinned->SubmitFrame(Options.StreamId, Payload, FPlatformTime::Seconds());
            return true;
        }
    }

    return false;
}

bool FO3DNngReceiver::ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize)
{
    if (!Payload || PayloadSize <= 0)
    {
        return false;
    }

    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin();
    if (!SinkPinned.IsValid())
    {
        // No audio sink configured - silently drop
        return true;
    }

    O3DAudio::FEncodedAudioFrame EncodedFrame;
    if (!O3DAudio::DeserializeEncodedAudioFrame(Codec, Payload, PayloadSize, EncodedFrame))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver failed to deserialize audio frame (payload=%d codec=%d)."), PayloadSize, static_cast<int32>(Codec));
        return false;
    }

    if (Codec == O3DS::EUnifiedCodec::PCM16)
    {
        SinkPinned->SubmitPcm16(EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num());
        // Counted once, by Poll() (TRB-42).
        return true;
    }

    if (!AudioDecoder.Decode(Codec, EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num(), DecodedPcmScratch))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver failed to decode audio frame (codec=%d)."), static_cast<int32>(Codec));
        return false;
    }

    SinkPinned->SubmitPcm16(EncodedFrame.Meta,
        reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()),
        DecodedPcmScratch.Num() * static_cast<int32>(sizeof(int16)));
    // Counted once, by Poll() (TRB-42).
    return true;
}

#endif // O3D_WITH_TRANSPORT_NNG
