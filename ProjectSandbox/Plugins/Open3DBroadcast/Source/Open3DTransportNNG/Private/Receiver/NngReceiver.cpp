// Copyright (c) Open3DStream Contributors
#include "Receiver/NngReceiver.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"
#include "SerializedFrameConsumerRegistry.h"
#include "O3DUnifiedMessage.h"
#include "O3DAudioFrameCodec.h"

#if !defined(NNG_STATIC_LIB)
#define NNG_STATIC_LIB 1
#endif

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pubsub0/sub.h>

DEFINE_LOG_CATEGORY_STATIC(LogO3DNngReceiver, Log, All);

namespace
{
    constexpr double InitialBackoffSeconds = 0.1;
    constexpr double MaxBackoffSeconds = 5.0;
    constexpr uint64 MaxPayloadBytes = 50ull * 1024ull * 1024ull;

    static void ReceiverPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        FO3DNngReceiver* Receiver = static_cast<FO3DNngReceiver*>(Context);
        if (!Receiver)
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            Receiver->HandlePipeAdded();
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            Receiver->HandlePipeRemoved();
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
 * - Resets runtime counters/state: Stats, PipeCount, BackoffAttempt, LastDialAttempt, LastErrorLogTimestamp.
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

    Stats.Reset();
    PipeCount.Reset();
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
    bRunning = true;

    UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver started uri=%s"), *Options.CanonicalUri);
    return bOpened || !Options.bListen;
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
    bConnected = false;
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
 *  - Valid messages are handed to ProcessReceivedPayload(...). The allocated buffer is freed with nng_free after processing.
 *    - If ProcessReceivedPayload returns true: increment FramesProcessed, increment Stats.FramesReceived, add to Stats.BytesReceived.
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

void FO3DNngReceiver::HandlePipeAdded()
{
    const int32 Count = PipeCount.Increment();
    bConnected = true;
    BackoffAttempt = 0;
    UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe added (count=%d)"), Count);
}

void FO3DNngReceiver::HandlePipeRemoved()
{
    const int32 Count = PipeCount.Decrement();
    if (Count <= 0)
    {
        if (!Options.bListen)
        {
            bConnected = false;
        }
        else
        {
            bConnected = true;
        }
    }
    UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe removed (count=%d)"), FMath::Max(0, Count));
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
 * - On successful listen/dial sets bConnected accordingly.
 * - On any open/configure failure logs a warning, deletes the temporary socket, sets Socket to nullptr, and if dialing
 *   updates LastDialAttempt and increments BackoffAttempt.
 * - On success resets PipeCount, registers pipe add/remove notifications (logs verbose if notify registration fails),
 *   assigns the new socket to Socket and updates LastDialAttempt.
 *
 * Side effects / member modifications:
 * - Socket            : set to the newly allocated FNngSocketWrapper on success, left/nullified on failure.
 * - bConnected        : set to true if the listen/dial call succeeds.
 * - LastDialAttempt   : set to current FPlatformTime::Seconds() on success and on dial failure.
 * - BackoffAttempt    : incremented on dial failure when not listening.
 * - PipeCount         : reset when socket opens successfully.
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

    const FTCHARToUTF8 AddressUtf8(*Options.TcpAddress);

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
            if (Options.bListen)
            {
                Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
                bConnected = (Ret == 0);
            }
            else
            {
                Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
                bConnected = (Ret == 0);
            }
        }
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            if (Options.bListen)
            {
                Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
                bConnected = (Ret == 0);
            }
            else
            {
                Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
                bConnected = (Ret == 0);
            }
        }
        break;
    case O3DNNG::ENngMode::Pull:
        Ret = nng_pull0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            if (Options.bListen)
            {
                Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
                bConnected = (Ret == 0);
            }
            else
            {
                Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
                bConnected = (Ret == 0);
            }
        }
        break;
    default:
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver unsupported mode"));
        delete NewSocket;
        return false;
    }

    if (Ret != 0)
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver socket open failed (%d) %s"), Ret, UTF8_TO_TCHAR(nng_strerror(Ret)));
        delete NewSocket;
        Socket = nullptr;
        if (!Options.bListen)
        {
            LastDialAttempt = FPlatformTime::Seconds();
            BackoffAttempt++;
        }
        return false;
    }

    PipeCount.Reset();
    const int AddNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, ReceiverPipeCallback, this);
    if (AddNotify != 0)
    {
        UE_LOG(LogO3DNngReceiver, Verbose, TEXT("NNG receiver pipe notify add failed (%d) %s"), AddNotify, UTF8_TO_TCHAR(nng_strerror(AddNotify)));
    }
    const int RemoveNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, ReceiverPipeCallback, this);
    if (RemoveNotify != 0)
    {
        UE_LOG(LogO3DNngReceiver, Verbose, TEXT("NNG receiver pipe notify remove failed (%d) %s"), RemoveNotify, UTF8_TO_TCHAR(nng_strerror(RemoveNotify)));
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
    if (Now - LastErrorLogTimestamp > 0.25)
    {
        UE_LOG(LogO3DNngReceiver, Verbose, TEXT("NNG receiver recv failed (%d) %s"), ErrorCode, UTF8_TO_TCHAR(nng_strerror(ErrorCode)));
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
        bConnected = false;
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
            // Route mocap data to the frame consumer
            if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
            {
                TArray<uint8> Payload;
                Payload.SetNumUninitialized(Size);
                FMemory::Memcpy(Payload.GetData(), Data, Size);
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
        {
            FScopeLock Lock(&StatsMutex);
            Stats.FramesReceived++;
            Stats.BytesReceived += PayloadSize;
        }
        return true;
    }

    if (!AudioDecoder.Decode(Codec, EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num(), DecodedPcmScratch))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver failed to decode audio frame (codec=%d)."), static_cast<int32>(Codec));
        return false;
    }

    SinkPinned->SubmitPcm16(EncodedFrame.Meta,
        reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()),
        DecodedPcmScratch.Num() * sizeof(int16));
    {
        FScopeLock Lock(&StatsMutex);
        Stats.FramesReceived++;
        Stats.BytesReceived += PayloadSize;
    }
    return true;
}
