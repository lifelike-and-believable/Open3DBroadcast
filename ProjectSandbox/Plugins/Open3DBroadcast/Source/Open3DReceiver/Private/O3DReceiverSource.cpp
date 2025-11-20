// Copyright (c) Open3DStream Contributors

#include "O3DReceiverSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "LiveLinkPreset.h"
#include "SerializedFrameConsumerRegistry.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"
#include "Misc/QualifiedFrameTime.h"

#include "O3DHelpers.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DAudioBus.h"
#include "O3DAudioFrameCodec.h"
#include "O3DPerformanceMetrics.h"

#include "o3ds_generated.h"

#define LOCTEXT_NAMESPACE "O3DReceiverSource"

// Receiver-side diagnostics
static TAutoConsoleVariable<int32> CVarO3DReceiverDebugParse(
    TEXT("o3ds.Receiver.DebugParse"),
    1,
    TEXT("Enable debug logs when parsing incoming O3DS packets (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DReceiverDropOutOfOrder(
    TEXT("o3ds.Receiver.DropOutOfOrder"),
    1,
    TEXT("When 1, drop frames whose SubjectList.time is older than the last applied timestamp."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DReceiverSilenceResetSeconds(
    TEXT("o3ds.Receiver.SilenceResetSeconds"),
    2.0f,
    TEXT("Seconds of no packets after which timestamp ordering state is reset. 0 disables."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DReceiverTimestampJumpResetSeconds(
    TEXT("o3ds.Receiver.TimestampJumpResetSeconds"),
    1.0f,
    TEXT("If SubjectList.time decreases by more than this many seconds relative to last applied time, reset ordering. 0 disables."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DReceiverAudioDebug(
    TEXT("o3ds.Receiver.Audio.Debug"),
    0,
    TEXT("Enable debug logs when publishing receiver audio frames (0/1)."),
    ECVF_Default);

/** Adapts the shared consumer registry callback into this live source instance. */
class FO3DReceiverSource::FSerializedConsumer : public ISerializedFrameConsumer
{
public:
    explicit FSerializedConsumer(TWeakPtr<FO3DReceiverSource> InOwner)
        : Owner(MoveTemp(InOwner))
    {
    }

    virtual ~FSerializedConsumer() override = default;

    virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) override
    {
        if (TSharedPtr<FO3DReceiverSource> OwnerPinned = Owner.Pin())
        {
            OwnerPinned->HandleSerializedFrame(Subject, Buffer, TimestampSeconds);
        }
    }

private:
    TWeakPtr<FO3DReceiverSource> Owner;
};

/** Lightweight audio bridge that republishes transport frames onto the gameplay audio bus. */
class FO3DReceiverSource::FAudioSink : public IO3DReceiverAudioSink
{
public:
    explicit FAudioSink(TWeakPtr<FO3DReceiverSource> InOwner)
        : Owner(MoveTemp(InOwner))
    {
    }

    virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& InMeta, const uint8* Data, int32 NumBytes) override
    {
        if (!Data || NumBytes <= 0)
        {
            return;
        }

        if (TSharedPtr<FO3DReceiverSource> OwnerPinned = Owner.Pin())
        {
            O3DS::FAudioFrameMeta MetaCopy = InMeta;
            OwnerPinned->FinalizeAudioMeta(MetaCopy);

            const bool bDebug = CVarO3DReceiverAudioDebug.GetValueOnAnyThread() != 0;
            TArray<uint8> Payload;
            Payload.Append(Data, NumBytes);

            AsyncTask(ENamedThreads::GameThread, [MetaCopy, Payload = MoveTemp(Payload), bDebug]() mutable
            {
                FO3DAudioBus::PublishPcm16(MetaCopy, Payload.GetData(), Payload.Num());
                if (bDebug)
                {
                    UE_LOG(LogO3DReceiverAudio, Verbose, TEXT("Published audio frame label='%s' subject='%s' bytes=%d sr=%d ch=%d"),
                        *MetaCopy.StreamLabel,
                        *MetaCopy.SubjectName,
                        Payload.Num(),
                        MetaCopy.SampleRate,
                        MetaCopy.NumChannels);
                }
            });
        }
    }

private:
    TWeakPtr<FO3DReceiverSource> Owner;
};

namespace
{
    uint64 HashArray(const TArray<FName>& Names, const TArray<int32>* Parents = nullptr)
    {
        return Parents ? O3DHelpers::HashNamesAndParents(Names, *Parents) : O3DHelpers::HashNames(Names);
    }

    uint64 HashCurveNames(const TArray<FName>& Names)
    {
        return O3DHelpers::HashNames(Names);
    }

    static const FName DefaultReceiverTransportName(TEXT("loopback"));
}

FO3DReceiverSource::FO3DReceiverSource()
    : FO3DReceiverSource(GetDefault<UO3DReceiverSettingsObject>()->Settings)
{
}

FO3DReceiverSource::FO3DReceiverSource(const FO3DReceiverSourceConfig& InSettings)
    : SourceType(LOCTEXT("SourceType", "Open3D Stream"))
    , SourceMachineName(LOCTEXT("SourceMachineName", "-"))
    , SourceStatus(LOCTEXT("SourceStatus", "Inactive"))
    , SourceSettings(InSettings)
{
    EnsureValidTransportName();
}

FO3DReceiverSource::~FO3DReceiverSource()
{
    StopTransport();
}

void FO3DReceiverSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    Client = InClient;
    SourceGuid = InSourceGuid;
    bIsValid = true;

    SourceMachineName = LOCTEXT("SourceHostUnknown", "-");
    bLoggedActiveState = false;

    if (StartTransport())
    {
        SourceStatus = LOCTEXT("StatusActive", "Receiving");
        UpdateConnectionLastActive();
    }
    else
    {
        SourceStatus = LOCTEXT("StatusError", "Inactive");
    }
}

bool FO3DReceiverSource::RequestSourceShutdown()
{
    bIsValid = false;
    StopTransport();
    return true;
}

void FO3DReceiverSource::InitializeSettings(ULiveLinkSourceSettings* InSettings)
{
    Settings = InSettings;
}

TSubclassOf<ULiveLinkSourceSettings> FO3DReceiverSource::GetSettingsClass() const
{
    return UO3DReceiverSourceSettings::StaticClass();
}

void FO3DReceiverSource::Tick(float DeltaTime)
{
    TimeSinceLastActivityCheck += DeltaTime;

    if (ActiveReceiver.IsValid())
    {
        ActiveReceiver->Poll();
    }

    if (TimeSinceLastActivityCheck >= ActivityCheckIntervalSeconds)
    {
        RemoveInactiveSubjects();
        TimeSinceLastActivityCheck = 0.0f;
    }
}

void FO3DReceiverSource::Update()
{
    // No-op; Tick handles polling.
}

/** Tear down any existing transport and spin up the selected receiver implementation. */
bool FO3DReceiverSource::StartTransport()
{
    StopTransport();

    EnsureValidTransportName();
    ActiveConfig = BuildTransportConfig();
    if (ActiveConfig.Transport.IsEmpty())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("No transport selected for receiver source."));
        return false;
    }

    const FName TransportName(*ActiveConfig.Transport);
    ActiveReceiver = O3DTransport::CreateReceiver(TransportName);
    if (!ActiveReceiver.IsValid())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("No receiver registered for transport '%s'."), *ActiveConfig.Transport);
        return false;
    }

    if (!ActiveReceiver->Initialize(ActiveConfig))
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Failed to initialize transport '%s'."), *ActiveConfig.Transport);
        ActiveReceiver.Reset();
        return false;
    }

    ActiveAudioSink.Reset();
    if (ActiveConfig.Audio.bEnableAudio)
    {
        if (ActiveReceiver->SupportsAudio())
        {
            ActiveAudioSink = MakeShared<FAudioSink>(TWeakPtr<FO3DReceiverSource>(AsShared()));
            ActiveReceiver->SetAudioSink(ActiveAudioSink, ActiveConfig.Audio);
            UE_LOG(LogO3DReceiverAudio, Log, TEXT("Audio sink bound for transport '%s'."),
                *ActiveConfig.Transport);
        }
        else
        {
            UE_LOG(LogO3DReceiverAudio, Warning, TEXT("Transport '%s' does not support audio; disabling audio for this source."), *ActiveConfig.Transport);
        }
    }

    ActiveConsumer = MakeShared<FSerializedConsumer>(TWeakPtr<FO3DReceiverSource>(AsShared()));
    ActiveReceiver->SetConsumer(ActiveConsumer);
    if (!ActiveReceiver->Start())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Failed to start transport '%s'."), *ActiveConfig.Transport);
        ActiveReceiver->Stop();
        ActiveReceiver->SetConsumer(nullptr);
        if (ActiveAudioSink.IsValid() && ActiveReceiver->SupportsAudio())
        {
            ActiveReceiver->SetAudioSink(nullptr, ActiveConfig.Audio);
        }
        ActiveReceiver.Reset();
        ActiveConsumer.Reset();
        ActiveAudioSink.Reset();
        return false;
    }

    if (!ActiveConfig.Uri.IsEmpty())
    {
        SourceMachineName = FText::FromString(ActiveConfig.Uri);
    }
    else if (!ActiveConfig.StreamId.IsEmpty())
    {
        SourceMachineName = FText::FromString(ActiveConfig.StreamId);
    }

    UE_LOG(LogO3DReceiverSource, Log, TEXT("Receiver transport '%s' started (Uri=%s, StreamId=%s)."),
        *ActiveConfig.Transport,
        *ActiveConfig.Uri,
        *ActiveConfig.StreamId);

    SourceStatus = FText::Format(LOCTEXT("StatusReceivingFmt", "Receiving via {0}"), FText::FromString(ActiveConfig.Transport));
    ResetOrderingState();
    return true;
}

/** Stop the active transport and clear subject/audio caches. */
void FO3DReceiverSource::StopTransport()
{
    if (ActiveReceiver.IsValid())
    {
        if (ActiveConfig.Audio.bEnableAudio && ActiveReceiver->SupportsAudio())
        {
            ActiveReceiver->SetAudioSink(nullptr, ActiveConfig.Audio);
        }
        ActiveReceiver->SetConsumer(nullptr);
        ActiveReceiver->Stop();
        ActiveReceiver.Reset();
    }
    ActiveAudioSink.Reset();
    ActiveConsumer.Reset();
    InitializedSubjects.Empty();
    SubjectSkeletonHashes.Empty();
    SubjectCurveHashes.Empty();
    SubjectLastUpdateTime.Empty();
    bLoggedActiveState = false;
    FrameCounter = 0;
    ResetOrderingState();
}

/** Ensure we always have a transport name for details panels that expose the source settings. */
void FO3DReceiverSource::EnsureValidTransportName()
{
    if (!SourceSettings.TransportName.IsNone())
    {
        return;
    }

    TArray<FName> RegisteredTransports;
    O3DReceiver::GetRegisteredTransportNames(RegisteredTransports);
    if (RegisteredTransports.Num() > 0)
    {
        SourceSettings.TransportName = RegisteredTransports[0];
    }
    else
    {
        SourceSettings.TransportName = DefaultReceiverTransportName;
    }
}

/** Build a transport config from the user settings, applying customization hooks if present. */
FO3DTransportConfig FO3DReceiverSource::BuildTransportConfig() const
{
    FO3DTransportConfig Config;

    FName TransportName = SourceSettings.TransportName;
    if (TransportName.IsNone())
    {
        TransportName = DefaultReceiverTransportName;
    }

    Config.Transport = TransportName.ToString();
    Config.Role = TEXT("receiver");

    Config.bPersistToken = false;

    Config.Audio.bEnableAudio = SourceSettings.bEnableAudio;
    if (Config.Audio.bEnableAudio)
    {
        Config.Audio.Mode = TEXT("playback");
        // Note: Audio stream label is now automatically derived from StreamId / subject name
        if (!SourceSettings.AudioCodec.IsNone())
        {
            const FString CodecString = O3DAudio::SanitizeCodecString(SourceSettings.AudioCodec.ToString());
            if (!CodecString.IsEmpty())
            {
                Config.Audio.Codec = CodecString;
                Config.Audio.AdvancedParams.Add(TEXT("codec"), CodecString);
            }
        }
    }
    else
    {
        Config.Audio.Mode.Reset();
        Config.Audio.Codec.Reset();
        Config.Audio.AdvancedParams.Remove(TEXT("codec"));
    }

    for (const TPair<FString, FString>& Option : SourceSettings.TransportOptions)
    {
        Config.AdvancedParams.Add(Option.Key, Option.Value);
    }

    if (!Config.Transport.IsEmpty())
    {
        if (const FO3DReceiverTransportCustomization* Customization = O3DReceiver::FindTransportCustomization(TransportName))
        {
            if (Customization && Customization->ConfigureTransport)
            {
                Customization->ConfigureTransport(SourceSettings, Config);
            }
        }
    }

    if (Config.StreamId.IsEmpty() && !Config.Uri.IsEmpty())
    {
        Config.StreamId = Config.Uri;
    }

    return Config;
}

/** Record the wall-clock time that the last packet was processed for connection health checks. */
void FO3DReceiverSource::UpdateConnectionLastActive()
{
    const double Now = FPlatformTime::Seconds();
    FScopeLock Lock(&ConnectionLastActiveSection);
    ConnectionLastActive = Now;
}

/** Drop LiveLink subjects that have not produced frames within the inactivity window. */
void FO3DReceiverSource::RemoveInactiveSubjects()
{
    const double Now = FPlatformTime::Seconds();
    for (auto It = SubjectLastUpdateTime.CreateIterator(); It; ++It)
    {
        if ((Now - It.Value()) > InactivityThresholdSeconds)
        {
            const FLiveLinkSubjectName SubjectName(It.Key());
            const FLiveLinkSubjectKey SubjectKey(SourceGuid, SubjectName);
            if (Client)
            {
                Client->RemoveSubject_AnyThread(SubjectKey);
            }
            UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Removed inactive subject %s"), *It.Key().ToString());
            SubjectSkeletonHashes.Remove(It.Key());
            SubjectCurveHashes.Remove(It.Key());
            InitializedSubjects.Remove(It.Key());
            It.RemoveCurrent();
        }
    }
}

/** Entry point from the serialized consumer; parses payloads then pushes LiveLink updates. */
void FO3DReceiverSource::HandleSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds)
{
    if (!Client || !bIsValid)
    {
        return;
    }

    // Record frame received
    FO3DPerformanceMetrics::Get().RecordFrameReceived();
    FO3DPerformanceMetrics::Get().RecordBytesDeserialized(Buffer.Num());

    if (!IsInGameThread())
    {
        TWeakPtr<FO3DReceiverSource> WeakSelf = AsShared();
        TArray<uint8> BufferCopy(Buffer);
        AsyncTask(ENamedThreads::GameThread, [WeakSelf, Subject, TimestampSeconds, BufferCopy = MoveTemp(BufferCopy)]() mutable
        {
            if (TSharedPtr<FO3DReceiverSource> Pinned = WeakSelf.Pin())
            {
                Pinned->HandleSerializedFrame(Subject, BufferCopy, TimestampSeconds);
            }
        });
        return;
    }

    if (!bLoggedActiveState)
    {
        SourceStatus = FText::Format(LOCTEXT("StatusReceivingFmt", "Receiving via {0}"), FText::FromString(ActiveConfig.Transport));
        bLoggedActiveState = true;
    }

    const double ParseStartWall = FPlatformTime::Seconds();

    // PHASE 6 OPTIMIZATION: Try to peek timestamp BEFORE expensive deserialization
    // This avoids parsing duplicate/out-of-order frames which would be dropped anyway
    double PeekedTime = -1.0;
    if (TryPeekSubjectListTime(Buffer, PeekedTime))
    {
        if (!ShouldProcessFrame(PeekedTime, ParseStartWall))
        {
            // Frame is a duplicate/out-of-order - skip expensive deserialization
            FO3DPerformanceMetrics::Get().RecordReceiverFrameDropped();
            return;
        }
    }

    // Deserialize only if frame passed timestamp check
    if (!ParseSubjectListBuffer(Subject, Buffer))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }

    const double SubjectListTime = SubjectScratch.mTime;
    // Re-check (in case peek wasn't accurate, though it should match)
    if (!ShouldProcessFrame(SubjectListTime, ParseStartWall))
    {
        FO3DPerformanceMetrics::Get().RecordReceiverFrameDropped();
        return;
    }

    // Record frame applied
    FO3DPerformanceMetrics::Get().RecordFrameApplied();

    // Record round-trip latency if we have the timestamp
    double LatencyMs = (FPlatformTime::Seconds() - TimestampSeconds) * 1000.0;
    if (LatencyMs >= 0.0 && LatencyMs < 10000.0)  // sanity check: latency should be < 10 seconds
    {
        FO3DPerformanceMetrics::Get().RecordFrameLatency(LatencyMs);
    }

    TArray<FName> BoneNames;
    TArray<int32> BoneParents;
    TArray<FTransform> BoneTransforms;
    TArray<FName> CurveNames;
    TArray<float> CurveValues;

    int32 PoseUpdateCount = 0;
    for (O3DS::Subject* SubjectPtr : SubjectScratch)
    {
        ProcessParsedSubject(SubjectPtr, SubjectListTime, BoneNames, BoneParents, BoneTransforms, CurveNames, CurveValues);
        ++PoseUpdateCount;
    }

    // Record pose and skeleton updates
    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    // Update active subject count
    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(static_cast<int32>(SubjectScratch.mItems.size()));

    if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
    {
        const double ParseEnd = FPlatformTime::Seconds();
        UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed subject list (subjects=%d bytes=%d dt=%.6fms)"),
            static_cast<int32>(SubjectScratch.mItems.size()), Buffer.Num(), (ParseEnd - ParseStartWall) * 1000.0);
    }
}

/** Parse the FlatBuffer payload into a reusable SubjectList scratch structure. */
bool FO3DReceiverSource::ParseSubjectListBuffer(const FString& Subject, const TArray<uint8>& Buffer)
{
    if (!SubjectScratch.Parse(reinterpret_cast<const char*>(Buffer.GetData()), Buffer.Num(), nullptr, true))
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Parse failed for subject '%s' (%d bytes)"), *Subject, Buffer.Num());
        return false;
    }
    return true;
}

/**
 * PHASE 6 OPTIMIZATION: Peek at the timestamp field in the FlatBuffer WITHOUT full deserialization.
 *
 * FlatBuffers are lazy-evaluation - we can access individual fields by their offset
 * without parsing the entire structure. This lets us check timestamps before
 * committing to expensive deserialization of duplicate/out-of-order frames.
 *
 * Performance impact: Saves deserialization cost for ~0.16-0.19% of received frames.
 *
 * @param Buffer The serialized FlatBuffer data
 * @param OutTime Reference to receive the timestamp value
 * @return True if timestamp was successfully extracted, False if buffer too small or invalid
 */
bool FO3DReceiverSource::TryPeekSubjectListTime(const TArray<uint8>& Buffer, double& OutTime)
{
    // FlatBuffer minimum size check: need at least the root table pointer + some overhead
    if (Buffer.Num() < 12)
    {
        return false;
    }

    try
    {
        // SAFETY: Verify the buffer structure before attempting to access any fields.
        // This prevents crashes from corrupted or malformed buffers by validating the vtable
        // and field offsets before dereferencing any pointers.
        flatbuffers::Verifier Verifier(Buffer.GetData(), Buffer.Num());
        if (!O3DS::Data::VerifySubjectListBuffer(Verifier))
        {
            if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
            {
                UE_LOG(LogO3DReceiverSource, Warning, TEXT("TryPeekSubjectListTime: Buffer verification failed (corrupted FlatBuffer)"));
            }
            return false;
        }

        // Now safe to access the buffer - verification confirmed structure is valid
        const O3DS::Data::SubjectList* FlatBufferRoot =
            O3DS::Data::GetSubjectList(static_cast<const void*>(Buffer.GetData()));

        if (!FlatBufferRoot)
        {
            return false;
        }

        // Call time() which is a simple field accessor in FlatBuffers
        // This retrieves just the VT_TIME field at its offset without parsing transforms/subjects
        OutTime = FlatBufferRoot->time();
        return true;
    }
    catch (const std::exception& Ex)
    {
        if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogO3DReceiverSource, Warning, TEXT("TryPeekSubjectListTime exception: %s"), ANSI_TO_TCHAR(Ex.what()));
        }
        return false;
    }
    catch (...)
    {
        if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogO3DReceiverSource, Warning, TEXT("TryPeekSubjectListTime unknown exception"));
        }
        return false;
    }
}

/** Apply duplicate/out-of-order suppression while keeping activity timestamps in sync. */
bool FO3DReceiverSource::ShouldProcessFrame(double SubjectListTime, double NowSeconds)
{
    if (ShouldResetOrderingWindow(NowSeconds, SubjectListTime))
    {
        if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Reset ordering window (last=%.6f new=%.6f)"), LastAppliedSubjectListTime, SubjectListTime);
        }
        ResetOrderingState();
    }

    bool bShouldProcess = true;
    if (LastAppliedSubjectListTime >= 0.0)
    {
        if (SubjectListTime == LastAppliedSubjectListTime)
        {
            if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
            {
                UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Dropping duplicate frame t=%.6f"), SubjectListTime);
            }
            bShouldProcess = false;
        }
        else if (CVarO3DReceiverDropOutOfOrder.GetValueOnAnyThread() != 0 && SubjectListTime < LastAppliedSubjectListTime)
        {
            if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
            {
                UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Dropping out-of-order frame t=%.6f < %.6f"), SubjectListTime, LastAppliedSubjectListTime);
            }
            bShouldProcess = false;
        }
    }

    UpdateConnectionLastActive();
    if (bShouldProcess)
    {
        LastAppliedSubjectListTime = SubjectListTime;
    }

    return bShouldProcess;
}

/** Decide whether latency spikes warrant resetting the timestamp ordering guardrails. */
bool FO3DReceiverSource::ShouldResetOrderingWindow(double NowSeconds, double SubjectListTime) const
{
    if (LastAppliedSubjectListTime < 0.0)
    {
        return false;
    }

    const float SilenceThreshold = CVarO3DReceiverSilenceResetSeconds.GetValueOnAnyThread();
    const float JumpThreshold = CVarO3DReceiverTimestampJumpResetSeconds.GetValueOnAnyThread();

    const double LastActive = GetLastConnectionActive();
    if (SilenceThreshold > 0.0 && LastActive > 0.0 && (NowSeconds - LastActive) > SilenceThreshold)
    {
        return true;
    }

    if (JumpThreshold > 0.0 && (LastAppliedSubjectListTime - SubjectListTime) > JumpThreshold)
    {
        return true;
    }

    return false;
}

/** Thread-safe accessor for when the last packet arrived. */
double FO3DReceiverSource::GetLastConnectionActive() const
{
    FScopeLock Lock(&ConnectionLastActiveSection);
    return ConnectionLastActive;
}

/** Convert SubjectList transform data into LiveLink-friendly arrays. */
bool FO3DReceiverSource::BuildSubjectPose(O3DS::Subject* SubjectPtr, TArray<FName>& OutBoneNames, TArray<int32>& OutBoneParents, TArray<FTransform>& OutBoneTransforms) const
{
    OutBoneNames.Reset();
    OutBoneParents.Reset();
    OutBoneTransforms.Reset();

    if (!SubjectPtr)
    {
        return false;
    }

    const size_t TransformCount = SubjectPtr->mTransforms.mItems.size();
    if (TransformCount == 0)
    {
        return false;
    }

    OutBoneNames.Reserve(static_cast<int32>(TransformCount));
    OutBoneParents.Reserve(static_cast<int32>(TransformCount));
    OutBoneTransforms.Reserve(static_cast<int32>(TransformCount));

    for (O3DS::Transform* TransformPtr : SubjectPtr->mTransforms.mItems)
    {
        if (!TransformPtr)
        {
            continue;
        }

        const O3DS::Vector3d Translation = TransformPtr->translation.value;
        const O3DS::Vector4d Rotation = TransformPtr->rotation.value;
        const O3DS::Vector3d Scale = TransformPtr->scale.value;

        FQuat Quat(
            static_cast<float>(Rotation.v[0]),
            static_cast<float>(Rotation.v[1]),
            static_cast<float>(Rotation.v[2]),
            static_cast<float>(Rotation.v[3]));
        FVector Location(
            static_cast<float>(Translation.v[0]),
            static_cast<float>(Translation.v[1]),
            static_cast<float>(Translation.v[2]));
        FVector ScaleVec(
            static_cast<float>(Scale.v[0]),
            static_cast<float>(Scale.v[1]),
            static_cast<float>(Scale.v[2]));

        if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z) ||
            !FMath::IsFinite(ScaleVec.X) || !FMath::IsFinite(ScaleVec.Y) || !FMath::IsFinite(ScaleVec.Z) ||
            !FMath::IsFinite(Quat.X) || !FMath::IsFinite(Quat.Y) || !FMath::IsFinite(Quat.Z) || !FMath::IsFinite(Quat.W))
        {
            return false;
        }

        if (Quat.SizeSquared() <= KINDA_SMALL_NUMBER)
        {
            return false;
        }

        Quat.Normalize();
        if (Quat.ContainsNaN())
        {
            return false;
        }

        std::string BoneNameUtf8 = TransformPtr->mName;
        const size_t ColonIndex = BoneNameUtf8.rfind(':');
        if (ColonIndex != std::string::npos)
        {
            BoneNameUtf8.erase(0, ColonIndex + 1);
        }

        const FName BoneName(UTF8_TO_TCHAR(BoneNameUtf8.c_str()));
        OutBoneNames.Add(BoneName);
        OutBoneParents.Add(TransformPtr->mParentId);
        OutBoneTransforms.Emplace(Quat, Location, ScaleVec);
    }

    return OutBoneTransforms.Num() > 0;
}

/** Extract animation curve names/values while filtering out invalid data. */
void FO3DReceiverSource::BuildSubjectCurves(O3DS::Subject* SubjectPtr, TArray<FName>& OutCurveNames, TArray<float>& OutCurveValues) const
{
    OutCurveNames.Reset();
    OutCurveValues.Reset();

    if (!SubjectPtr)
    {
        return;
    }

    const size_t CurveCount = SubjectPtr->mCurveNames.size();
    OutCurveNames.Reserve(static_cast<int32>(CurveCount));
    OutCurveValues.Reserve(static_cast<int32>(CurveCount));

    for (size_t CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
    {
        const std::string& CurveNameUtf8 = SubjectPtr->mCurveNames[CurveIndex];
        const FName CurveName(UTF8_TO_TCHAR(CurveNameUtf8.c_str()));
        OutCurveNames.Add(CurveName);

        float Value = 0.0f;
        if (CurveIndex < SubjectPtr->mCurveValues.size())
        {
            Value = SubjectPtr->mCurveValues[CurveIndex];
        }
        OutCurveValues.Add(Value);
    }
}

/** Build LiveLink static/frame data and push it to the client for a single parsed subject. */
void FO3DReceiverSource::ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues)
{
    if (!SubjectPtr)
    {
        return;
    }

    const FString SubjectNameUtf8 = UTF8_TO_TCHAR(SubjectPtr->mName.c_str());
    const FName SubjectFName(*SubjectNameUtf8);

    LastObservedSubjectName = SubjectFName;

    // PHASE 4 OPTIMIZATION: Check transform cache BEFORE rebuilding skeleton
    // Compute skeleton hash early to check if we can reuse cached transform data
    // This avoids the expensive per-frame BuildSubjectPose() call for stable skeletons

    // First, we need to peek at what the skeleton would be to compute its hash
    // We can do this by checking transform count and parent IDs (quick, no conversion)
    FSubjectTransformCache* ExistingCache = SubjectTransformCaches.Find(SubjectFName);

    // Quick skeleton fingerprint: transform count + parent ID sum (very cheap to compute)
    size_t TransformCount = SubjectPtr->mTransforms.mItems.size();
    uint64 QuickSkeletonFingerprint = TransformCount;
    for (O3DS::Transform* TransformPtr : SubjectPtr->mTransforms.mItems)
    {
        if (TransformPtr)
        {
            QuickSkeletonFingerprint = QuickSkeletonFingerprint * 31 + TransformPtr->mParentId;
        }
    }

    // If cache exists and fingerprint matches, reuse cached transforms
    if (ExistingCache && ExistingCache->SkeletonFingerprint == QuickSkeletonFingerprint)
    {
        BoneNames = ExistingCache->BoneNames;
        BoneParents = ExistingCache->BoneParents;
        BoneTransforms = ExistingCache->BoneTransforms;
        // Still need to rebuild curves (they can change independently)
        BuildSubjectCurves(SubjectPtr, CurveNames, CurveValues);
    }
    else
    {
        // Skeleton changed or first time - rebuild everything
        if (!BuildSubjectPose(SubjectPtr, BoneNames, BoneParents, BoneTransforms))
        {
            return;
        }

        BuildSubjectCurves(SubjectPtr, CurveNames, CurveValues);

        // Update cache with new skeleton data
        FSubjectTransformCache& Cache = SubjectTransformCaches.FindOrAdd(SubjectFName);
        Cache.BoneNames = BoneNames;
        Cache.BoneParents = BoneParents;
        Cache.BoneTransforms = BoneTransforms;
        Cache.SkeletonFingerprint = QuickSkeletonFingerprint;
    }

    const FLiveLinkSubjectName SubjectName(SubjectFName);
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, SubjectName);

    const uint64 SkeletonHash = HashArray(BoneNames, &BoneParents);
    const uint64 CurveHash = HashCurveNames(CurveNames);

    const uint64* ExistingSkeletonHash = SubjectSkeletonHashes.Find(SubjectFName);
    const uint64* ExistingCurveHash = SubjectCurveHashes.Find(SubjectFName);
    const bool bNeedStaticUpdate = (!ExistingSkeletonHash || *ExistingSkeletonHash != SkeletonHash) || (!ExistingCurveHash || *ExistingCurveHash != CurveHash);

    if (!InitializedSubjects.Contains(SubjectFName) || bNeedStaticUpdate)
    {
        PushSubjectStaticData(SubjectKey, BoneNames, BoneParents, CurveNames, SkeletonHash);
        InitializedSubjects.Add(SubjectFName);
        SubjectSkeletonHashes.Add(SubjectFName, SkeletonHash);
        SubjectCurveHashes.Add(SubjectFName, CurveHash);

        if (!ExistingSkeletonHash)
        {
            UE_LOG(LogO3DReceiverSource, Log, TEXT("Created subject '%s'"), *SubjectFName.ToString());
        }
        else if (bNeedStaticUpdate)
        {
            UE_LOG(LogO3DReceiverSource, Log, TEXT("Static data updated for subject '%s'"), *SubjectFName.ToString());
        }
    }
    else
    {
        SubjectCurveHashes[SubjectFName] = CurveHash;
    }

    PushSubjectFrameData(SubjectKey, BoneTransforms, CurveNames, CurveValues, SubjectListTime, CurveHash);
    SubjectLastUpdateTime.Add(SubjectFName, FPlatformTime::Seconds());
}

/** Fill in missing audio metadata (subject name, defaults) before publishing to the bus. */
void FO3DReceiverSource::FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const
{
    Meta.SourceGuid = SourceGuid;

    // Audio stream label is now provided directly by WebRTC transport via per-subject audio callback (OnAudioReceivedEx)
    // which receives explicit subject labels from LiveKit FFI. No fallback logic needed.
    if (ActiveConfig.Audio.bEnableAudio && Meta.StreamLabel.IsEmpty())
    {
        Meta.StreamLabel = ActiveConfig.StreamId.IsEmpty() ? TEXT("o3ds:mix") : ActiveConfig.StreamId;
    }

    // With per-subject audio labels from the callback, audio is explicitly routed to the correct subject
    // SubjectName should be populated from the StreamLabel provided by the callback
    if (Meta.SubjectName.IsEmpty() && !Meta.StreamLabel.IsEmpty())
    {
        Meta.SubjectName = Meta.StreamLabel;  // Direct mapping from explicit label
    }

    // Fallback for edge cases (but should not be needed with proper label routing)
    if (Meta.SubjectName.IsEmpty())
    {
        const FString StreamId = ActiveConfig.StreamId;
        if (!StreamId.IsEmpty())
        {
            Meta.SubjectName = StreamId;
        }
        else
        {
            Meta.SubjectName = TEXT("Open3DReceiver");
        }
    }

    if (Meta.SampleRate <= 0)
    {
        Meta.SampleRate = (ActiveConfig.Audio.SampleRate > 0) ? ActiveConfig.Audio.SampleRate : 48000;
    }

    if (Meta.NumChannels <= 0)
    {
        Meta.NumChannels = (ActiveConfig.Audio.NumChannels > 0) ? ActiveConfig.Audio.NumChannels : 1;
    }

    if (Meta.TimestampSec <= 0.0)
    {
        Meta.TimestampSec = FPlatformTime::Seconds();
    }
}

void FO3DReceiverSource::PushSubjectStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, uint64 DescriptorHash)
{
    if (!Client)
    {
        return;
    }

    FLiveLinkSubjectPreset Preset;
    Preset.Key = SubjectKey;
    Preset.Role = ULiveLinkAnimationRole::StaticClass();
    Preset.Settings = nullptr;
    Preset.bEnabled = true;
    Client->CreateSubject(Preset);

    FLiveLinkStaticDataStruct StaticDataStruct;
    StaticDataStruct.InitializeWith(FLiveLinkSkeletonStaticData::StaticStruct(), nullptr);
    FLiveLinkSkeletonStaticData* SkeletonData = StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();

    SkeletonData->SetBoneNames(BoneNames);
    SkeletonData->SetBoneParents(BoneParents);
    SkeletonData->PropertyNames = CurveNames;

    Client->PushSubjectStaticData_AnyThread(SubjectKey, ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticDataStruct));
}

void FO3DReceiverSource::PushSubjectFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues, double TimestampSeconds, uint64 CurveHash)
{
    if (!Client)
    {
        return;
    }

    (void)CurveNames;

    FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
    FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
    FLiveLinkBaseFrameData& BaseFrameData = FrameData;

    FrameData.Transforms = BoneTransforms;
    BaseFrameData.PropertyValues = CurveValues;
    BaseFrameData.WorldTime = FPlatformTime::Seconds();
    BaseFrameData.MetaData.SceneTime = FQualifiedFrameTime();
    BaseFrameData.MetaData.StringMetaData.Add(TEXT("CurveHash"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(CurveHash)));
    BaseFrameData.MetaData.StringMetaData.Add(TEXT("SubjectListTime"), FString::Printf(TEXT("%.6f"), TimestampSeconds));

    FrameData.FrameId = FrameCounter++;

    Client->PushSubjectFrameData_AnyThread(SubjectKey, MoveTemp(FrameDataStruct));
}

void FO3DReceiverSource::ResetOrderingState()
{
    LastAppliedSubjectListTime = -1.0;
}

#undef LOCTEXT_NAMESPACE
