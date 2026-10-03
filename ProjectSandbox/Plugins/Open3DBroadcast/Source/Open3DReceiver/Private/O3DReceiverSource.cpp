// Copyright (c) Open3DStream Contributors

#include "O3DReceiverSource.h"

#include "O3DLiveLinkPublisher.h"
#include "O3DReceiverConcealment.h"
#include "O3DReceiverControlRouter.h"
#include "O3DReceiverStreamScheduler.h"
#include "O3DReceiverFrameDecoder.h"

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
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/reorder_gate.h"
#include "o3ds/sequencing.h"
THIRD_PARTY_INCLUDES_END

#include <utility>

#define LOCTEXT_NAMESPACE "O3DReceiverSource"

// Receiver-side diagnostics
static TAutoConsoleVariable<int32> CVarO3DReceiverDebugParse(
    TEXT("o3ds.Receiver.DebugParse"),
    0,
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
    , FrameDecoder(MakeUnique<FO3DReceiverFrameDecoder>())
    , Publisher(MakeUnique<FO3DLiveLinkPublisher>())
{
    // Declared before the decoder and publisher, so created here rather than in the list (C5038).
    Concealment = MakeUnique<FO3DReceiverConcealment>();
    ControlRouter = MakeUnique<FO3DReceiverControlRouter>();
    Scheduler = MakeUnique<FO3DReceiverStreamScheduler>(
        [this](O3DS::ReceiverStream& Stream, const FString& Label, const char* Data, size_t Len, double LegacyTimestampSeconds, const O3DS::Frame* GatedFrame)
        {
            ApplyReleasedFrame(Stream, Label, Data, Len, LegacyTimestampSeconds, GatedFrame);
        });
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
    Publisher->SetClient(InClient, InSourceGuid);
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
    // new frame arrives to trigger it via Push. Game thread, like HandleSerializedFrame.
    const double NowS = FPlatformTime::Seconds();
    Scheduler->Flush(NowS);

    // C1: synthesize a frame for any subject whose real data has starved
    // beyond LiveLink's own interpolation (FO3DReceiverConcealment::Tick).
    Concealment->Tick(GetConcealmentSettings(), CanPublish(), FPlatformTime::Seconds(),
        [this](FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double Time)
        {
            Publisher->PublishSyntheticFrame(Subject, BoneTransforms, CurveValues, Time);
        });

    ControlRouter->Tick(IsControlEnabled(), NowS, ActiveConfig.StreamId,
        [this](const std::vector<std::string>& MocapSubjects, uint64_t& OutUs) { return GetPresentedSenderTimeUs(MocapSubjects, OutUs); });

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
        if (ActiveReceiver->GetCapabilities().bAudioReceive)
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
    if (ActiveReceiver->GetCapabilities().bControl)
    {
        ActiveControlSink = MakeShared<FControlSink, ESPMode::ThreadSafe>(TWeakPtr<FO3DReceiverSource>(AsShared()));
        ActiveReceiver->SetControlSink(ActiveControlSink);
        ControlRouter->ApplyConfig();
    }
    const FO3DTransportResult StartResult = ActiveReceiver->Start();
    if (!StartResult.IsOk())
    {
        UE_LOG(LogO3DReceiverSource, Warning, TEXT("Failed to start transport '%s': %s"), *ActiveConfig.Transport.ToString(), *LexToString(StartResult));
        ActiveReceiver->Stop();
        ActiveReceiver->SetConsumer(nullptr);
        if (ActiveAudioSink.IsValid() && ActiveReceiver->GetCapabilities().bAudioReceive)
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
        if (ActiveConfig.Audio.bEnableAudio && ActiveReceiver->GetCapabilities().bAudioReceive)
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
    ControlRouter->FlushHeld(ActiveConfig.StreamId);
    Publisher->Reset();
    // RCV-5/RCV-34: cached bone names from the previous session must not survive a
    // restart, like the other per-subject maps above.
    FrameDecoder->Reset();
    bLoggedActiveState = false;
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
            // The view is over this function's own copy of the options (WP-A1 PR 5a).
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
    Publisher->RemoveInactiveSubjects(Now, InactivityThresholdSeconds, [this](FName Subject)
    {
        FrameDecoder->ForgetSubject(Subject);
        Concealment->ForgetSubject(Subject);  // C1: drop the per-subject predictor/state too
    });

    // Senders that went quiet: drop their parse and ordering state too, so a
    // restarted sender starts clean and the table does not keep dead streams.
    Scheduler->PruneIdle(Now, InactivityThresholdSeconds);
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
            // D8 (ADR 0009): a frame this build may not apply says why, so the user knows which
            // side to update.
            if (Meta.check == O3DS::Wire::FrameCheck::VersionTooNew)
            {
                UE_LOG(LogO3DReceiverSource, Warning, TEXT("Sender on '%s' requires wire protocol %d; this receiver implements %d. Update this receiver."),
                    *Subject, static_cast<int32>(Meta.min_reader_version), static_cast<int32>(O3DS::Wire::kProtocolVersion));
            }
            else if (SuppressedMalformedWarnings > 0)
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

    // Ordering per sender stream (RCV-5): legacy timestamp ordering or the reorder gate. Released
    // packets come back through ApplyReleasedFrame.
    Scheduler->Push(Subject, Buffer, TimestampSeconds, Meta, ArrivalEpochUs, FPlatformTime::Seconds(), GetLegacyOrderingConfig(),
        CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0);
}

/** Parse and publish one packet the scheduler released in order. Legacy packets (no tx_seq) are
 *  stamped with the apply time; gated packets map the sender's tx_wallclock onto local engine time
 *  (A2.b/A2.c). */
void FO3DReceiverSource::ApplyReleasedFrame(O3DS::ReceiverStream& Stream, const FString& Label, const char* Data, size_t Len, double LegacyTimestampSeconds, const O3DS::Frame* GatedFrame)
{
    // A gated frame can be released later, from Flush; the source may no longer publish by then.
    if (GatedFrame != nullptr && (!CanPublish() || !bIsValid))
    {
        return;
    }

    const double ParseStartWall = FPlatformTime::Seconds();

    // ADR 0005 (ix): a gated frame is parsed with its sequence context, so an update the stream
    // cannot apply correctly (its full Subject was missed, or residual history broke at a gap) is
    // dropped and the subject is not pushed until the next full Subject. A frame skipped above, or
    // one that fails to parse, is not noted as applied, so the next frame sees the gap.
    O3DS::ParseContext Context;
    if (GatedFrame != nullptr)
    {
        Context = O3DS::MakeParseContext(Stream, GatedFrame->seq, GatedFrame->epoch);
    }
    const uint64 DroppedBefore = Stream.subjects.mUpdatesDroppedUnsynced;

    std::vector<O3DS::ParsedSubjectInfo> Touched;
    if (!ParseSubjectListRaw(Stream.subjects, Label, Data, Len, Touched, GatedFrame != nullptr ? &Context : nullptr))
    {
        FO3DPerformanceMetrics::Get().RecordDeserializationError();
        return;
    }
    if (GatedFrame != nullptr)
    {
        O3DS::NoteFrameApplied(Stream, GatedFrame->seq, GatedFrame->epoch);
    }
    if (Stream.subjects.mUpdatesDroppedUnsynced > DroppedBefore)
    {
        FO3DPerformanceMetrics::Get().RecordUpdatesAwaitingFullSync(Stream.subjects.mUpdatesDroppedUnsynced - DroppedBefore);
    }
    const double ParseTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordParseTimeMs(ParseTimeMs);

    FO3DPerformanceMetrics::Get().RecordFrameApplied();

    double WorldTimeSecondsOverride = -1.0;
    if (GatedFrame == nullptr)
    {
        // Receive-to-apply latency (RCV-33): the transport stamps the packet with
        // FPlatformTime::Seconds() when it receives it.
        const double LatencyMs = (FPlatformTime::Seconds() - LegacyTimestampSeconds) * 1000.0;
        if (LatencyMs >= 0.0 && LatencyMs < 10000.0)  // sanity check: latency should be < 10 seconds
        {
            FO3DPerformanceMetrics::Get().RecordFrameLatency(LatencyMs);
        }
    }
    else
    {
        // A2.b/A2.c: map tx_wallclock_us onto local engine time. Observe() uses
        // local_recv_us (the true arrival instant, not "now") so gate-buffering wait never
        // gets misattributed as network jitter. The estimator's result is in epoch
        // microseconds; the (NowEpochUs, NowPlatformS) pair taken back-to-back right here is
        // purely an anchor to translate that into FPlatformTime::Seconds() terms - the two
        // clocks tick at the same rate, so this conversion holds regardless of when it's
        // taken, as long as both readings are simultaneous. Falls back to "now" (this frame's
        // true arrival time, via Observe()'s own tx_wallclock_us==0 handling) when the sender
        // didn't set tx_wallclock_us, matching the roadmap's A2.c legacy-timestamp fallback.
        auto Sample = Stream.clock.Observe(GatedFrame->wallclock_us, GatedFrame->local_recv_us);
        const uint64 NowEpochUs = O3DS::NowUtcMicros();
        const double NowPlatformS = FPlatformTime::Seconds();
        WorldTimeSecondsOverride = NowPlatformS + (double)((int64)Sample.mapped_presentation_time_us - (int64)NowEpochUs) / 1.0e6;

        // Receive-to-apply latency (RCV-33), gate wait included: both readings are this
        // receiver's own UTC clock.
        if (GatedFrame->local_recv_us != 0)
        {
            const double LatencyMs = (double)((int64)NowEpochUs - (int64)GatedFrame->local_recv_us) / 1000.0;
            if (LatencyMs >= 0.0 && LatencyMs < 10000.0)
            {
                FO3DPerformanceMetrics::Get().RecordFrameLatency(LatencyMs);
            }
        }

        if (GatedFrame->wallclock_us != 0)
        {
            // C1: offset_estimate_us is hardwired to 0 in ClockOffsetEstimator::Observe()'s
            // legacy (tx_wallclock_us == 0) branch, so it is noted only for timestamped frames,
            // like the metrics below.
            Concealment->NoteClockOffset(Sample.offset_estimate_us);

            FO3DPerformanceMetrics::Get().RecordClockOffsetSampleMs(
                (double)Sample.offset_estimate_us / 1000.0,
                (double)Sample.excess_delay_us / 1000.0);
        }
    }

    // Track per-operation timing for bottleneck identification
    const double PoseExtractionStartTime = FPlatformTime::Seconds();
    const int32 PoseUpdateCount = PublishTouchedSubjects(Stream.subjects, Touched, WorldTimeSecondsOverride);
    const double PoseExtractionTimeMs = (FPlatformTime::Seconds() - PoseExtractionStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordPoseExtractionTimeMs(PoseExtractionTimeMs);

    if (PoseUpdateCount > 0)
    {
        FO3DPerformanceMetrics::Get().RecordPoseUpdate();
    }

    FO3DPerformanceMetrics::Get().SetReceiverActiveSubjectCount(Publisher->GetActiveSubjectCount());

    const double TotalProcessingTimeMs = (FPlatformTime::Seconds() - ParseStartWall) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordTotalProcessingTimeMs(TotalProcessingTimeMs);

    if (CVarO3DReceiverDebugParse.GetValueOnAnyThread() != 0)
    {
        if (GatedFrame != nullptr)
        {
            UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed gated subject list (subjects=%d bytes=%d seq=%llu)"),
                PoseUpdateCount, (int32)Len, GatedFrame->seq);
        }
        else
        {
            UE_LOG(LogO3DReceiverSource, VeryVerbose, TEXT("Processed subject list (subjects=%d bytes=%d dt=%.6fms)"),
                PoseUpdateCount, (int32)Len, (FPlatformTime::Seconds() - ParseStartWall) * 1000.0);
        }
    }
}

/** Casts Settings to access concealment config - see the header's own doc comment. */
const UO3DReceiverSourceSettings* FO3DReceiverSource::GetConcealmentSettings() const
{
    return Cast<UO3DReceiverSourceSettings>(Settings);
}

bool FO3DReceiverSource::ParseSubjectListRaw(O3DS::SubjectList& List, const FString& Subject, const char* Data, size_t Len, std::vector<O3DS::ParsedSubjectInfo>& OutTouched,
    const O3DS::ParseContext* Context)
{
    if (!List.Parse(Data, Len, nullptr, true, &OutTouched, Context))
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
    int32 Count = 0;
    for (const O3DS::ParsedSubjectInfo& Info : Touched)
    {
        if (O3DS::Subject* SubjectPtr = List.findSubject(Info.name))
        {
            ProcessParsedSubject(SubjectPtr, List.mTime, WorldTimeSecondsOverride, Info.fullDescriptor);
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
    return Publisher->CanPublish();
}

void FO3DReceiverSource::SetSourceGuid(const FGuid& InSourceGuid)
{
    SourceGuid = InSourceGuid;
    Publisher->SetSourceGuid(InSourceGuid);
}

void FO3DReceiverSource::SetTestPushHooks(
    TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>&, const TArray<int32>&, const TArray<FName>&, bool)> StaticHook,
    TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>&, const TArray<float>&, double)> FrameHook)
{
    Publisher->SetTestHooks(MoveTemp(StaticHook), MoveTemp(FrameHook));
}

/** Build LiveLink static/frame data and push it to the client for a single parsed subject. */
void FO3DReceiverSource::ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, bool bFullDescriptor)
{
    if (!SubjectPtr)
    {
        return;
    }

    // Names, parents, transforms and curves for LiveLink, with the topology cached per subject
    // (FO3DReceiverFrameDecoder, WP-A3). A subject without a usable pose is skipped.
    FO3DDecodedSubject Decoded;
    if (!FrameDecoder->Decode(*SubjectPtr, bFullDescriptor, Decoded))
    {
        return;
    }
    const FName SubjectFName = Decoded.SubjectName;
    const TArray<FTransform>& BoneTransforms = *Decoded.BoneTransforms;
    const TArray<FName>& CurveNames = *Decoded.CurveNames;
    const TArray<float>& CurveValues = *Decoded.CurveValues;

    const double LiveLinkStartTime = FPlatformTime::Seconds();

    // Static data when the subject is new this session or its names changed (FO3DLiveLinkPublisher).
    const bool bNeedStaticUpdate = Publisher->PublishStatic(Decoded);
    if (bNeedStaticUpdate)
    {
        FO3DPerformanceMetrics::Get().RecordSkeletonUpdate();
    }

    // C1: feed the real frame into this subject's concealment engine before
    // pushing it - only for the gated (A2) path, where WorldTimeSecondsOverride
    // is a real sender-clock-mapped time (>= 0.0); the legacy path has no
    // reliable clock domain to reason about gaps in (see the roadmap's C1.a
    // scope note and FO3DReceiverConcealment).
    if (WorldTimeSecondsOverride >= 0.0)
    {
        Concealment->ObserveRealFrame(GetConcealmentSettings(), SubjectFName, WorldTimeSecondsOverride, BoneTransforms, CurveValues, bNeedStaticUpdate);
    }

    Publisher->PublishFrame(SubjectFName, BoneTransforms, CurveNames, CurveValues, SubjectListTime, WorldTimeSecondsOverride, Decoded.CurveHash);

    const double LiveLinkPushTimeMs = (FPlatformTime::Seconds() - LiveLinkStartTime) * 1000.0;
    FO3DPerformanceMetrics::Get().RecordLiveLinkPushTimeMs(LiveLinkPushTimeMs);
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

void FO3DReceiverSource::ResetStreamState()
{
    // A transport start or stop begins a clean session: every sender stream's gate,
    // clock estimator, legacy ordering and parse state go. The legacy path's own
    // silence and timestamp-jump resets no longer come here; they stay inside their
    // stream's LegacyOrdering (RCV-34).
    Scheduler->Reset();

    // C1: a publisher restart invalidates every subject's prediction history, and the
    // cached clock-offset estimate is stale too.
    Concealment->Reset();
}

// ── Control channel (docs/adr/0011-control-channel.md, item 8) ─────────────────────────────

bool FO3DReceiverSource::IsControlEnabled() const
{
    const UO3DReceiverSourceSettings* SourceSettingsObject = GetConcealmentSettings();
    const EO3DControlAcceptMode PerSource = SourceSettingsObject ? SourceSettingsObject->ControlAccept : EO3DControlAcceptMode::ProjectDefault;
    return UO3DControlSettings::IsReceiveEnabled(PerSource);
}

void FO3DReceiverSource::HandleControlPayload(const TArray<uint8>& Payload, const FString& /*StreamId*/, double /*ReceiveTimeSec*/)
{
    // Core clocks are this receiver's own FPlatformTime, taken now on the game thread.
    ControlRouter->HandlePayload(IsControlEnabled(), Payload.GetData(), Payload.Num(), FPlatformTime::Seconds(), ActiveConfig.StreamId);
}

uint64 FO3DReceiverSource::GetControlPayloadsDroppedDisabled() const
{
    return ControlRouter->GetPayloadsDroppedDisabled();
}

const O3DS::Control::AlignerStats& FO3DReceiverSource::GetControlAlignerStats() const
{
    return ControlRouter->GetAlignerStats();
}

size_t FO3DReceiverSource::GetNumHeldControlChanges() const
{
    return ControlRouter->GetNumHeld();
}

bool FO3DReceiverSource::GetPresentedSenderTimeUs(const std::vector<std::string>& MocapSubjects, uint64_t& OutUs)
{
    // Timecode mode presents frames by timecode, which this channel does not carry: no alignment.
    const ELiveLinkSourceMode Mode = Settings ? Settings->Mode : ELiveLinkSourceMode::EngineTime;
    if (Mode == ELiveLinkSourceMode::Timecode)
    {
        return false;
    }

    O3DS::ReceiverStream* Stream = Scheduler->FindBySubjects(MocapSubjects);
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

#undef LOCTEXT_NAMESPACE
