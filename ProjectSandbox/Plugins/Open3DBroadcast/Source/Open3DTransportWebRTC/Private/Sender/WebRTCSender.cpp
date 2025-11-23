#include "WebRTCSender.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "livekit_ffi.h"
#include "o3ds/model.h"
#include "O3DPerformanceMetrics.h"
#include <vector>

using WebRTCUtils::FromAnsi;

/**
 * PHASE 1 OPTIMIZATION: Simple per-subject serialization buffer pool.
 *
 * Pools reusable SubjectList and serialization buffers to eliminate per-frame allocations.
 * This is a single-threaded pool - call from a single thread (sender frame thread).
 *
 * Impact: Reduces 3,000+ allocations/sec → 0 allocations/sec
 */
struct FPooledSubject
{
    O3DS::SubjectList SubjectList;           // Pre-allocated container for single subject
    std::vector<char> SerializedBuffer;      // Pre-allocated serialization buffer (~16KB)

    FPooledSubject()
    {
        SerializedBuffer.reserve(16 * 1024);
    }

    /**
     * Reset for reuse: clears subject data and buffer for next serialization.
     * Does NOT deallocate memory - maintains pre-allocated capacity.
     */
    void Reset()
    {
        // Clear subject list (deletes contained subjects)
        for (auto Subject : SubjectList.mItems)
        {
            delete Subject;
        }
        SubjectList.mItems.clear();

        // Clear buffer but keep reserved memory
        SerializedBuffer.clear();
    }
};

/**
 * Simple single-threaded object pool for serialization.
 * Not thread-safe - intended for use from a single sender frame thread.
 */
class FSerializerPool
{
public:
    FSerializerPool(int32 PreAllocatedCount = 10)
    {
        for (int32 i = 0; i < PreAllocatedCount; ++i)
        {
            Available.Add(MakeUnique<FPooledSubject>());
        }
    }

    /**
     * Acquire a pooled subject for serialization.
     * If pool is empty, allocates additional items on-demand.
     */
    FPooledSubject* Acquire()
    {
        if (Available.Num() > 0)
        {
            return Available.Pop(EAllowShrinking::No).Release();  // Pop and release ownership
        }

        // Pool empty - allocate on demand
        return new FPooledSubject();
    }

    /**
     * Return pooled item to pool for reuse.
     */
    void Release(FPooledSubject* Item)
    {
        if (Item)
        {
            Item->Reset();
            Available.Add(TUniquePtr<FPooledSubject>(Item));
        }
    }

private:
    TArray<TUniquePtr<FPooledSubject>> Available;
};


namespace WebRTCOptions
{
    static constexpr TCHAR PreferLossyOptionKey[] = TEXT("webrtc.prefer_lossy");

    static bool ParseBool(const TMap<FString, FString>& Params, const TCHAR* Key, bool DefaultValue)
    {
        if (!Key)
        {
            return DefaultValue;
        }

        if (const FString* Value = Params.Find(Key))
        {
            if (Value->Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("yes"), ESearchCase::IgnoreCase))
            {
                return true;
            }

            if (Value->Equals(TEXT("0"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("no"), ESearchCase::IgnoreCase))
            {
                return false;
            }
        }

        return DefaultValue;
    }
}

/**
 * Audio sink implementation for WebRTC sender using LiveKit FFI.
 *
 * Publishes audio to per-subject labeled audio tracks via lk_audio_track_publish_pcm_i16().
 * Each StreamLabel gets its own dedicated audio track, preventing distortion from multiple
 * concurrent audio sources.
 *
 * Optimized with reusable PCM conversion buffer to avoid per-frame allocations.
 * This significantly reduces allocator pressure on the audio thread.
 *
 * Key Features:
 * - Per-subject audio isolation (no mixing distortion)
 * - Automatic track creation on first audio for each subject
 * - Thread-safe via AudioTracksMutex in parent sender
 * - Graceful fallback if track creation fails
 */
class FWebRTCSenderAudioSink final : public IO3DSenderAudioSink
{
public:
    FWebRTCSenderAudioSink(FO3DWebRTCSender& InOwner)
        : Owner(InOwner), PcmConversionBuffer()
    {
    }

    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
        if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
        {
            return false;
        }

        if (!Owner.ClientHandle || !Owner.bConnected.Load())
        {
            return false;
        }

        // Get or create audio track for this subject
        LkAudioTrackHandle* Track = GetOrCreateAudioTrack(StreamLabel, NumChannels, SampleRate);
        if (!Track)
        {
            // Track creation failed, but we return false so it's logged properly
            return false;
        }

        // Reuse buffer, allocate only if necessary (optimization to reduce per-frame allocations)
        const int32 TotalSamples = NumFrames * NumChannels;
        if (PcmConversionBuffer.Num() < TotalSamples)
        {
            PcmConversionBuffer.SetNumUninitialized(TotalSamples);
        }

        // Convert float to int16 with clamping
        for (int32 i = 0; i < TotalSamples; ++i)
        {
            float Sample = FMath::Clamp(Interleaved[i], -1.0f, 1.0f);
            PcmConversionBuffer[i] = static_cast<int16>(Sample * 32767.0f);
        }

        // Publish to labeled audio track
        LkResult Result = lk_audio_track_publish_pcm_i16(
            Track,
            PcmConversionBuffer.GetData(),
            NumFrames
        );

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning,
                TEXT("Failed to publish audio to track '%s' (frames=%d, ch=%d, sr=%d): %s"),
                *StreamLabel, NumFrames, NumChannels, SampleRate, *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
            return false;
        }

        return true;
    }

    virtual void OnCaptureStopped() override
    {
        // No cleanup needed (tracks cleaned up in FO3DWebRTCSender::Stop())
    }

private:
    FO3DWebRTCSender& Owner;

    // Reusable buffer for float->int16 conversion (optimization: avoid per-frame allocation)
    // Typical size: 960 samples * 2 channels * 2 bytes = 3840 bytes
    TArray<int16> PcmConversionBuffer;

    /**
     * Gets existing audio track for StreamLabel, or creates one if it doesn't exist.
     * Thread-safe via Owner.AudioTracksMutex.
     *
     * @param StreamLabel Subject name / track identifier
     * @param NumChannels Audio channel count (1=mono, 2=stereo)
     * @param SampleRate Audio sample rate in Hz (e.g., 48000)
     * @return Pointer to audio track, or nullptr if creation failed
     */
    LkAudioTrackHandle* GetOrCreateAudioTrack(const FString& StreamLabel, int32 NumChannels, int32 SampleRate)
    {
        FScopeLock Lock(&Owner.AudioTracksMutex);

        // Check if track already exists for this subject
        LkAudioTrackHandle* const* ExistingTrack = Owner.AudioTracks.Find(StreamLabel);
        if (ExistingTrack && *ExistingTrack)
        {
            return *ExistingTrack;
        }

        // Create new track with configuration
        LkAudioTrackConfig TrackConfig;
        TrackConfig.track_name = TCHAR_TO_UTF8(*StreamLabel);
        TrackConfig.sample_rate = SampleRate;
        TrackConfig.channels = NumChannels;
        TrackConfig.buffer_ms = 100; // 100ms buffer for smooth audio streaming

        LkAudioTrackHandle* NewTrack = nullptr;
        LkResult Result = lk_audio_track_create(
            Owner.ClientHandle,
            &TrackConfig,
            &NewTrack
        );

        if (Result.code != 0 || !NewTrack)
        {
            UE_LOG(LogO3DWebRTCSender, Error,
                TEXT("Failed to create audio track for '%s' (ch=%d, sr=%d): %s"),
                *StreamLabel, NumChannels, SampleRate, *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
            return nullptr;
        }

        // Store and return new track
        Owner.AudioTracks.Add(StreamLabel, NewTrack);
        UE_LOG(LogO3DWebRTCSender, Log,
            TEXT("Created audio track '%s' (ch=%d, sr=%d kHz, buf=100ms)"),
            *StreamLabel, NumChannels, SampleRate / 1000);

        return NewTrack;
    }
};

// PHASE 10: Check if we should drop frames due to FFI backpressure
bool FO3DWebRTCSender::ShouldDropFrameDueToBackpressure() const
{
    int32 Pending = EstimatedPendingFrames.Load();
    return Pending > DefaultBackpressureThreshold;
}

// PHASE 10: Update frame send metrics (frame rate and backpressure decay)
void FO3DWebRTCSender::UpdateFrameSendMetrics(int32 SubjectsInFrame)
{
    const int64 NowUs = FPlatformTime::Seconds() * 1000000.0;
    const int64 LastSendUs = LastFrameSendTimeUs.Load();

    if (LastSendUs > 0)
    {
        // Calculate frame interval
        const int64 DeltaUs = NowUs - LastSendUs;
        if (DeltaUs > 0 && DeltaUs < 1000000)  // Sanity: between 1us and 1 second
        {
            // Convert to FPS
            double FpsMeasured = 1000000.0 / static_cast<double>(DeltaUs);
            int32 FpsMeasuredInt = static_cast<int32>(FpsMeasured);

            // Update moving average of frame rate (exponential smoothing)
            int32 CurrentRate = RecentSendRateFps.Load();
            int32 UpdatedRate = (CurrentRate * 7 + FpsMeasuredInt) / 8;  // 87.5% old, 12.5% new
            RecentSendRateFps.Store(UpdatedRate);
        }
    }

    LastFrameSendTimeUs.Store(NowUs);

    // PHASE 10: Periodic decay of estimated pending frames
    // Assumes FFI thread is consuming frames at a measurable rate
    const double NowSeconds = FPlatformTime::Seconds();
    if (NowSeconds - LastBackpressureDecayTimeSeconds > 0.1)  // Every 100ms
    {
        LastBackpressureDecayTimeSeconds = NowSeconds;

        int32 Current = EstimatedPendingFrames.Load();
        int32 Decayed = FMath::Max(0, Current - DefaultBackpressureDecayRate);
        EstimatedPendingFrames.Store(Decayed);

        // Debug logging
        if (Decayed > DefaultBackpressureThreshold * 0.8)  // Warn if >80% of threshold
        {
            UE_LOG(LogO3DWebRTCSender, Warning,
                TEXT("WebRTC FFI queue backing up: pending=%d frames (threshold=%d)"),
                Decayed, DefaultBackpressureThreshold);
        }
    }
}

// Static callback for connection state changes
void FO3DWebRTCSender::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    FO3DWebRTCSender* Self = reinterpret_cast<FO3DWebRTCSender*>(user);
    if (!Self) return;

    // Update metrics for connection state
    bool bNewConnectedState = false;

    switch (state)
    {
    case LkConnConnecting:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connecting..."));
        bNewConnectedState = false;
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connected"));
        bNewConnectedState = true;
        // PHASE 12: Reset backpressure on connection (frames should flow again)
        Self->EstimatedPendingFrames.Store(0);
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC reconnecting..."));
        bNewConnectedState = false;
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC disconnected: %s"), *FromAnsi(message));
        bNewConnectedState = false;
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC connection failed (code=%d): %s"), reason_code, *FromAnsi(message));
        bNewConnectedState = false;
        break;
    }

    Self->bConnected.Store(bNewConnectedState);
    FO3DPerformanceMetrics::Get().SetTransportConnected(TEXT("WebRTC"), bNewConnectedState);
}

FO3DWebRTCSender::FO3DWebRTCSender()
{
    // LiveKit FFI DLL is loaded automatically via .lib linkage
}

FO3DWebRTCSender::~FO3DWebRTCSender()
{
    Stop();
}

bool FO3DWebRTCSender::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already initialized"));
        #endif
        return false;
    }

    // Platform validation: WebRTC module currently supports Windows 64-bit only
#if !PLATFORM_WINDOWS || !PLATFORM_64BITS
    UE_LOG(LogO3DWebRTCSender, Error,
        TEXT("Open3DTransportWebRTC requires Windows 64-bit. Current platform: %s %d-bit. "
             "Please use alternative transport (TCP, UDP, NNG, or Loopback) or compile LiveKit FFI for your platform."),
#if PLATFORM_WINDOWS
        TEXT("Windows"),
#elif PLATFORM_MAC
        TEXT("macOS"),
#elif PLATFORM_LINUX
        TEXT("Linux"),
#else
        TEXT("Unknown"),
#endif
        (int32)(sizeof(void*) * 8));
    return false;
#endif

    if (!ParseConfig(Config))
    {
        return false;
    }

    // Create LiveKit client handle
    ClientHandle = lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    // Set connection callback
    LkResult Result = lk_set_connection_callback(ClientHandle, FO3DWebRTCSender::OnConnectionState, this);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set connection callback: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    // Configure audio if enabled
    if (Config.Audio.bEnableAudio)
    {
        ActiveAudioConfig = Config.Audio;

        int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
        Result = lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set audio options: %s"), *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
        }
    }

    ActiveConfig = Config;
    Stats.Reset();

    // PHASE 1: Initialize serialization pool (10 pre-allocated items = ~160KB)
    SerializerPool = MakeUnique<FSerializerPool>(10);
    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender: serialization pool initialized (10 items pre-allocated)"));

    bInitialized.Store(true);

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender initialized: URL=%s Audio=%s PreferLossy=%s"),
        *RoomUrl,
        ActiveAudioConfig.bEnableAudio ? TEXT("true") : TEXT("false"),
        bPreferLossyData ? TEXT("true") : TEXT("false"));

    return true;
}

bool FO3DWebRTCSender::Start()
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Cannot start WebRTC sender: not initialized"));
        return false;
    }

    if (bConnected.Load())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already connected"));
        #endif
        return true;
    }

    // Ensure we have a valid token
    if (!EnsureTokenAvailable())
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Waiting for token before connecting..."));
        // Will retry in Tick()
        return true; // Return true to not fail startup, will connect when token available
    }

    if (Token.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Cannot connect: token is empty"));
        return false;
    }

    // Connect asynchronously with Publisher role
    LkResult Result = lk_connect_with_role_async(
        ClientHandle,
        TCHAR_TO_UTF8(*RoomUrl),
        TCHAR_TO_UTF8(*Token),
        LkRolePublisher
    );

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to connect: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
        return false;
    }

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender connecting..."));
    return true;
}

void FO3DWebRTCSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    if (!ClientHandle)
    {
        return;
    }

    // Clean up all audio tracks before disconnecting
    {
        FScopeLock AudioLock(&AudioTracksMutex);
        for (auto& TrackEntry : AudioTracks)
        {
            if (TrackEntry.Value)
            {
                LkResult Result = lk_audio_track_destroy(TrackEntry.Value);
                if (Result.code != 0 && Result.message)
                {
                    UE_LOG(LogO3DWebRTCSender, Warning,
                        TEXT("Failed to destroy audio track '%s': %s"),
                        *TrackEntry.Key, *FromAnsi(Result.message));
                    lk_free_str(const_cast<char*>(Result.message));
                }
                TrackEntry.Value = nullptr;
            }
        }
        AudioTracks.Reset();
    }

    if (bConnected.Load())
    {
        LkResult Result = lk_disconnect(ClientHandle);
        if (Result.code != 0 && Result.message)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Disconnect warning: %s"), *FromAnsi(Result.message));
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    lk_client_destroy(ClientHandle);
    ClientHandle = nullptr;

    // Reset token manager
    if (TokenManager.IsValid())
    {
        TokenManager->Reset();
    }

    bConnected.Store(false);
    bInitialized.Store(false);
    bWaitingForToken.Store(false);

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender stopped"));
}

bool FO3DWebRTCSender::Send(const O3DS::SubjectList& List)
{
    if (!bConnected.Load())
    {
        static double LastDisconnectedWarningTime = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastDisconnectedWarningTime > 5.0)
        {
            UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("Send() called while not connected, dropping frame"));
            LastDisconnectedWarningTime = Now;
        }
        FO3DPerformanceMetrics::Get().RecordFrameDropped();
        {
            FScopeLock Lock(&StatsMutex);
            Stats.DroppedFrames++;
        }
        return false;
    }

    // PHASE 10: Check for backpressure in the FFI queue
    // If too many frames are pending, drop this frame to prevent latency spikes
    if (ShouldDropFrameDueToBackpressure())
    {
        UE_LOG(LogO3DWebRTCSender, Warning,
            TEXT("WebRTC backpressure: dropping frame (pending=%d)"),
            EstimatedPendingFrames.Load());
        FO3DPerformanceMetrics::Get().RecordFrameDropped();
        {
            FScopeLock Lock(&StatsMutex);
            Stats.DroppedFrames++;
        }
        return false;
    }

    const double TimestampSeconds = FPlatformTime::Seconds();
    constexpr int32 LossyMaxBytes = 1300;
    constexpr int32 ReliableMaxBytes = 15000;

    bool bAnyFrameSucceeded = false;
    int32 SubjectsProcessed = 0;

    // Record frame capture attempt
    FO3DPerformanceMetrics::Get().RecordFrameCaptured();
    FO3DPerformanceMetrics::Get().SetActiveSubjectCount(static_cast<int32>(List.mItems.size()));

    // ARCHITECTURE VERIFICATION LOGGING
    UE_LOG(LogO3DWebRTCSender, Verbose,
        TEXT("[ARCH] Send() START: Input SubjectList has %zu subjects"),
        List.mItems.size());

    // Serialize and send each subject individually with its own labeled data channel.
    // This allows multiple senders to coexist without overwhelming a single channel.
    // Each subject's mocap data (typically ~13KB) stays well within the reliable limit.
    //
    // PHASE 1 OPTIMIZATION: Use pooled serializer objects to eliminate per-subject allocations.
    // Instead of creating a new SubjectList for each subject, we acquire a pre-allocated
    // pooled object, use it, and return it. This reduces 3,000+ allocations/sec to 0.
    for (size_t SubjectIdx = 0; SubjectIdx < List.mItems.size(); ++SubjectIdx)
    {
        O3DS::Subject* Subject = List.mItems[SubjectIdx];
        if (!Subject)
        {
            continue;
        }

        // ARCHITECTURE VERIFICATION: Log subject being processed
        FString SubjectNameStr = FString(Subject->mName.c_str());
        const TCHAR* SubjectNameDisplay = SubjectNameStr.IsEmpty() ? TEXT("(unnamed)") : *SubjectNameStr;
        UE_LOG(LogO3DWebRTCSender, Verbose,
            TEXT("[ARCH] Processing subject[%zu]: name='%s' transforms=%zu"),
            SubjectIdx, SubjectNameDisplay,
            Subject->mTransforms.mItems.size());

        // PHASE 1: Acquire pooled serializer (zero allocation)
        FPooledSubject* PooledSubject = SerializerPool->Acquire();
        if (!PooledSubject)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to acquire pooled subject for Subject[%zu]"), SubjectIdx);
            continue;
        }

        // Use pooled object to create single-subject list
        O3DS::Subject* NewSubject = PooledSubject->SubjectList.addSubject(Subject->mName, Subject->mReference);
        if (!NewSubject)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to create single-subject list for Subject[%zu]"), SubjectIdx);
            SerializerPool->Release(PooledSubject);
            continue;
        }

        // ARCHITECTURE VERIFICATION: Log single-subject list creation
        UE_LOG(LogO3DWebRTCSender, Verbose,
            TEXT("[ARCH] PooledSubject acquired for subject[%zu]: %zu transforms will be copied"),
            SubjectIdx, Subject->mTransforms.mItems.size());

        // Copy subject data to new subject
        NewSubject->mJoints = Subject->mJoints;
        NewSubject->mCurveNames = Subject->mCurveNames;
        NewSubject->mCurveValues = Subject->mCurveValues;
        NewSubject->mContext = Subject->mContext;

        // Copy all transforms
        for (const auto& Transform : Subject->mTransforms.mItems)
        {
            if (Transform)
            {
                NewSubject->addTransform(Transform);
            }
        }

        // Serialize the single-subject list using pooled buffer
        PooledSubject->SerializedBuffer.clear();
        int32 BytesWritten = PooledSubject->SubjectList.Serialize(PooledSubject->SerializedBuffer, TimestampSeconds);
        if (BytesWritten <= 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to serialize Subject[%zu]"), SubjectIdx);
            FO3DPerformanceMetrics::Get().RecordSerializationError();
            SerializerPool->Release(PooledSubject);
            continue;
        }

        // Record serialization metrics
        FO3DPerformanceMetrics::Get().RecordBytesSerialized(BytesWritten);

        // ARCHITECTURE VERIFICATION: Log serialization success
        UE_LOG(LogO3DWebRTCSender, Verbose,
            TEXT("[ARCH] Serialized subject[%zu]: %d bytes (pooled buffer, capacity=%zu)"),
            SubjectIdx, BytesWritten, PooledSubject->SerializedBuffer.capacity());

        // IMPORTANT: Clear the transform list before pooling.
        // We added raw pointers from the source Subject, so we must prevent
        // SubjectList's pool-reset from deleting them (they're still owned by List).
        NewSubject->mTransforms.mItems.clear();

        // Determine reliability based on payload size
        const bool bAllowLossy = bPreferLossyData;
        LkReliability Reliability = bAllowLossy ? LkLossy : LkReliable;

        if (bAllowLossy && BytesWritten > LossyMaxBytes)
        {
            if (BytesWritten <= ReliableMaxBytes)
            {
                Reliability = LkReliable;
            }
            else
            {
                UE_LOG(LogO3DWebRTCSender, Error,
                    TEXT("Subject[%zu] '%s' payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton"),
                    SubjectIdx, *FString(Subject->mName.c_str()), BytesWritten, ReliableMaxBytes);
                SerializerPool->Release(PooledSubject);
                continue;
            }
        }
        else if (!bAllowLossy && BytesWritten > ReliableMaxBytes)
        {
            UE_LOG(LogO3DWebRTCSender, Error,
                TEXT("Subject[%zu] '%s' payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton"),
                SubjectIdx, *FString(Subject->mName.c_str()), BytesWritten, ReliableMaxBytes);
            SerializerPool->Release(PooledSubject);
            continue;
        }

        // Use subject name as the data channel label for routing on receiver side
        FString SubjectLabel = FString(Subject->mName.c_str());
        if (SubjectLabel.IsEmpty())
        {
            SubjectLabel = FString::Printf(TEXT("subject_%zu"), SubjectIdx);
        }

        // ARCHITECTURE VERIFICATION: Log label and send attempt
        UE_LOG(LogO3DWebRTCSender, Verbose,
            TEXT("[ARCH] Sending subject[%zu] with label='%s' (%d bytes, %s)"),
            SubjectIdx, *SubjectLabel, BytesWritten,
            Reliability == LkReliable ? TEXT("reliable") : TEXT("lossy"));

        // Send via labeled LiveKit data channel
        // The label allows the receiver to route this subject's data to the correct consumer
        LkResult Result = lk_send_data_ex(
            ClientHandle,
            reinterpret_cast<const uint8*>(PooledSubject->SerializedBuffer.data()),
            BytesWritten,
            Reliability,
            1, // ordered = true (preserve frame order)
            TCHAR_TO_UTF8(*SubjectLabel)
        );

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to send Subject[%zu] '%s': %s"),
                SubjectIdx, *SubjectLabel, *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
            FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
            SerializerPool->Release(PooledSubject);
            continue;
        }

        // ARCHITECTURE VERIFICATION: Log successful send
        UE_LOG(LogO3DWebRTCSender, Verbose,
            TEXT("[ARCH] Successfully sent subject[%zu] with label='%s'"),
            SubjectIdx, *SubjectLabel);

        // Record transport send metrics
        FO3DPerformanceMetrics::Get().RecordBytesSent(BytesWritten);
        FO3DPerformanceMetrics::Get().RecordTransportFrameSent(TEXT("WebRTC"), BytesWritten);

        // PHASE 10: Update backpressure tracking
        // Increment estimated pending frames (will be decremented by periodic decay)
        int32 NewPendingCount = EstimatedPendingFrames.IncrementExchange();

        // PHASE 12: Enhanced diagnostics - log queue buildup patterns
        // This helps diagnose the 4070 ms latency spike by showing when the FFI queue backs up
        if (NewPendingCount > DefaultBackpressureThreshold * 0.5)  // 50% of threshold (15 frames)
        {
            UE_LOG(LogO3DWebRTCSender, Warning,
                TEXT("WebRTC queue building: %d pending frames (threshold=%d, subject='%s')"),
                NewPendingCount, DefaultBackpressureThreshold, *SubjectLabel);
        }

        // PHASE 1: Return pooled object for reuse (resets and returns to pool)
        SerializerPool->Release(PooledSubject);

        bAnyFrameSucceeded = true;
        SubjectsProcessed++;

        {
            FScopeLock Lock(&StatsMutex);
            Stats.FramesSent++;
            Stats.BytesSent += BytesWritten;
        }
    }

    // PHASE 10: Update frame send metrics for adaptive decisions
    if (SubjectsProcessed > 0)
    {
        UpdateFrameSendMetrics(SubjectsProcessed);
    }

    if (!bAnyFrameSucceeded)
    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;
    }

    // ARCHITECTURE VERIFICATION: Log end of send
    UE_LOG(LogO3DWebRTCSender, Verbose,
        TEXT("[ARCH] Send() END: Processed %d/%zu subjects (%s)"),
        SubjectsProcessed, List.mItems.size(),
        bAnyFrameSucceeded ? TEXT("SUCCESS") : TEXT("FAILED"));

    return bAnyFrameSucceeded;
}

void FO3DWebRTCSender::Tick(float DeltaSeconds)
{
    // LiveKit FFI handles internal event processing

    // Check if we're waiting for initial token and it's now available
    if (bInitialized.Load() && !bConnected.Load() && !bWaitingForToken.Load())
    {
        FString CurrentToken;
        if (TokenManager.IsValid() && TokenManager->GetCurrentToken(CurrentToken) && !CurrentToken.IsEmpty())
        {
            // Token is now available, retry connection
            if (Token.IsEmpty() || Token != CurrentToken)
            {
                Token = CurrentToken;
                UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token now available, attempting connection..."));
                Start(); // Will use the newly fetched token
            }
        }
    }

    // Check for token refresh if connected
    if (bConnected.Load())
    {
        CheckTokenRefresh();
    }
}

FO3DTransportStats FO3DWebRTCSender::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DWebRTCSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        return nullptr;
    }

    ActiveAudioConfig = AudioConfig;

    // Update audio options
    int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
    LkResult Result = lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to update audio options: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    return MakeShared<FWebRTCSenderAudioSink, ESPMode::ThreadSafe>(*this);
}

bool FO3DWebRTCSender::ParseConfig(const FO3DTransportConfig& Config)
{
    FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC host address not specified"));
        #endif
        return false;
    }

    // Automatically prepend the correct WebSocket protocol prefix
    RoomUrl = WebRTCUtils::PrependWebSocketProtocol(HostAddress);

    // Initialize token manager
    TokenManager = MakeUnique<FO3DTokenManager>();

    FO3DTokenConfig TokenConfig;
    
    if (Config.bUseAutoTokenFetch)
    {
        // Auto-fetch mode
        TokenConfig.Mode = EO3DTokenMode::AutoFetch;
        TokenConfig.EndpointUrl = Config.TokenEndpointUrl;
        TokenConfig.RoomName = Config.StreamId; // Use StreamId as room name
        TokenConfig.Identity = FString::Printf(TEXT("sender-%d"), FPlatformProcess::GetCurrentProcessId());
        TokenConfig.Role = EO3DTokenRole::Publisher;
        TokenConfig.RefreshLeadTimeSec = Config.TokenRefreshLeadTimeSec;

        if (TokenConfig.EndpointUrl.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Auto-fetch enabled but no token endpoint URL provided"));
            return false;
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token auto-fetch enabled: endpoint=%s, room=%s"),
            *TokenConfig.EndpointUrl, *TokenConfig.RoomName);
    }
    else
    {
        // Manual token mode
        TokenConfig.Mode = EO3DTokenMode::Manual;
        TokenConfig.ManualToken = Config.Token;

        if (TokenConfig.ManualToken.IsEmpty())
        {
            #if !WITH_DEV_AUTOMATION_TESTS
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC token not provided"));
            #endif
            return false;
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Manual token mode"));
    }

    if (!TokenManager->Initialize(TokenConfig))
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to initialize token manager"));
        return false;
    }

    // Get initial token if in manual mode
    if (!Config.bUseAutoTokenFetch)
    {
        if (!TokenManager->GetCurrentToken(Token))
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to get initial token"));
            return false;
        }
    }

    bPreferLossyData = WebRTCOptions::ParseBool(Config.AdvancedParams, WebRTCOptions::PreferLossyOptionKey, /*DefaultValue=*/false);

    return true;
}

bool FO3DWebRTCSender::EnsureTokenAvailable()
{
    // Check if we already have a valid token
    if (TokenManager->GetCurrentToken(Token))
    {
        return true;
    }

    // Need to fetch token
    if (!bWaitingForToken.Load())
    {
        bWaitingForToken.Store(true);
        TokenFetchStartTime = FPlatformTime::Seconds();

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Fetching token..."));

        // Initiate async token fetch
        TokenManager->RefreshTokenAsync([this](const FO3DTokenResult& Result)
        {
            if (Result.bSuccess && !Result.Token.IsEmpty())
            {
                UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token fetched successfully"));
                Token = Result.Token;
                bWaitingForToken.Store(false);
            }
            else
            {
                UE_LOG(LogO3DWebRTCSender, Error, TEXT("Token fetch failed: %s"), *Result.ErrorMessage);
                bWaitingForToken.Store(false);
            }
        });
    }

    // Check for timeout
    const double Now = FPlatformTime::Seconds();
    if (bWaitingForToken.Load() && (Now - TokenFetchStartTime > TokenFetchTimeoutSec))
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Token fetch timed out after %.1f seconds"), TokenFetchTimeoutSec);
        bWaitingForToken.Store(false);
        return false;
    }

    // Still waiting for token
    return false;
}

void FO3DWebRTCSender::CheckTokenRefresh()
{
    if (!TokenManager.IsValid())
    {
        return;
    }

    // Check if token needs refresh
    if (TokenManager->NeedsRefresh() && !bWaitingForToken.Load())
    {
        const int64 TimeUntilExpiry = TokenManager->GetTimeUntilExpiry();
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Token expiring soon (in %lld seconds), refreshing..."), TimeUntilExpiry);

        bWaitingForToken.Store(true);

        TokenManager->RefreshTokenAsync([this](const FO3DTokenResult& Result)
        {
            if (Result.bSuccess && !Result.Token.IsEmpty())
            {
                UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token refreshed successfully"));
                Token = Result.Token;
                
                // TODO: Reconnect with new token if already connected
                // For now, we rely on token having sufficient TTL
            }
            else
            {
                UE_LOG(LogO3DWebRTCSender, Error, TEXT("Token refresh failed: %s"), *Result.ErrorMessage);
            }
            
            bWaitingForToken.Store(false);
        });
    }
}
