// Copyright (c) Open3DStream Contributors

#include "O3DReceiverSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "LiveLinkPreset.h"
#include "LiveLinkSubjectSettings.h"
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
#include "o3ds/sequencing.h"
#include "o3ds/predict/linear_predictor.h"

#include <utility>

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

// C1: receiver-side concealment (roadmap doc §5/C1). Defaults match
// ConcealmentConfig's own defaults (see concealment.h) - exposed as cvars so
// they can be tuned per-deployment without a rebuild, per the roadmap's "Open
// decisions" note that these values need real-world/live-LiveLink tuning.
static TAutoConsoleVariable<int32> CVarO3DReceiverConcealmentEnabled(
    TEXT("o3ds.Receiver.Concealment.Enabled"),
    1,
    TEXT("Enable receiver-side concealment (predict/hold synthetic frames on a gap) for gated (A2) frames (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DReceiverConcealmentStarvationMs(
    TEXT("o3ds.Receiver.Concealment.StarvationThresholdMs"),
    50.0f,
    TEXT("Gap since the last real frame (ms) beyond which concealment starts synthesizing, instead of leaving small gaps to LiveLink's own interpolation."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DReceiverConcealmentHorizonMs(
    TEXT("o3ds.Receiver.Concealment.MaxHorizonMs"),
    150.0f,
    TEXT("Stop extrapolating and hold after this many ms of continuous concealment with no real frame."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DReceiverConcealmentCorrectionMs(
    TEXT("o3ds.Receiver.Concealment.CorrectionWindowMs"),
    100.0f,
    TEXT("Blend from the last synthesized pose toward the resumed real trajectory over this many ms after a gap recovers, instead of snapping. 0 disables correction blending."),
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

    // C1: PoseSample <-> LiveLink transform/curve conversion. PoseSample
    // stores rotations as (x,y,z,w) doubles (O3DS::Quat = Vector4d), matching
    // FQuat's own component order, so no component reshuffling is needed.
    O3DS::PoseSample BuildPoseSampleFromLiveLink(double PresentationTimeSeconds, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues)
    {
        O3DS::PoseSample Sample;
        Sample.t = PresentationTimeSeconds;

        Sample.translations.reserve(BoneTransforms.Num());
        Sample.rotations.reserve(BoneTransforms.Num());
        Sample.scales.reserve(BoneTransforms.Num());
        for (const FTransform& Xform : BoneTransforms)
        {
            const FVector Loc = Xform.GetLocation();
            const FQuat Rot = Xform.GetRotation();
            const FVector Scale = Xform.GetScale3D();
            Sample.translations.emplace_back((double)Loc.X, (double)Loc.Y, (double)Loc.Z);
            Sample.rotations.emplace_back((double)Rot.X, (double)Rot.Y, (double)Rot.Z, (double)Rot.W);
            Sample.scales.emplace_back((double)Scale.X, (double)Scale.Y, (double)Scale.Z);
        }

        Sample.curves.reserve(CurveValues.Num());
        for (float Value : CurveValues)
        {
            Sample.curves.push_back(Value);
        }

        return Sample;
    }

    // Inverse of BuildPoseSampleFromLiveLink, for pushing a synthesized
    // (predicted/held/corrected) pose back through the same LiveLink path a
    // real frame takes. Scales default to 1 (matching BuildSubjectPose's
    // real-frame default when a channel is missing) if the sample has fewer
    // scale entries than translations/rotations - shouldn't happen in
    // practice since PoseSample channel counts are contractually stable
    // within an epoch, but avoids reading out of bounds if it ever does.
    void ApplyPoseSampleToLiveLink(const O3DS::PoseSample& Sample, TArray<FTransform>& OutBoneTransforms, TArray<float>& OutCurveValues)
    {
        const int32 Count = static_cast<int32>(Sample.translations.size());
        OutBoneTransforms.Reset(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const O3DS::Vector3d& Translation = Sample.translations[(size_t)Index];
            const O3DS::Vector3d Scale = (Index < (int32)Sample.scales.size()) ? Sample.scales[(size_t)Index] : O3DS::Vector3d(1.0, 1.0, 1.0);

            FQuat Rot = FQuat::Identity;
            if (Index < (int32)Sample.rotations.size())
            {
                const O3DS::Quat& Q = Sample.rotations[(size_t)Index];
                Rot = FQuat(Q.v[0], Q.v[1], Q.v[2], Q.v[3]);
                Rot.Normalize();
            }

            const FVector Location(Translation.v[0], Translation.v[1], Translation.v[2]);
            const FVector ScaleVec(Scale.v[0], Scale.v[1], Scale.v[2]);
            OutBoneTransforms.Emplace(Rot, Location, ScaleVec);
        }

        OutCurveValues.Reset(static_cast<int32>(Sample.curves.size()));
        for (float Value : Sample.curves)
        {
            OutCurveValues.Add(Value);
        }
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

    // A2.a: release any gap-buffered frames whose wait has timed out even when no
    // new frame arrives to trigger it via Push. Confined to the game thread, same
    // as HandleSerializedFrame - see ReceiverGate's declaration comment.
    const double NowS = FPlatformTime::Seconds();
    ReceiverGate.Flush(NowS, [this](O3DS::Frame&& F) { EmitGatedFrame(std::move(F)); });
    ReportGateMetricsDelta();

    // C1: synthesize a frame for any subject whose real data has starved
    // beyond LiveLink's own interpolation - see TickConcealment's declaration
    // comment.
    TickConcealment();

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
            SubjectTransformCaches.Remove(It.Key());  // PHASE 7 FIX: Clean up transform cache
            SubjectSkeletonHashes.Remove(It.Key());
            SubjectCurveHashes.Remove(It.Key());
            InitializedSubjects.Remove(It.Key());
            SubjectConcealment.Remove(It.Key());  // C1: drop the per-subject predictor/state too
            PrevConcealmentMetricsBySubject.Remove(It.Key());
            It.RemoveCurrent();
        }
    }
}

/** Entry point from the serialized consumer; peeks sequencing metadata, then either
 *  routes through the A1 reorder gate (senders that set tx_seq) or falls back to the
 *  pre-A1 legacy dedup/reorder path unchanged (senders that don't). */
void FO3DReceiverSource::HandleSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds, uint64 ArrivalEpochUsOverride)
{
    // Capture the true arrival instant exactly once, on whichever thread this frame
    // first arrived on - not after a possible transport-thread -> game-thread hop
    // below, which would otherwise fold scheduling delay into the gated path's
    // jitter/offset estimate (see ArrivalEpochUsOverride's doc comment on the header).
    const uint64 ArrivalEpochUs = (ArrivalEpochUsOverride != 0) ? ArrivalEpochUsOverride : O3DS::NowUtcMicros();

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
        AsyncTask(ENamedThreads::GameThread, [WeakSelf, Subject, TimestampSeconds, ArrivalEpochUs, BufferCopy = MoveTemp(BufferCopy)]() mutable
        {
            if (TSharedPtr<FO3DReceiverSource> Pinned = WeakSelf.Pin())
            {
                Pinned->HandleSerializedFrame(Subject, BufferCopy, TimestampSeconds, ArrivalEpochUs);
            }
        });
        return;
    }

    if (!bLoggedActiveState)
    {
        SourceStatus = FText::Format(LOCTEXT("StatusReceivingFmt", "Receiving via {0}"), FText::FromString(ActiveConfig.Transport));
        bLoggedActiveState = true;
    }

    // A2.a: peek tx_seq/tx_wallclock_us/frame_epoch without a full FlatBuffer parse
    // (O3DS::SubjectList::PeekMeta verifies the buffer first, so this is safe on
    // malformed input). This lets reorder/dedup/stale-drop happen before the
    // expensive parse+apply, so a superseded frame is never fully parsed.
    uint64 TxSeq = 0, TxWallclockUs = 0;
    uint32 FrameEpoch = 0;
    const bool bHasSeq = O3DS::SubjectList::PeekMeta(
        reinterpret_cast<const char*>(Buffer.GetData()), (size_t)Buffer.Num(),
        TxSeq, TxWallclockUs, FrameEpoch) && TxSeq != 0;

    if (!bHasSeq)
    {
        // Legacy sender (no tx_seq): behaves exactly as before A1/A2.
        HandleLegacyFrame(Subject, Buffer, TimestampSeconds);
        return;
    }

    O3DS::Frame Frame;
    Frame.seq = TxSeq;
    Frame.wallclock_us = TxWallclockUs;
    Frame.epoch = FrameEpoch;
    Frame.local_recv_us = ArrivalEpochUs; // true arrival instant - see Frame's doc comment
    Frame.bytes.assign(reinterpret_cast<const char*>(Buffer.GetData()),
        reinterpret_cast<const char*>(Buffer.GetData()) + Buffer.Num());

    LastGateSubjectLabel = Subject;

    const double NowS = FPlatformTime::Seconds();
    ReceiverGate.Push(std::move(Frame), NowS, [this](O3DS::Frame&& F) { EmitGatedFrame(std::move(F)); });
    ReportGateMetricsDelta();
}

/** Legacy (pre-A1) path for senders that don't set tx_seq: full parse, then
 *  SubjectList.time-based dedup/reorder suppression, then apply. */
void FO3DReceiverSource::HandleLegacyFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds)
{
    const double ParseStartWall = FPlatformTime::Seconds();

    const double ParseTimingStart = FPlatformTime::Seconds();
    if (!ParseSubjectListBuffer(Subject, Buffer))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }
    const double ParseTimeMs = (FPlatformTime::Seconds() - ParseTimingStart) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordParseTimeMs(ParseTimeMs);

    const double SubjectListTime = SubjectScratch.mTime;
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

    // Track per-operation timing for bottleneck identification
    const double PoseExtractionStartTime = FPlatformTime::Seconds();

    int32 PoseUpdateCount = 0;
    for (O3DS::Subject* SubjectPtr : SubjectScratch)
    {
        ProcessParsedSubject(SubjectPtr, SubjectListTime, -1.0, BoneNames, BoneParents, BoneTransforms, CurveNames, CurveValues);
        ++PoseUpdateCount;
    }

    const double PoseExtractionTimeMs = (FPlatformTime::Seconds() - PoseExtractionStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordPoseExtractionTimeMs(PoseExtractionTimeMs);

    // Record pose and skeleton updates
    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    // Update active subject count
    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(static_cast<int32>(SubjectScratch.mItems.size()));

    // Record total frame processing time
    const double TotalProcessingTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordTotalProcessingTimeMs(TotalProcessingTimeMs);

    if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
    {
        const double ParseEnd = FPlatformTime::Seconds();
        UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed subject list (subjects=%d bytes=%d dt=%.6fms)"),
            static_cast<int32>(SubjectScratch.mItems.size()), Buffer.Num(), (ParseEnd - ParseStartWall) * 1000.0);
    }
}

/** The A1 ReorderGate's emit callback: full parse + apply for a frame the gate has
 *  determined is in-order (called synchronously from Push, or later from Flush for
 *  a frame that was buffered waiting on a gap). Maps the sender's tx_wallclock onto
 *  local engine time (A2.b/A2.c) instead of the legacy path's "apply-time" stamp. */
void FO3DReceiverSource::EmitGatedFrame(O3DS::Frame&& Frame)
{
    if (!Client || !bIsValid)
    {
        return;
    }

    const double ParseStartWall = FPlatformTime::Seconds();

    if (!ParseSubjectListRaw(LastGateSubjectLabel, Frame.bytes.data(), Frame.bytes.size()))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }
    const double ParseTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordParseTimeMs(ParseTimeMs);

    const double SubjectListTime = SubjectScratch.mTime;

    FO3DPerformanceMetrics::Get().RecordFrameApplied();

    // A2.b/A2.c: map tx_wallclock_us onto local engine time. Observe() uses
    // Frame.local_recv_us (the true arrival instant, not "now") so gate-buffering
    // wait never gets misattributed as network jitter. The estimator's result is in
    // epoch microseconds; the (NowEpochUs, NowPlatformS) pair taken back-to-back
    // right here is purely an anchor to translate that into FPlatformTime::Seconds()
    // terms - the two clocks tick at the same rate, so this conversion holds
    // regardless of when it's taken, as long as both readings are simultaneous.
    // Falls back to "now" (this frame's true arrival time, via Observe()'s own
    // tx_wallclock_us==0 handling) when the sender didn't set tx_wallclock_us,
    // matching the roadmap's A2.c legacy-timestamp fallback.
    auto Sample = ClockEstimator.Observe(Frame.wallclock_us, Frame.local_recv_us);
    const uint64 NowEpochUs = O3DS::NowUtcMicros();
    const double NowPlatformS = FPlatformTime::Seconds();
    const double MappedWorldTimeSeconds = NowPlatformS +
        (double)((int64)Sample.mapped_presentation_time_us - (int64)NowEpochUs) / 1.0e6;

    if (Frame.wallclock_us != 0)
    {
        // C1: cache for TickConcealment()'s "mapped time right now" query.
        // Guarded the same as the metrics record below: offset_estimate_us
        // is hardwired to 0 in ClockOffsetEstimator::Observe()'s legacy
        // (tx_wallclock_us == 0) branch, so caching it unconditionally would
        // corrupt this with a bogus 0 offset whenever an untimestamped frame
        // arrives mixed into an otherwise-gated stream.
        LastClockOffsetEstimateUs = Sample.offset_estimate_us;
        bHasClockOffsetEstimate = true;

        FO3DPerformanceMetrics::Get().RecordClockOffsetSampleMs(
            (double)Sample.offset_estimate_us / 1000.0,
            (double)Sample.excess_delay_us / 1000.0);
    }

    TArray<FName> BoneNames;
    TArray<int32> BoneParents;
    TArray<FTransform> BoneTransforms;
    TArray<FName> CurveNames;
    TArray<float> CurveValues;

    const double PoseExtractionStartTime = FPlatformTime::Seconds();
    int32 PoseUpdateCount = 0;
    for (O3DS::Subject* SubjectPtr : SubjectScratch)
    {
        ProcessParsedSubject(SubjectPtr, SubjectListTime, MappedWorldTimeSeconds, BoneNames, BoneParents, BoneTransforms, CurveNames, CurveValues);
        ++PoseUpdateCount;
    }
    const double PoseExtractionTimeMs = (FPlatformTime::Seconds() - PoseExtractionStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordPoseExtractionTimeMs(PoseExtractionTimeMs);

    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(static_cast<int32>(SubjectScratch.mItems.size()));

    const double TotalProcessingTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordTotalProcessingTimeMs(TotalProcessingTimeMs);

    if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
    {
        UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed gated subject list (subjects=%d bytes=%d seq=%llu)"),
            static_cast<int32>(SubjectScratch.mItems.size()), (int32)Frame.bytes.size(), Frame.seq);
    }
}

/** Report the ReorderGate's stats as a delta against the last-reported snapshot (so
 *  multiple receiver sources correctly aggregate into the shared metrics singleton,
 *  the same convention as FramesReceived etc. - see FReceiverMetrics's doc comment). */
void FO3DReceiverSource::ReportGateMetricsDelta()
{
    const O3DS::ReorderStats& Stats = ReceiverGate.Stats();
    auto Delta = [](uint64 NewVal, uint64 OldVal) -> uint64 { return NewVal >= OldVal ? (NewVal - OldVal) : 0; };

    const uint64 DeltaDup = Delta(Stats.dup_dropped, PrevGateStats.dup_dropped);
    const uint64 DeltaStale = Delta(Stats.stale_dropped, PrevGateStats.stale_dropped);
    const uint64 DeltaLost = Delta(Stats.lost, PrevGateStats.lost);
    const uint64 DeltaReordered = Delta(Stats.reordered, PrevGateStats.reordered);

    FO3DPerformanceMetrics& Metrics = FO3DPerformanceMetrics::Get();
    if (DeltaDup) Metrics.RecordGateDupDropped(DeltaDup);
    if (DeltaStale) Metrics.RecordGateStaleDropped(DeltaStale);
    if (DeltaLost) Metrics.RecordGateLost(DeltaLost);
    if (DeltaReordered) Metrics.RecordGateReordered(DeltaReordered);
    if (DeltaDup || DeltaStale) Metrics.RecordReceiverFrameDropped(DeltaDup + DeltaStale);

    Metrics.SetGateBufferOccupancy(static_cast<int32>(ReceiverGate.PendingCount()));

    PrevGateStats = Stats;
}

/** Lazily creates a per-subject ConcealmentEngine on first use (roadmap doc §5/C1.a).
 *  LinearPredictor is the roadmap's recommended C1 default ("almost certainly" - see
 *  §5/C1's "Open decisions"); Quadratic may overshoot on longer horizons. */
O3DS::ConcealmentEngine& FO3DReceiverSource::GetOrCreateSubjectConcealment(FName SubjectName)
{
    if (TUniquePtr<O3DS::ConcealmentEngine>* Existing = SubjectConcealment.Find(SubjectName))
    {
        return **Existing;
    }

    O3DS::ConcealmentConfig Config;
    Config.starvationThresholdSeconds = FMath::Max(0.0, (double)CVarO3DReceiverConcealmentStarvationMs.GetValueOnGameThread() / 1000.0);
    Config.maxConcealHorizonSeconds = FMath::Max(0.0, (double)CVarO3DReceiverConcealmentHorizonMs.GetValueOnGameThread() / 1000.0);
    Config.correctionWindowSeconds = FMath::Max(0.0, (double)CVarO3DReceiverConcealmentCorrectionMs.GetValueOnGameThread() / 1000.0);

    TUniquePtr<O3DS::ConcealmentEngine> NewEngine = MakeUnique<O3DS::ConcealmentEngine>(MakeUnique<O3DS::LinearPredictor>(), Config);
    O3DS::ConcealmentEngine& Ref = *NewEngine;
    SubjectConcealment.Add(SubjectName, MoveTemp(NewEngine));
    return Ref;
}

/** Feed one confirmed real (gated) frame into this subject's concealment engine. Only
 *  called for the A2-gated path - see this method's declaration comment on why the
 *  legacy/ungated path is left alone. Resets the engine on a topology/curve-set change
 *  (bTopologyChanged) so stale per-node history from a different skeleton never mixes
 *  into a prediction, per C1.a's "Reset the predictor on... topology change". */
void FO3DReceiverSource::ObserveConcealmentRealFrame(FName SubjectName, double PresentationTimeSeconds, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, bool bTopologyChanged)
{
    if (CVarO3DReceiverConcealmentEnabled.GetValueOnGameThread() == 0)
    {
        return;
    }

    O3DS::ConcealmentEngine& Engine = GetOrCreateSubjectConcealment(SubjectName);
    if (bTopologyChanged)
    {
        Engine.Reset();
    }

    Engine.ObserveRealFrame(BuildPoseSampleFromLiveLink(PresentationTimeSeconds, BoneTransforms, CurveValues));
}

/** Per-tick concealment poll (roadmap doc §5/C1.a): for every subject with an active
 *  engine, ask whether "now" needs a synthesized frame (gap beyond LiveLink's own
 *  interpolation) and push one if so. "Now" is the mapped presentation time computed
 *  the same way EmitGatedFrame maps a real frame's tx_wallclock_us, but anchored to the
 *  last known clock-offset estimate rather than a fresh Observe() call, since there's
 *  no new frame to observe here - see LastClockOffsetEstimateUs's declaration comment. */
void FO3DReceiverSource::TickConcealment()
{
    if (!Client || !bHasClockOffsetEstimate || SubjectConcealment.Num() == 0)
    {
        return;
    }

    if (CVarO3DReceiverConcealmentEnabled.GetValueOnGameThread() == 0)
    {
        return;
    }

    const double TNow = FPlatformTime::Seconds() + (double)LastClockOffsetEstimateUs / 1.0e6;

    for (TPair<FName, TUniquePtr<O3DS::ConcealmentEngine>>& Pair : SubjectConcealment)
    {
        if (!Pair.Value.IsValid())
        {
            continue;
        }

        O3DS::PoseSample Predicted;
        if (!Pair.Value->TryConceal(TNow, Predicted))
        {
            continue;
        }

        TArray<FTransform> BoneTransforms;
        TArray<float> CurveValues;
        ApplyPoseSampleToLiveLink(Predicted, BoneTransforms, CurveValues);
        if (BoneTransforms.Num() == 0)
        {
            continue;
        }

        const FLiveLinkSubjectKey SubjectKey(SourceGuid, FLiveLinkSubjectName(Pair.Key));
        const uint64* CurveHashPtr = SubjectCurveHashes.Find(Pair.Key);
        // No static-data re-push: a synthesized frame never changes topology
        // (bTopologyChanged already reset the engine above when that
        // happens), so the already-registered skeleton/curve names apply.
        PushSubjectFrameData(SubjectKey, BoneTransforms, TArray<FName>(), CurveValues, Predicted.t, Predicted.t, CurveHashPtr ? *CurveHashPtr : 0);
    }

    ReportConcealmentMetricsDelta();
}

/** Report each subject's ConcealmentEngine stats as a delta against its last-reported
 *  snapshot (same convention as ReportGateMetricsDelta above), summed across subjects
 *  into the shared metrics singleton. The error/pop averages are a last-writer-wins
 *  gauge across subjects/sources, same simplification as AvgClockOffsetMs. */
void FO3DReceiverSource::ReportConcealmentMetricsDelta()
{
    auto Delta = [](uint64 NewVal, uint64 OldVal) -> uint64 { return NewVal >= OldVal ? (NewVal - OldVal) : 0; };

    uint64 DeltaConcealed = 0, DeltaFallback = 0, DeltaCorrection = 0, DeltaRecovery = 0;
    bool bHasAnyRecovery = false;
    double LastTransErr = 0.0, LastRotErrDeg = 0.0, LastPopTrans = 0.0, LastPopRotDeg = 0.0;

    for (TPair<FName, TUniquePtr<O3DS::ConcealmentEngine>>& Pair : SubjectConcealment)
    {
        if (!Pair.Value.IsValid())
        {
            continue;
        }

        const O3DS::ConcealmentMetrics& Current = Pair.Value->Metrics();
        O3DS::ConcealmentMetrics& Prev = PrevConcealmentMetricsBySubject.FindOrAdd(Pair.Key);

        DeltaConcealed += Delta(Current.concealedFrameCount, Prev.concealedFrameCount);
        DeltaFallback += Delta(Current.fallbackHoldCount, Prev.fallbackHoldCount);
        DeltaCorrection += Delta(Current.correctionFrameCount, Prev.correctionFrameCount);
        DeltaRecovery += Delta(Current.recoveryCount, Prev.recoveryCount);

        if (Current.recoveryCount > 0)
        {
            bHasAnyRecovery = true;
            LastTransErr = Current.MeanPredictionTranslationError();
            LastRotErrDeg = FMath::RadiansToDegrees(Current.MeanPredictionRotationErrorRadians());
            LastPopTrans = Current.MeanPopTranslation();
            LastPopRotDeg = FMath::RadiansToDegrees(Current.MeanPopRotationRadians());
        }

        Prev = Current;
    }

    FO3DPerformanceMetrics& Metrics = FO3DPerformanceMetrics::Get();
    if (DeltaConcealed) Metrics.RecordConcealedFrames(DeltaConcealed);
    if (DeltaFallback) Metrics.RecordConcealmentFallbackHolds(DeltaFallback);
    if (DeltaCorrection) Metrics.RecordConcealmentCorrectionFrames(DeltaCorrection);
    if (DeltaRecovery) Metrics.RecordConcealmentRecoveries(DeltaRecovery);
    if (bHasAnyRecovery)
    {
        Metrics.SetConcealmentPredictionError(LastTransErr, LastRotErrDeg);
        Metrics.SetConcealmentPop(LastPopTrans, LastPopRotDeg);
    }
}

/** Parse the FlatBuffer payload into a reusable SubjectList scratch structure. */
bool FO3DReceiverSource::ParseSubjectListBuffer(const FString& Subject, const TArray<uint8>& Buffer)
{
    return ParseSubjectListRaw(Subject, reinterpret_cast<const char*>(Buffer.GetData()), (size_t)Buffer.Num());
}

/** Parse the FlatBuffer payload (raw pointer form, used by the gate's emit path
 *  since its payload is a std::vector<char> rather than a TArray<uint8>). */
bool FO3DReceiverSource::ParseSubjectListRaw(const FString& Subject, const char* Data, size_t Len)
{
    if (!SubjectScratch.Parse(Data, Len, nullptr, true))
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Parse failed for subject '%s' (%d bytes)"), *Subject, (int32)Len);
        return false;
    }
    return true;
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
void FO3DReceiverSource::ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues)
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

    // PHASE 4: Cache check - can we skip rebuilding the skeleton structure?
    if (ExistingCache && ExistingCache->SkeletonFingerprint == QuickSkeletonFingerprint)
    {
        // Skeleton structure didn't change, reuse cached bone names/parents
        // BUT we still need to extract the transform VALUES from this frame!
        BoneNames = ExistingCache->BoneNames;
        BoneParents = ExistingCache->BoneParents;

        // Extract only the transform values for this frame
        BoneTransforms.Reset();
        BoneTransforms.Reserve(static_cast<int32>(SubjectPtr->mTransforms.mItems.size()));
        for (O3DS::Transform* TransformPtr : SubjectPtr->mTransforms.mItems)
        {
            if (!TransformPtr)
                continue;

            const O3DS::Vector3d Translation = TransformPtr->translation.value;
            const O3DS::Vector4d Rotation = TransformPtr->rotation.value;
            const O3DS::Vector3d Scale = TransformPtr->scale.value;

            FQuat Quat(static_cast<float>(Rotation.v[0]), static_cast<float>(Rotation.v[1]),
                       static_cast<float>(Rotation.v[2]), static_cast<float>(Rotation.v[3]));
            FVector Location(static_cast<float>(Translation.v[0]), static_cast<float>(Translation.v[1]),
                            static_cast<float>(Translation.v[2]));
            FVector ScaleVec(static_cast<float>(Scale.v[0]), static_cast<float>(Scale.v[1]),
                            static_cast<float>(Scale.v[2]));

            // Validation checks
            if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z) ||
                !FMath::IsFinite(ScaleVec.X) || !FMath::IsFinite(ScaleVec.Y) || !FMath::IsFinite(ScaleVec.Z) ||
                !FMath::IsFinite(Quat.X) || !FMath::IsFinite(Quat.Y) || !FMath::IsFinite(Quat.Z) || !FMath::IsFinite(Quat.W))
            {
                return;
            }

            if (Quat.SizeSquared() <= KINDA_SMALL_NUMBER)
                return;

            Quat.Normalize();
            if (Quat.ContainsNaN())
                return;

            BoneTransforms.Add(FTransform(Quat, Location, ScaleVec));
        }

        if (BoneTransforms.Num() == 0)
            return;

        BuildSubjectCurves(SubjectPtr, CurveNames, CurveValues);
    }
    else
    {
        // Skeleton structure changed or first time - rebuild everything
        if (!BuildSubjectPose(SubjectPtr, BoneNames, BoneParents, BoneTransforms))
        {
            return;
        }

        BuildSubjectCurves(SubjectPtr, CurveNames, CurveValues);

        // Cache the new skeleton structure
        FSubjectTransformCache& Cache = SubjectTransformCaches.FindOrAdd(SubjectFName);
        Cache.BoneNames = BoneNames;
        Cache.BoneParents = BoneParents;
        Cache.SkeletonFingerprint = QuickSkeletonFingerprint;
    }

    const FLiveLinkSubjectName SubjectName(SubjectFName);
    const FLiveLinkSubjectKey SubjectKey(SourceGuid, SubjectName);

    // Compute hashes for LiveLink update check
    const uint64 SkeletonHash = HashArray(BoneNames, &BoneParents);
    const uint64 CurveHash = HashCurveNames(CurveNames);

    const uint64* ExistingSkeletonHash = SubjectSkeletonHashes.Find(SubjectFName);
    const uint64* ExistingCurveHash = SubjectCurveHashes.Find(SubjectFName);
    const bool bNeedStaticUpdate = (!ExistingSkeletonHash || *ExistingSkeletonHash != SkeletonHash) || (!ExistingCurveHash || *ExistingCurveHash != CurveHash);

    double LiveLinkPushTimeMs = 0.0;
    const double LiveLinkStartTime = FPlatformTime::Seconds();

    // Measure static data push separately to identify which operation blocks
    if (!InitializedSubjects.Contains(SubjectFName) || bNeedStaticUpdate)
    {
        const double StaticStartTime = FPlatformTime::Seconds();
        PushSubjectStaticData(SubjectKey, BoneNames, BoneParents, CurveNames, SkeletonHash);
        const double StaticTimeMs = (FPlatformTime::Seconds() - StaticStartTime) * 1000.0;

        // Log if static push is slow (potential blocking point)
        if (StaticTimeMs > 5.0)
        {
            UE_LOG(LogO3DReceiverSource, Warning,
                TEXT("PushSubjectStaticData took %.2f ms (subject='%s', may indicate LiveLink client blocking)"),
                StaticTimeMs, *SubjectFName.ToString());
        }

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

    // C1: feed the real frame into this subject's concealment engine before
    // pushing it - only for the gated (A2) path, where WorldTimeSecondsOverride
    // is a real sender-clock-mapped time (>= 0.0); the legacy path has no
    // reliable clock domain to reason about gaps in (see the roadmap's C1.a
    // scope note and ObserveConcealmentRealFrame's declaration comment).
    if (WorldTimeSecondsOverride >= 0.0)
    {
        ObserveConcealmentRealFrame(SubjectFName, WorldTimeSecondsOverride, BoneTransforms, CurveValues, bNeedStaticUpdate);
    }

    const double FrameStartTime = FPlatformTime::Seconds();
    PushSubjectFrameData(SubjectKey, BoneTransforms, CurveNames, CurveValues, SubjectListTime, WorldTimeSecondsOverride, CurveHash);
    const double FrameTimeMs = (FPlatformTime::Seconds() - FrameStartTime) * 1000.0;

    // Log if frame push is slow (primary blocking suspect)
    if (FrameTimeMs > 5.0)
    {
        UE_LOG(LogO3DReceiverSource, Warning,
            TEXT("PushSubjectFrameData took %.2f ms (subject='%s', PRIMARY SUSPECT for latency)"),
            FrameTimeMs, *SubjectFName.ToString());
    }

    LiveLinkPushTimeMs = (FPlatformTime::Seconds() - LiveLinkStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordLiveLinkPushTimeMs(LiveLinkPushTimeMs);

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

    // Create a settings object to allow subject-level configuration of preprocessors, interpolation, and translators
    ULiveLinkSubjectSettings* SubjectSettings = NewObject<ULiveLinkSubjectSettings>();
    if (SubjectSettings)
    {
        SubjectSettings->Initialize(SubjectKey);
        // CRITICAL: Set the role on the settings object so ValidateProcessors() won't clear preprocessors/interpolation/translators
        SubjectSettings->Role = ULiveLinkAnimationRole::StaticClass();
    }

    FLiveLinkSubjectPreset Preset;
    Preset.Key = SubjectKey;
    Preset.Role = ULiveLinkAnimationRole::StaticClass();
    Preset.Settings = SubjectSettings;
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

void FO3DReceiverSource::PushSubjectFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues, double TimestampSeconds, double WorldTimeSecondsOverride, uint64 CurveHash)
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
    // A2.c: use the sender-clock-mapped presentation time when available (gated
    // path), falling back to today's apply-time stamp for legacy/ungated frames.
    BaseFrameData.WorldTime = (WorldTimeSecondsOverride >= 0.0) ? WorldTimeSecondsOverride : FPlatformTime::Seconds();
    BaseFrameData.MetaData.SceneTime = FQualifiedFrameTime();
    BaseFrameData.MetaData.StringMetaData.Add(TEXT("CurveHash"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(CurveHash)));
    BaseFrameData.MetaData.StringMetaData.Add(TEXT("SubjectListTime"), FString::Printf(TEXT("%.6f"), TimestampSeconds));

    FrameData.FrameId = FrameCounter++;

    Client->PushSubjectFrameData_AnyThread(SubjectKey, MoveTemp(FrameDataStruct));
}

void FO3DReceiverSource::ResetOrderingState()
{
    LastAppliedSubjectListTime = -1.0;
    // A2.a: a transport restart starts a clean gate/estimator session too, so
    // stale sequence/epoch/offset state from a previous connection never bleeds
    // into a new one. Note this is also reachable from the legacy path's own
    // silence/timestamp-jump reset (ShouldResetOrderingWindow), so a stream that
    // mixes tx_seq and non-tx_seq frames would have a legacy-triggered reset also
    // wipe the gate's buffered frames/epoch tracking - a real caveat only for that
    // mixed-stream case, not for a source that's purely gated or purely legacy.
    ReceiverGate = O3DS::ReorderGate();
    ClockEstimator = O3DS::ClockOffsetEstimator();
    PrevGateStats = O3DS::ReorderStats();

    // C1: a publisher restart invalidates every subject's prediction history
    // equally (ties to C1.a's "Reset the predictor on... publisher restart"),
    // and the cached clock-offset estimate above is now stale too.
    SubjectConcealment.Empty();
    PrevConcealmentMetricsBySubject.Empty();
    bHasClockOffsetEstimate = false;
    LastClockOffsetEstimateUs = 0;
}

#undef LOCTEXT_NAMESPACE
