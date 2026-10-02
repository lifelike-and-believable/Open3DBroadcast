// Copyright (c) Open3DStream Contributors

#include "O3DReceiverSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "LiveLinkPreset.h"
#include "LiveLinkSubjectSettings.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"
#include "Misc/QualifiedFrameTime.h"

#include "O3DHelpers.h"
#include "O3DRedact.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DReceiverLegacyTransportShims.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"
#include "O3DAudioBus.h"
#include "O3DAudioFrameCodec.h"
#include "O3DPerformanceMetrics.h"
#include "O3DControlBus.h"
#include "O3DControlConvert.h"
#include "O3DControlSettings.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds_generated.h"
#include "o3ds/sequencing.h"
#include "o3ds/predict/linear_predictor.h"
THIRD_PARTY_INCLUDES_END

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

// C1: receiver-side concealment (roadmap doc §5/C1) config used to live here
// as cvars. These are production tuning knobs a project would set per
// deployment (per the roadmap's "Open decisions" note that they need
// real-world/live-LiveLink tuning), not debug/iteration toggles, so they're
// now real UPROPERTY fields on UO3DReceiverSourceSettings - see
// GetConcealmentSettings() below - editable from LiveLink's own per-source
// "Settings" panel instead of requiring a console command. Defaults still
// match ConcealmentConfig's own defaults (see concealment.h).

/** Adapts the shared consumer registry callback into this live source instance. */
class FO3DReceiverSource::FSerializedConsumer : public ISerializedFrameConsumer
{
public:
    explicit FSerializedConsumer(TWeakPtr<FO3DReceiverSource> InOwner)
        : Owner(MoveTemp(InOwner))
    {
    }

    virtual ~FSerializedConsumer() override = default;

    /** View form (WP-A1 PR 5b): Buffer is valid only for this call; a hop copies it once. */
    virtual void SubmitFrame(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds) override
    {
        if (!IsInGameThread())
        {
            HopToGameThread(Subject, TArray<uint8>(Buffer.GetData(), Buffer.Num()), TimestampSeconds);
            return;
        }

        if (TSharedPtr<FO3DReceiverSource> OwnerPinned = Owner.Pin())
        {
            OwnerPinned->HandleSerializedFrame(Subject, Buffer, TimestampSeconds);
        }
    }

    /** Owned form (WP-A1 PR 5b): a hop moves the buffer instead of copying it. */
    virtual void SubmitFrameOwned(const FString& Subject, TArray<uint8>&& Buffer, double TimestampSeconds) override
    {
        if (!IsInGameThread())
        {
            HopToGameThread(Subject, MoveTemp(Buffer), TimestampSeconds);
            return;
        }

        if (TSharedPtr<FO3DReceiverSource> OwnerPinned = Owner.Pin())
        {
            OwnerPinned->HandleSerializedFrame(Subject, Buffer, TimestampSeconds);
        }
    }

private:
    // WP-S5 (RCV-1): never pin the source on a transport or FFI thread, where it could become the
    // last owner and run ~FO3DReceiverSource (and the transport's Stop()) off the game thread.
    // Hop with the weak reference and pin on the game thread.
    void HopToGameThread(const FString& Subject, TArray<uint8>&& Buffer, double TimestampSeconds) const
    {
        const uint64 ArrivalEpochUs = O3DS::NowUtcMicros();
        TWeakPtr<FO3DReceiverSource> WeakOwner = Owner;
        AsyncTask(ENamedThreads::GameThread, [WeakOwner, Subject, TimestampSeconds, ArrivalEpochUs, Bytes = MoveTemp(Buffer)]()
        {
            if (TSharedPtr<FO3DReceiverSource> OwnerPinned = WeakOwner.Pin())
            {
                OwnerPinned->HandleSerializedFrame(Subject, Bytes, TimestampSeconds, ArrivalEpochUs);
            }
        });
    }

    TWeakPtr<FO3DReceiverSource> Owner;
};

TSharedRef<ISerializedFrameConsumer, ESPMode::ThreadSafe> FO3DReceiverSource::MakeSerializedConsumer(TWeakPtr<FO3DReceiverSource> Owner)
{
    return MakeShared<FSerializedConsumer>(MoveTemp(Owner));
}

/**
 * Control sink handed to the transport (ADR 0011). May be called on any thread: a socket or NNG
 * Poll on the game thread today, an FFI thread for MoQ and WebRTC later. Off the game thread it
 * copies the payload (the view lives only for the call) and hops with a weak reference, pinning
 * the source only on the game thread (WP-S5, RCV-1), as FSerializedConsumer does.
 */
class FO3DReceiverSource::FControlSink final : public IO3DReceiverControlSink
{
public:
    explicit FControlSink(TWeakPtr<FO3DReceiverSource> InOwner)
        : Owner(MoveTemp(InOwner))
    {
    }

    virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double ReceiveTimeSec) override
    {
        TArray<uint8> Copy(Payload.GetData(), Payload.Num());
        if (!IsInGameThread())
        {
            TWeakPtr<FO3DReceiverSource> WeakOwner = Owner;
            AsyncTask(ENamedThreads::GameThread, [WeakOwner, StreamId, ReceiveTimeSec, Copy = MoveTemp(Copy)]()
            {
                if (TSharedPtr<FO3DReceiverSource> Pinned = WeakOwner.Pin())
                {
                    Pinned->HandleControlPayload(Copy, StreamId, ReceiveTimeSec);
                }
            });
            return;
        }
        if (TSharedPtr<FO3DReceiverSource> Pinned = Owner.Pin())
        {
            Pinned->HandleControlPayload(Copy, StreamId, ReceiveTimeSec);
        }
    }

private:
    TWeakPtr<FO3DReceiverSource> Owner;
};

/**
 * Lightweight audio bridge that republishes transport frames onto the gameplay audio bus.
 *
 * WP-S5 (RCV-1): may be called on any transport or FFI thread. It holds an immutable metadata
 * snapshot and no reference to the source, and it hands data to the game thread by value
 * (the audio bus is game-thread-only, SHR-10). Its destructor frees memory only, so any
 * thread may drop the last reference.
 */
class FO3DReceiverSource::FAudioSink : public IO3DReceiverAudioSink
{
public:
    explicit FAudioSink(FAudioMetaDefaults InDefaults)
        : Defaults(MoveTemp(InDefaults))
    {
    }

    virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& InMeta, const uint8* Data, int32 NumBytes) override
    {
        if (!Data || NumBytes <= 0)
        {
            return;
        }

        {
            O3DS::FAudioFrameMeta MetaCopy = InMeta;
            Defaults.Apply(MetaCopy);

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
    const FAudioMetaDefaults Defaults;
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
    // as HandleSerializedFrame - see the Streams declaration comment.
    // Each sender stream has its own gate (RCV-5). EmitGatedFrame looks its stream
    // up by key and never adds or removes streams, as ForEach requires.
    const double NowS = FPlatformTime::Seconds();
    Streams.ForEach([this, NowS](uint64 StreamKey, O3DS::ReceiverStream& Stream)
    {
        Stream.gate.Flush(NowS, [this, StreamKey](O3DS::Frame&& F) { EmitGatedFrame(StreamKey, std::move(F)); });
    });
    ReportGateMetricsDelta();

    // C1: synthesize a frame for any subject whose real data has starved
    // beyond LiveLink's own interpolation - see TickConcealment's declaration
    // comment.
    TickConcealment();

    TickControl(NowS);

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
    if (ActiveConfig.Transport.IsNone())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("No transport selected for receiver source."));
        return false;
    }

    // The schema's Validate functions refuse the options before anything is created (WP-A1 PR 5c).
    LastTransportResult = O3DTransportOptions::ValidateOptions(ActiveConfig.GetOptions());
    if (!LastTransportResult.IsOk())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Receiver transport '%s' not started: %s"), *ActiveConfig.Transport.ToString(), *LexToString(LastTransportResult));
        SourceStatus = FText::Format(LOCTEXT("StatusInvalidOptionsFmt", "Invalid options: {0}"), FText::FromString(LastTransportResult.Message));
        return false;
    }

    const FName TransportName(ActiveConfig.Transport);
    ActiveReceiver = FO3DTransportRegistry::Get().CreateReceiver(TransportName);
    if (!ActiveReceiver.IsValid())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("No receiver registered for transport '%s'."), *ActiveConfig.Transport.ToString());
        return false;
    }

    // The receiver is registry-tracked: release it when its transport unregisters (ADR 0007 item 5).
    // StopTransport removes the subscription, on every path that drops ActiveReceiver.
    TransportUnregisteringHandle = FO3DTransportRegistry::Get().OnTransportUnregistering().AddRaw(this, &FO3DReceiverSource::HandleTransportUnregistering);

    const FO3DTransportResult InitResult = ActiveReceiver->Initialize(ActiveConfig);
    if (!InitResult.IsOk())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Failed to initialize transport '%s': %s"), *ActiveConfig.Transport.ToString(), *LexToString(InitResult));
        ActiveReceiver.Reset();
        StopTransport();
        return false;
    }

    ActiveAudioSink.Reset();
    if (ActiveConfig.Audio.bEnableAudio)
    {
        if (ActiveReceiver->SupportsAudio())
        {
            ActiveAudioSink = MakeAudioSink();
            ActiveReceiver->SetAudioSink(ActiveAudioSink, ActiveConfig.Audio);
            UE_LOG(LogO3DReceiverAudio, Log, TEXT("Audio sink bound for transport '%s'."),
                *ActiveConfig.Transport.ToString());
        }
        else
        {
            UE_LOG(LogO3DReceiverAudio, Warning, TEXT("Transport '%s' does not support audio; disabling audio for this source."), *ActiveConfig.Transport.ToString());
        }
    }

    ActiveConsumer = MakeSerializedConsumer(TWeakPtr<FO3DReceiverSource>(AsShared()));
    ActiveReceiver->SetConsumer(ActiveConsumer);

    // Control (ADR 0011): the sink is always installed when the transport carries control;
    // whether a payload is accepted is decided per message (IsControlEnabled), so turning
    // control on at runtime needs no transport restart.
    ActiveControlSink.Reset();
    if (ActiveReceiver->SupportsControl())
    {
        ActiveControlSink = MakeShared<FControlSink, ESPMode::ThreadSafe>(TWeakPtr<FO3DReceiverSource>(AsShared()));
        ActiveReceiver->SetControlSink(ActiveControlSink);
        ApplyControlConfig();
    }
    const FO3DTransportResult StartResult = ActiveReceiver->Start();
    if (!StartResult.IsOk())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Failed to start transport '%s': %s"), *ActiveConfig.Transport.ToString(), *LexToString(StartResult));
        ActiveReceiver->Stop();
        ActiveReceiver->SetConsumer(nullptr);
        if (ActiveAudioSink.IsValid() && ActiveReceiver->SupportsAudio())
        {
            ActiveReceiver->SetAudioSink(nullptr, ActiveConfig.Audio);
        }
        ActiveReceiver.Reset();
        ActiveConsumer.Reset();
        ActiveAudioSink.Reset();
        ActiveControlSink.Reset();
        StopTransport();
        return false;
    }

    if (!ActiveConfig.Uri.IsEmpty())
    {
        SourceMachineName = FText::FromString(O3DRedact::Url(ActiveConfig.Uri));
    }
    else if (!ActiveConfig.StreamId.IsEmpty())
    {
        SourceMachineName = FText::FromString(O3DRedact::Url(ActiveConfig.StreamId));
    }

    UE_LOG(LogO3DReceiverSource, Log, TEXT("Receiver transport '%s' started (Uri=%s, StreamId=%s)."),
        *ActiveConfig.Transport.ToString(),
        *O3DRedact::Url(ActiveConfig.Uri),
        *O3DRedact::Url(ActiveConfig.StreamId));

    SourceStatus = FText::Format(LOCTEXT("StatusReceivingFmt", "Receiving via {0}"), FText::FromName(ActiveConfig.Transport));
    ResetStreamState();
    return true;
}

/** Stop the active transport and clear subject/audio caches. */
void FO3DReceiverSource::StopTransport()
{
    if (TransportUnregisteringHandle.IsValid())
    {
        // Safe during the registry's broadcast (UE defers the invocation-list compaction).
        FO3DTransportRegistry::Get().OnTransportUnregistering().Remove(TransportUnregisteringHandle);
        TransportUnregisteringHandle.Reset();
    }

    if (ActiveReceiver.IsValid())
    {
        if (ActiveConfig.Audio.bEnableAudio && ActiveReceiver->SupportsAudio())
        {
            ActiveReceiver->SetAudioSink(nullptr, ActiveConfig.Audio);
        }
        ActiveReceiver->SetConsumer(nullptr);
        ActiveReceiver->Stop();
        // A receiver releases its control sink in Stop (ADR 0011); clearing it as well keeps the
        // sink from outliving the session with a receiver that does not.
        if (ActiveControlSink.IsValid())
        {
            ActiveReceiver->SetControlSink(nullptr);
        }
        ActiveReceiver.Reset();
    }
    ActiveAudioSink.Reset();
    ActiveConsumer.Reset();
    ActiveControlSink.Reset();

    // Deliver what alignment was still holding: the changes are real, only their timing is lost.
    std::vector<O3DS::Control::Change> Held;
    ControlAligner.Flush(Held);
    for (const O3DS::Control::Change& Change : Held)
    {
        PublishControlChange(Change);
    }
    InitializedSubjects.Empty();
    SubjectSkeletonHashes.Empty();
    SubjectCurveHashes.Empty();
    SubjectLastUpdateTime.Empty();
    // RCV-5/RCV-34: cached bone names from the previous session must not survive a
    // restart, like the other per-subject maps above.
    SubjectTransformCaches.Empty();
    bLoggedActiveState = false;
    FrameCounter = 0;
    ResetStreamState();
}

void FO3DReceiverSource::HandleTransportUnregistering(FName TransportName)
{
    if (!ActiveReceiver.IsValid() || TransportName != ActiveConfig.Transport)
    {
        return;
    }

    UE_LOG(LogO3DReceiverSource, Warning, TEXT("Receiver transport '%s' is being unregistered (its module is shutting down); stopping and releasing the receiver."), *ActiveConfig.Transport.ToString());
    StopTransport();
    SourceStatus = FText::Format(LOCTEXT("StatusTransportUnregisteredFmt", "Transport {0} unloaded"), FText::FromName(ActiveConfig.Transport));
}

/** Ensure we always have a transport name for details panels that expose the source settings. */
void FO3DReceiverSource::EnsureValidTransportName()
{
    if (!SourceSettings.TransportName.IsNone())
    {
        return;
    }

    const TArray<FName> RegisteredTransports = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver);
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
    FName TransportName = SourceSettings.TransportName;
    if (TransportName.IsNone())
    {
        TransportName = DefaultReceiverTransportName;
    }

    // The registered name and the side (TRB-27, WP-A1 PR 5c).
    FO3DTransportConfig Config(TransportName, EO3DTransportRole::Receiver);

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

    // Declared secret keys are never copied into AdvancedParams; they are resolved from the
    // secret store into Config.Secrets (ADR 0004 item 4).
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars);
    TMap<FString, FString> Options;
    for (const TPair<FString, FString>& Option : SourceSettings.TransportOptions)
    {
        if (!SecretKeys.Contains(Option.Key))
        {
            Options.Add(Option.Key, Option.Value);
        }
    }
    Config.AdvancedParams = Options;
    O3DReceiver::ResolveSecrets(SourceSettings, Config.Secrets);

    // The descriptor is a shared, immutable snapshot, so the function stays valid while it runs
    // even if the transport unregisters meanwhile (RCV-27).
    const FO3DTransportDescriptorPtr Descriptor = Config.Transport.IsNone() ? FO3DTransportDescriptorPtr() : FO3DTransportRegistry::Get().Find(TransportName);
    if (Descriptor.IsValid())
    {
        Config.OptionSchema = MakeShared<FO3DTransportOptionSchema>(Descriptor->ReceiverOptions.OptionSchema);
        if (Descriptor->ConfigureReceiver)
        {
            // The view is over this function's own copy of the options (WP-A1 PR 5a); the scope
            // hands a configure function registered through the deprecated customization these
            // settings.
            const O3DReceiverLegacyShims::FScopedConfiguringSettings LegacyScope(SourceSettings);
            Descriptor->ConfigureReceiver(FO3DTransportOptionsView(Options, Config.OptionSchema.Get()), Config);
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

    // Senders that went quiet: drop their parse and ordering state too, so a
    // restarted sender starts clean and the table does not keep dead streams.
    Streams.PruneIdle(Now, InactivityThresholdSeconds);
}

/** Entry point from the serialized consumer; peeks sequencing metadata, then either
 *  routes through the A1 reorder gate (senders that set tx_seq) or falls back to the
 *  pre-A1 legacy dedup/reorder path unchanged (senders that don't). */
void FO3DReceiverSource::HandleSerializedFrame(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, uint64 ArrivalEpochUsOverride)
{
    // Capture the true arrival instant exactly once, on whichever thread this frame
    // first arrived on - not after a possible transport-thread -> game-thread hop
    // below, which would otherwise fold scheduling delay into the gated path's
    // jitter/offset estimate (see ArrivalEpochUsOverride's doc comment on the header).
    const uint64 ArrivalEpochUs = (ArrivalEpochUsOverride != 0) ? ArrivalEpochUsOverride : O3DS::NowUtcMicros();

    if (!CanPublish() || !bIsValid)
    {
        return;
    }

    // Record frame received
    FO3DPerformanceMetrics::Get().RecordFrameReceived();
    FO3DPerformanceMetrics::Get().RecordBytesDeserialized(Buffer.Num());

    if (!IsInGameThread())
    {
        TWeakPtr<FO3DReceiverSource> WeakSelf = AsShared();
        // Buffer is only valid for this call, so the hop takes a copy.
        TArray<uint8> BufferCopy(Buffer.GetData(), Buffer.Num());
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
        SourceStatus = FText::Format(LOCTEXT("StatusReceivingFmt", "Receiving via {0}"), FText::FromName(ActiveConfig.Transport));
        bLoggedActiveState = true;
    }

    // A2.a: peek tx_seq/tx_wallclock_us/frame_epoch, the content time and the sender
    // stream key without a full FlatBuffer parse (PeekPacketMeta verifies the buffer
    // first, so this is safe on malformed input). Reorder, dedup and stale-drop then
    // happen before the parse, so a superseded frame never changes parse state.
    O3DS::PacketMeta Meta;
    if (!O3DS::PeekPacketMeta(reinterpret_cast<const char*>(Buffer.GetData()), (size_t)Buffer.Num(), Meta))
    {
        // Throttled: a peer sending bytes this build cannot read (for example control messages
        // to a receiver that predates them, ADR 0011 item 10) must not flood the log.
        const double NowSeconds = FPlatformTime::Seconds();
        if (NowSeconds - LastMalformedWarningTime >= MalformedWarningIntervalSeconds)
        {
            if (SuppressedMalformedWarnings > 0)
            {
                UE_LOG(LogO3DReceiverSource, Warning, TEXT("Rejected malformed packet for subject '%s' (%d bytes); %d similar rejected since the last warning"),
                    *Subject, Buffer.Num(), SuppressedMalformedWarnings);
            }
            else
            {
                UE_LOG(LogO3DReceiverSource, Warning, TEXT("Rejected malformed packet for subject '%s' (%d bytes)"), *Subject, Buffer.Num());
            }
            LastMalformedWarningTime = NowSeconds;
            SuppressedMalformedWarnings = 0;
        }
        else
        {
            ++SuppressedMalformedWarnings;
        }
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }

    UpdateConnectionLastActive();

    // RCV-5: one parse/ordering state per sender, so senders sharing this channel
    // cannot delete each other's subjects or mix their tx_seq spaces and clocks.
    const double NowS = FPlatformTime::Seconds();
    const uint64 StreamKey = Streams.ResolveKey(Meta.subject_names);
    O3DS::ReceiverStream& Stream = Streams.Acquire(StreamKey, NowS, &Meta.subject_names);

    if (Meta.tx_seq == 0)
    {
        // Legacy sender (no tx_seq): timestamp ordering, per stream.
        HandleLegacyFrame(Subject, Buffer, TimestampSeconds, Meta, Stream);
        return;
    }

    O3DS::Frame Frame;
    Frame.seq = Meta.tx_seq;
    Frame.wallclock_us = Meta.tx_wallclock_us;
    Frame.epoch = Meta.frame_epoch;
    Frame.local_recv_us = ArrivalEpochUs; // true arrival instant - see Frame's doc comment
    Frame.bytes.assign(reinterpret_cast<const char*>(Buffer.GetData()),
        reinterpret_cast<const char*>(Buffer.GetData()) + Buffer.Num());

    LastGateSubjectLabel = Subject;

    Stream.gate.Push(std::move(Frame), NowS, [this, StreamKey](O3DS::Frame&& F) { EmitGatedFrame(StreamKey, std::move(F)); });
    ReportGateMetricsDelta();
}

/** Legacy (pre-A1) path for senders that don't set tx_seq: SubjectList.time-based
 *  dedup/reorder suppression first, then parse and apply. The ordering decision runs
 *  before Parse() so a dropped frame never changes parse state (ADR 0005 (ix)). */
void FO3DReceiverSource::HandleLegacyFrame(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, const O3DS::PacketMeta& Meta, O3DS::ReceiverStream& Stream)
{
    const double ParseStartWall = FPlatformTime::Seconds();
    const bool bDebugParse = CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0;

    // RCV-34: a silence or timestamp-jump reset here only forgets this stream's last
    // applied time; the gate, clock estimator and concealment state are untouched.
    const O3DS::LegacyOrdering::Decision Decision = Stream.legacy.Check(Meta.time, ParseStartWall, GetLegacyOrderingConfig());
    if (bDebugParse && Stream.legacy.LastCheckReset())
    {
        UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Reset legacy ordering window (new=%.6f)"), Meta.time);
    }
    if (Decision != O3DS::LegacyOrdering::Decision::Apply)
    {
        if (bDebugParse)
        {
            UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Dropping %s frame t=%.6f"),
                Decision == O3DS::LegacyOrdering::Decision::Duplicate ? TEXT("duplicate") : TEXT("out-of-order"), Meta.time);
        }
        FO3DPerformanceMetrics::Get().RecordReceiverFrameDropped();
        return;
    }

    std::vector<O3DS::ParsedSubjectInfo> Touched;
    const double ParseTimingStart = FPlatformTime::Seconds();
    if (!ParseSubjectListRaw(Stream.subjects, Subject, reinterpret_cast<const char*>(Buffer.GetData()), (size_t)Buffer.Num(), Touched))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }
    const double ParseTimeMs = (FPlatformTime::Seconds() - ParseTimingStart) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordParseTimeMs(ParseTimeMs);

    // Record frame applied
    FO3DPerformanceMetrics::Get().RecordFrameApplied();

    // Record round-trip latency if we have the timestamp
    double LatencyMs = (FPlatformTime::Seconds() - TimestampSeconds) * 1000.0;
    if (LatencyMs >= 0.0 && LatencyMs < 10000.0)  // sanity check: latency should be < 10 seconds
    {
        FO3DPerformanceMetrics::Get().RecordFrameLatency(LatencyMs);
    }

    // Track per-operation timing for bottleneck identification
    const double PoseExtractionStartTime = FPlatformTime::Seconds();
    const int32 PoseUpdateCount = PublishTouchedSubjects(Stream.subjects, Touched, -1.0);
    const double PoseExtractionTimeMs = (FPlatformTime::Seconds() - PoseExtractionStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordPoseExtractionTimeMs(PoseExtractionTimeMs);

    // Record pose and skeleton updates
    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    // Update active subject count
    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(SubjectLastUpdateTime.Num());

    // Record total frame processing time
    const double TotalProcessingTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordTotalProcessingTimeMs(TotalProcessingTimeMs);

    if (bDebugParse)
    {
        const double ParseEnd = FPlatformTime::Seconds();
        UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed subject list (subjects=%d bytes=%d dt=%.6fms)"),
            PoseUpdateCount, Buffer.Num(), (ParseEnd - ParseStartWall) * 1000.0);
    }
}

/** The A1 ReorderGate's emit callback: full parse + apply for a frame the gate has
 *  determined is in-order (called synchronously from Push, or later from Flush for
 *  a frame that was buffered waiting on a gap). Maps the sender's tx_wallclock onto
 *  local engine time (A2.b/A2.c) instead of the legacy path's "apply-time" stamp. */
void FO3DReceiverSource::EmitGatedFrame(uint64 StreamKey, O3DS::Frame&& Frame)
{
    if (!CanPublish() || !bIsValid)
    {
        return;
    }

    // The stream can only be missing if it was dropped between Push and a later
    // Flush (idle prune or table eviction); its buffered frames go with it.
    O3DS::ReceiverStream* Stream = Streams.Find(StreamKey);
    if (!Stream)
    {
        return;
    }

    const double ParseStartWall = FPlatformTime::Seconds();

    std::vector<O3DS::ParsedSubjectInfo> Touched;
    if (!ParseSubjectListRaw(Stream->subjects, LastGateSubjectLabel, Frame.bytes.data(), Frame.bytes.size(), Touched))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }
    const double ParseTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordParseTimeMs(ParseTimeMs);

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
    auto Sample = Stream->clock.Observe(Frame.wallclock_us, Frame.local_recv_us);
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

    const double PoseExtractionStartTime = FPlatformTime::Seconds();
    const int32 PoseUpdateCount = PublishTouchedSubjects(Stream->subjects, Touched, MappedWorldTimeSeconds);
    const double PoseExtractionTimeMs = (FPlatformTime::Seconds() - PoseExtractionStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordPoseExtractionTimeMs(PoseExtractionTimeMs);

    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(SubjectLastUpdateTime.Num());

    const double TotalProcessingTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordTotalProcessingTimeMs(TotalProcessingTimeMs);

    if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
    {
        UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed gated subject list (subjects=%d bytes=%d seq=%llu)"),
            PoseUpdateCount, (int32)Frame.bytes.size(), Frame.seq);
    }
}

/** Report the ReorderGate's stats as a delta against the last-reported snapshot (so
 *  multiple receiver sources correctly aggregate into the shared metrics singleton,
 *  the same convention as FramesReceived etc. - see FReceiverMetrics's doc comment). */
void FO3DReceiverSource::ReportGateMetricsDelta()
{
    auto Delta = [](uint64 NewVal, uint64 OldVal) -> uint64 { return NewVal >= OldVal ? (NewVal - OldVal) : 0; };

    uint64 DeltaDup = 0, DeltaStale = 0, DeltaLost = 0, DeltaReordered = 0;
    int32 Pending = 0;
    Streams.ForEach([&](uint64, O3DS::ReceiverStream& Stream)
    {
        const O3DS::ReorderStats& Stats = Stream.gate.Stats();
        DeltaDup += Delta(Stats.dup_dropped, Stream.reportedGateStats.dup_dropped);
        DeltaStale += Delta(Stats.stale_dropped, Stream.reportedGateStats.stale_dropped);
        DeltaLost += Delta(Stats.lost, Stream.reportedGateStats.lost);
        DeltaReordered += Delta(Stats.reordered, Stream.reportedGateStats.reordered);
        Pending += static_cast<int32>(Stream.gate.PendingCount());
        Stream.reportedGateStats = Stats;
    });

    FO3DPerformanceMetrics& Metrics = FO3DPerformanceMetrics::Get();
    if (DeltaDup) Metrics.RecordGateDupDropped(DeltaDup);
    if (DeltaStale) Metrics.RecordGateStaleDropped(DeltaStale);
    if (DeltaLost) Metrics.RecordGateLost(DeltaLost);
    if (DeltaReordered) Metrics.RecordGateReordered(DeltaReordered);
    if (DeltaDup || DeltaStale) Metrics.RecordReceiverFrameDropped(DeltaDup + DeltaStale);

    Metrics.SetGateBufferOccupancy(Pending);
}

/** Casts Settings to access concealment config - see the header's own doc comment. */
const UO3DReceiverSourceSettings* FO3DReceiverSource::GetConcealmentSettings() const
{
    return Cast<UO3DReceiverSourceSettings>(Settings);
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

    const UO3DReceiverSourceSettings* ConcealmentSettings = GetConcealmentSettings();
    O3DS::ConcealmentConfig Config;
    Config.starvationThresholdSeconds = FMath::Max(0.0, (double)(ConcealmentSettings ? ConcealmentSettings->StarvationThresholdMs : 50.0f) / 1000.0);
    Config.maxConcealHorizonSeconds = FMath::Max(0.0, (double)(ConcealmentSettings ? ConcealmentSettings->MaxHorizonMs : 150.0f) / 1000.0);
    Config.correctionWindowSeconds = FMath::Max(0.0, (double)(ConcealmentSettings ? ConcealmentSettings->CorrectionWindowMs : 100.0f) / 1000.0);
    Config.renderAheadSeconds = FMath::Max(0.0, (double)(ConcealmentSettings ? ConcealmentSettings->RenderAheadMs : 0.0f) / 1000.0);

    TUniquePtr<O3DS::ConcealmentEngine> NewEngine = MakeUnique<O3DS::ConcealmentEngine>(std::make_unique<O3DS::LinearPredictor>(), Config);
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
    const UO3DReceiverSourceSettings* ConcealmentSettings = GetConcealmentSettings();
    if (ConcealmentSettings != nullptr && !ConcealmentSettings->bEnableConcealment)
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
 *  interpolation) and push one if so. Engines are only fed real frames whose
 *  PresentationTimeSeconds is EmitGatedFrame's MappedWorldTimeSeconds - which is
 *  already expressed in the local FPlatformTime::Seconds() domain (it converts
 *  mapped_presentation_time_us to platform time using a NowEpochUs/NowPlatformS
 *  anchor pair taken at that instant). So "now" in that same domain is simply a
 *  fresh FPlatformTime::Seconds() reading; LastClockOffsetEstimateUs must NOT be
 *  added here too - that offset is already baked into each frame's own
 *  MappedWorldTimeSeconds, and re-applying it would skew TryConceal's gap/horizon
 *  math by roughly the current send-to-receive offset. bHasClockOffsetEstimate is
 *  still used below purely as "has the gated path observed at least one real
 *  frame yet", not to adjust the time base. */
void FO3DReceiverSource::TickConcealment()
{
    if (!CanPublish() || !bHasClockOffsetEstimate || SubjectConcealment.Num() == 0)
    {
        return;
    }

    const UO3DReceiverSourceSettings* ConcealmentSettings = GetConcealmentSettings();
    if (ConcealmentSettings != nullptr && !ConcealmentSettings->bEnableConcealment)
    {
        return;
    }

    const double TNow = FPlatformTime::Seconds();

    for (TPair<FName, TUniquePtr<O3DS::ConcealmentEngine>>& Pair : SubjectConcealment)
    {
        if (!Pair.Value.IsValid())
        {
            continue;
        }

        O3DS::PoseSample Predicted;
        if (!Pair.Value->TryConceal(TNow, Predicted))
        {
            // No actual gap right now (real data is flowing on schedule) -
            // C1.c latency-hiding only applies on top of that healthy case
            // (see TryRenderAhead()'s own doc comment on why it must not run
            // during a genuine TryConceal()-handled gap/correction). Opt-in,
            // default OFF: returns false immediately when disabled.
            if (!Pair.Value->TryRenderAhead(Predicted))
            {
                continue;
            }
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

    uint64 DeltaConcealed = 0, DeltaFallback = 0, DeltaCorrection = 0, DeltaRecovery = 0, DeltaRenderAhead = 0;
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
        DeltaRenderAhead += Delta(Current.renderAheadFrameCount, Prev.renderAheadFrameCount);

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
    if (DeltaRenderAhead) Metrics.RecordConcealmentRenderAheadFrames(DeltaRenderAhead);
    if (bHasAnyRecovery)
    {
        Metrics.SetConcealmentPredictionError(LastTransErr, LastRotErrDeg);
        Metrics.SetConcealmentPop(LastPopTrans, LastPopRotDeg);
    }
}

/** Parse the FlatBuffer payload into the sender stream's SubjectList, reporting which
 *  subjects this packet touched. */
bool FO3DReceiverSource::ParseSubjectListRaw(O3DS::SubjectList& List, const FString& Subject, const char* Data, size_t Len, std::vector<O3DS::ParsedSubjectInfo>& OutTouched)
{
    if (!List.Parse(Data, Len, nullptr, true, &OutTouched))
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Parse failed for subject '%s' (%d bytes): %s"), *Subject, (int32)Len, UTF8_TO_TCHAR(List.mError.c_str()));
        return false;
    }
    return true;
}

/** Publish only the subjects this packet touched (RCV-5). Subjects the stream knows
 *  but the packet did not mention are left alone, so RemoveInactiveSubjects can
 *  retire them. */
int32 FO3DReceiverSource::PublishTouchedSubjects(O3DS::SubjectList& List, const std::vector<O3DS::ParsedSubjectInfo>& Touched, double WorldTimeSecondsOverride)
{
    TArray<FName> BoneNames;
    TArray<int32> BoneParents;
    TArray<FTransform> BoneTransforms;
    TArray<FName> CurveNames;
    TArray<float> CurveValues;

    int32 Count = 0;
    for (const O3DS::ParsedSubjectInfo& Info : Touched)
    {
        if (O3DS::Subject* SubjectPtr = List.findSubject(Info.name))
        {
            ProcessParsedSubject(SubjectPtr, List.mTime, WorldTimeSecondsOverride, Info.fullDescriptor, BoneNames, BoneParents, BoneTransforms, CurveNames, CurveValues);
            ++Count;
        }
    }
    return Count;
}

O3DS::LegacyOrderingConfig FO3DReceiverSource::GetLegacyOrderingConfig() const
{
    O3DS::LegacyOrderingConfig Config;
    Config.dropOutOfOrder = CVarO3DReceiverDropOutOfOrder.GetValueOnAnyThread() != 0;
    Config.silenceResetSeconds = CVarO3DReceiverSilenceResetSeconds.GetValueOnAnyThread();
    Config.timestampJumpResetSeconds = CVarO3DReceiverTimestampJumpResetSeconds.GetValueOnAnyThread();
    return Config;
}

bool FO3DReceiverSource::CanPublish() const
{
    return Client != nullptr || (TestStaticPushHook && TestFramePushHook);
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
        // RCV-14: parent ids index this list, so skipping an entry would shift every
        // later parent. The core parser never leaves a null entry (a nameless node
        // gets a placeholder name), so treat one as a malformed frame.
        if (!TransformPtr)
        {
            return false;
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
void FO3DReceiverSource::ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, bool bFullDescriptor, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues)
{
    if (!SubjectPtr)
    {
        return;
    }

    const FString SubjectNameUtf8 = UTF8_TO_TCHAR(SubjectPtr->mName.c_str());
    const FName SubjectFName(*SubjectNameUtf8);

    // RCV-4: the cached bone names and parents are reused only when the skeleton
    // fingerprint (bone count, names and parent ids) is unchanged and this packet did
    // not carry a full descriptor for the subject. Hashing the name bytes is cheap
    // next to building FNames, which is what the cache saves.
    FSubjectTransformCache* ExistingCache = SubjectTransformCaches.Find(SubjectFName);
    const uint64 Fingerprint = O3DS::SkeletonFingerprint(*SubjectPtr);

    if (!bFullDescriptor && ExistingCache && ExistingCache->SkeletonFingerprint == Fingerprint)
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
            // RCV-14: see BuildSubjectPose; a null entry would misalign parents.
            if (!TransformPtr)
                return;

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
        Cache.SkeletonFingerprint = Fingerprint;
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
        PushSubjectStaticData(SubjectKey, BoneNames, BoneParents, CurveNames, SkeletonHash, !InitializedSubjects.Contains(SubjectFName));
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
    BuildAudioMetaDefaults().Apply(Meta);
}

TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> FO3DReceiverSource::MakeAudioSink() const
{
    return MakeShared<FAudioSink, ESPMode::ThreadSafe>(BuildAudioMetaDefaults());
}

FO3DReceiverSource::FAudioMetaDefaults FO3DReceiverSource::BuildAudioMetaDefaults() const
{
    FAudioMetaDefaults Defaults;
    Defaults.SourceGuid = SourceGuid;
    Defaults.bEnableAudio = ActiveConfig.Audio.bEnableAudio;
    Defaults.StreamId = ActiveConfig.StreamId;
    Defaults.SampleRate = ActiveConfig.Audio.SampleRate;
    Defaults.NumChannels = ActiveConfig.Audio.NumChannels;
    return Defaults;
}

void FO3DReceiverSource::FAudioMetaDefaults::Apply(O3DS::FAudioFrameMeta& Meta) const
{
    Meta.SourceGuid = SourceGuid;

    // Audio stream label is now provided directly by WebRTC transport via per-subject audio callback (OnAudioReceivedEx)
    // which receives explicit subject labels from LiveKit FFI. No fallback logic needed.
    if (bEnableAudio && Meta.StreamLabel.IsEmpty())
    {
        Meta.StreamLabel = StreamId.IsEmpty() ? TEXT("o3ds:mix") : StreamId;
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
        Meta.SubjectName = StreamId.IsEmpty() ? FString(TEXT("Open3DReceiver")) : StreamId;
    }

    if (Meta.SampleRate <= 0)
    {
        Meta.SampleRate = (SampleRate > 0) ? SampleRate : 48000;
    }

    if (Meta.NumChannels <= 0)
    {
        Meta.NumChannels = (NumChannels > 0) ? NumChannels : 1;
    }

    if (Meta.TimestampSec <= 0.0)
    {
        Meta.TimestampSec = FPlatformTime::Seconds();
    }
}

void FO3DReceiverSource::PushSubjectStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, uint64 DescriptorHash, bool bFirstPushThisSession)
{
    if (TestStaticPushHook)
    {
        TestStaticPushHook(SubjectKey, BoneNames, BoneParents, CurveNames, bFirstPushThisSession);
        return;
    }

    if (!Client)
    {
        return;
    }

    // RCV-7: create the LiveLink subject only on the first push of this session, and
    // only if LiveLink doesn't already have it (for example from a previous transport
    // session or a preset the user loaded). Calling CreateSubject again for an
    // existing subject either fails with a warning or replaces the user's
    // per-subject settings (preprocessors, interpolation, translators). Later
    // hierarchy or curve changes re-push static data only.
    if (bFirstPushThisSession && Client->GetSubjectSettings(SubjectKey) == nullptr)
    {
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
    }

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
    if (TestFramePushHook)
    {
        TestFramePushHook(SubjectKey, BoneTransforms, CurveValues, (WorldTimeSecondsOverride >= 0.0) ? WorldTimeSecondsOverride : FPlatformTime::Seconds());
        return;
    }

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

void FO3DReceiverSource::ResetStreamState()
{
    // A transport start or stop begins a clean session: every sender stream's gate,
    // clock estimator, legacy ordering and parse state go. The legacy path's own
    // silence and timestamp-jump resets no longer come here; they stay inside their
    // stream's LegacyOrdering (RCV-34).
    Streams.Clear();

    // C1: a publisher restart invalidates every subject's prediction history
    // equally (ties to C1.a's "Reset the predictor on... publisher restart"),
    // and the cached clock-offset estimate is now stale too.
    SubjectConcealment.Empty();
    PrevConcealmentMetricsBySubject.Empty();
    bHasClockOffsetEstimate = false;
    LastClockOffsetEstimateUs = 0;
}

// ── Control channel (docs/adr/0011-control-channel.md, item 8) ─────────────────────────────

bool FO3DReceiverSource::IsControlEnabled() const
{
    const UO3DReceiverSourceSettings* SourceSettingsObject = GetConcealmentSettings();
    const EO3DControlAcceptMode PerSource = SourceSettingsObject ? SourceSettingsObject->ControlAccept : EO3DControlAcceptMode::ProjectDefault;
    return UO3DControlSettings::IsReceiveEnabled(PerSource);
}

void FO3DReceiverSource::ApplyControlConfig()
{
    const UO3DControlSettings* Project = GetDefault<UO3DControlSettings>();
    O3DS::Control::ReceiverConfig Config;
    Config.max_keys_per_source = static_cast<size_t>(FMath::Clamp(Project->MaxControlKeysPerSource, 1, static_cast<int32>(O3DS::ControlLimits::kMaxKeysPerSource)));
    Config.max_live_bytes_per_s = static_cast<double>(FMath::Max(Project->MaxControlLiveBytesPerSecond, 2048));
    Config.max_snapshot_bytes_per_s = static_cast<double>(FMath::Max(Project->MaxControlSnapshotBytesPerSecond, 2048));
    for (const FString& Prefix : Project->ControlAllowlist)
    {
        if (!Prefix.IsEmpty())
        {
            Config.allow_prefixes.push_back(O3DControl::ToUtf8(Prefix));
        }
    }
    ControlReceiver.SetConfig(Config);
    ControlAligner.SetMaxHold(FMath::Max(Project->MaxAlignmentHoldMs, 0) / 1000.0);
}

void FO3DReceiverSource::HandleControlPayload(const TArray<uint8>& Payload, const FString& /*StreamId*/, double /*ReceiveTimeSec*/)
{
    check(IsInGameThread());
    if (!IsControlEnabled())
    {
        ++ControlPayloadsDroppedDisabled;
        return;
    }
    bControlWasEnabled = true;

    // Core clocks are this receiver's own FPlatformTime, taken now on the game thread.
    const double NowS = FPlatformTime::Seconds();
    std::vector<O3DS::Control::Change> Changes;
    const O3DS::Control::ParseError Error = ControlReceiver.Submit(Payload.GetData(), static_cast<size_t>(Payload.Num()), NowS, Changes);
    if (Error != O3DS::Control::ParseError::None)
    {
        UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Rejected control message (%d bytes): %s"), Payload.Num(), UTF8_TO_TCHAR(O3DS::Control::ToString(Error)));
        return;
    }
    for (O3DS::Control::Change& Change : Changes)
    {
        RouteControlChange(MoveTemp(Change), NowS);
    }
}

void FO3DReceiverSource::RouteControlChange(O3DS::Control::Change&& Change, double NowS)
{
    ControlSourcesSeen.Add(O3DControl::FromUtf8(Change.source_id));
    if (GetDefault<UO3DControlSettings>()->bAlignControlToMocap)
    {
        ControlAligner.Push(MoveTemp(Change), NowS);
    }
    else
    {
        PublishControlChange(Change);
    }
}

void FO3DReceiverSource::PublishControlChange(const O3DS::Control::Change& Change) const
{
    FO3DControlBus::Publish(O3DControl::FromCore(Change, ActiveConfig.StreamId));
}

void FO3DReceiverSource::DiscardControlState()
{
    ControlReceiver.Reset();
    std::vector<O3DS::Control::Change> Held;
    ControlAligner.Flush(Held); // dropped, not published
    for (const FString& SourceId : ControlSourcesSeen)
    {
        FO3DControlBus::ForgetSource(SourceId);
    }
    ControlSourcesSeen.Reset();
    bControlWasEnabled = false;
}

bool FO3DReceiverSource::GetPresentedSenderTimeUs(const std::string& SourceId, uint64_t& OutUs)
{
    // Timecode mode presents frames by timecode, which this channel does not carry: no alignment.
    const ELiveLinkSourceMode Mode = Settings ? Settings->Mode : ELiveLinkSourceMode::EngineTime;
    if (Mode == ELiveLinkSourceMode::Timecode)
    {
        return false;
    }

    const std::vector<std::string>* Subjects = ControlReceiver.FindMocapSubjects(SourceId);
    if (Subjects == nullptr || Subjects->empty())
    {
        return false; // a control-only sender
    }
    O3DS::ReceiverStream* Stream = Streams.Find(Streams.ResolveKey(*Subjects));
    if (Stream == nullptr || FPlatformTime::Seconds() - Stream->lastSeenS > AlignmentStreamLivenessSeconds)
    {
        return false; // no mocap from that sender here, or it paused: never hold cues for a stream that is not moving
    }

    // SubjectList.time of the newest frame handed to LiveLink, on the sender's clock - the same
    // clock the control publisher stamps (ADR 0011 item 9). In EngineTime mode LiveLink shows the
    // buffer EngineTimeOffset seconds behind, so the pose on screen is that much older.
    double PresentedS = Stream->subjects.mTime;
    if (Mode == ELiveLinkSourceMode::EngineTime && Settings)
    {
        PresentedS -= static_cast<double>(Settings->BufferSettings.EngineTimeOffset);
    }
    OutUs = PresentedS > 0.0 ? static_cast<uint64_t>(PresentedS * 1.0e6) : 0;
    return true;
}

void FO3DReceiverSource::TickControl(double NowS)
{
    if (!IsControlEnabled())
    {
        if (bControlWasEnabled)
        {
            DiscardControlState(); // silently: no Cleared delegates (ADR 0011 item 8)
        }
        return;
    }

    std::vector<O3DS::Control::Change> Changes;
    ControlReceiver.Tick(NowS, Changes); // incomplete snapshots, quiet sources
    for (O3DS::Control::Change& Change : Changes)
    {
        RouteControlChange(MoveTemp(Change), NowS);
    }

    std::vector<O3DS::Control::Change> Released;
    if (GetDefault<UO3DControlSettings>()->bAlignControlToMocap)
    {
        ControlAligner.SetMaxHold(FMath::Max(GetDefault<UO3DControlSettings>()->MaxAlignmentHoldMs, 0) / 1000.0);
        ControlAligner.Release(NowS, [this](const std::string& SourceId, uint64_t& OutUs) { return GetPresentedSenderTimeUs(SourceId, OutUs); }, Released);
    }
    else
    {
        ControlAligner.Flush(Released); // alignment turned off while changes were held
    }
    for (const O3DS::Control::Change& Change : Released)
    {
        PublishControlChange(Change);
    }
}

#undef LOCTEXT_NAMESPACE
