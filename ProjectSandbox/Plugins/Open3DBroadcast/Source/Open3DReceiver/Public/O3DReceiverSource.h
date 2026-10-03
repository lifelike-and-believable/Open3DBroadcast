// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "ILiveLinkSource.h"
#include "Tickable.h"

#include "Transport/O3DReceiverInterface.h"
#include "O3DReceiverLogs.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
#include "o3ds/reorder_gate.h"
#include "o3ds/clock_offset.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/predict/concealment.h"
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <vector>

class ILiveLinkClient;
class FO3DReceiverFrameDecoder;
class FO3DLiveLinkPublisher;
class FO3DReceiverConcealment;
class FO3DReceiverStreamScheduler;

/**
 * LiveLink source implementation that consumes serialized Open3DStream frames via registered transports.
 */
class OPEN3DRECEIVER_API FO3DReceiverSource : public ILiveLinkSource, public TSharedFromThis<FO3DReceiverSource>, public FTickableGameObject
{
public:
    FO3DReceiverSource();
    explicit FO3DReceiverSource(const FO3DReceiverSourceConfig& InSettings);
    virtual ~FO3DReceiverSource();

    // ILiveLinkSource interface
    virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual bool RequestSourceShutdown() override;
    virtual FText GetSourceType() const override { return SourceType; }
    virtual FText GetSourceMachineName() const override { return SourceMachineName; }
    virtual FText GetSourceStatus() const override { return SourceStatus; }
    virtual bool IsSourceStillValid() const override { return Client != nullptr && bIsValid.load(); }
    virtual void Update() override;

    // Tickable interface
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override { return true; }
    virtual bool IsTickableWhenPaused() const override { return true; }
    virtual bool IsTickableInEditor() const override { return true; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FO3DReceiverSource, STATGROUP_Tickables); }

    // Settings helpers
    virtual void InitializeSettings(ULiveLinkSourceSettings* InSettings) override;
    virtual TSubclassOf<ULiveLinkSourceSettings> GetSettingsClass() const override;

    const FO3DReceiverSourceConfig& GetSourceSettings() const { return SourceSettings; }

private:
    // Test-only white-box access, defined in Public/Testing/O3DReceiverTesting.h (WP-T2).
    friend struct FO3DReceiverSourceTestAccessor;
    friend struct FO3DReceiverCorrectnessTestAccessor;
    friend struct FO3DReceiverSecretsTestAccess;

    class FSerializedConsumer;
    class FAudioSink;
    class FControlSink;

    // Builds the consumer StartTransport() hands to the transport. Defined in the .cpp,
    // where FSerializedConsumer is a complete type (tests call it through their accessor).
    static TSharedRef<ISerializedFrameConsumer, ESPMode::ThreadSafe> MakeSerializedConsumer(TWeakPtr<FO3DReceiverSource> Owner);

    /**
     * Immutable audio metadata defaults, snapshotted on the game thread when the transport
     * starts (WP-S5, RCV-1). The audio sink holds a copy instead of a back-reference to this
     * source, so an FFI or network thread never reads source state or pins the source.
     */
    struct FAudioMetaDefaults
    {
        FGuid SourceGuid;
        bool bEnableAudio = false;
        FString StreamId;
        int32 SampleRate = 0;
        int32 NumChannels = 0;

        /** Fill missing fields of Meta. Safe on any thread. */
        void Apply(O3DS::FAudioFrameMeta& Meta) const;
    };

    FAudioMetaDefaults BuildAudioMetaDefaults() const;

    /** Audio sink handed to the transport; holds a snapshot of the defaults above, never this source. */
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> MakeAudioSink() const;

    // ArrivalEpochUsOverride == 0 means "capture the arrival time now" (the top-level
    // call from the serialized consumer); a nonzero value is used when re-invoking
    // after the transport-thread -> game-thread AsyncTask hop below, so the gated
    // path's Frame.local_recv_us reflects the frame's true wire-arrival instant, not
    // whenever the game thread got around to running the dispatched task.
    // Buffer is valid only for the call (the consumer's view form, WP-A1 PR 5b); a TArray converts.
    void HandleSerializedFrame(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, uint64 ArrivalEpochUsOverride = 0);
    /**
     * Parses and publishes one packet the stream scheduler released in order: from the legacy
     * path (GatedFrame null; LegacyTimestampSeconds is the transport's timestamp) or from a
     * sender stream's reorder gate, whose tx_wallclock is mapped onto local engine time.
     */
    void ApplyReleasedFrame(O3DS::ReceiverStream& Stream, const FString& Label, const char* Data, size_t Len, double LegacyTimestampSeconds, const O3DS::Frame* GatedFrame);
    bool StartTransport();
    void StopTransport();
    /**
     * FO3DTransportRegistry::OnTransportUnregistering (ADR 0007 item 5, WP-A1 PR 2): when the
     * active transport unregisters, stop it and release the receiver and every sink given to it.
     * Subscribed only while a receiver is active. Game thread.
     */
    void HandleTransportUnregistering(FName TransportName);
    FO3DTransportConfig BuildTransportConfig() const;
    void UpdateConnectionLastActive();
    void RemoveInactiveSubjects();

    /** Sets the source GUID here and in the publisher's subject keys. */
    void SetSourceGuid(const FGuid& InSourceGuid);
    /** Test seam (WP-S4): routes the publisher's static and frame pushes to these instead of LiveLink. */
    void SetTestPushHooks(
        TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)> StaticHook,
        TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double WorldTime)> FrameHook);

    /** Drops every per-sender stream (reorder gate, clock estimator, legacy ordering,
     *  parse state) and all concealment state. Called on transport start and stop only;
     *  a legacy-path timestamp reset stays inside its own stream (RCV-34). */
    void ResetStreamState();
    /** True when frames can be published: a LiveLink client, or a test push hook. */
    bool CanPublish() const;
    O3DS::LegacyOrderingConfig GetLegacyOrderingConfig() const;
    void EnsureValidTransportName();

    /** Casts Settings (populated by InitializeSettings(), always a UO3DReceiverSourceSettings -
     *  see GetSettingsClass()) to access concealment config; null before InitializeSettings() runs. */
    const class UO3DReceiverSourceSettings* GetConcealmentSettings() const;

    bool ParseSubjectListRaw(O3DS::SubjectList& List, const FString& Subject, const char* Data, size_t Len, std::vector<O3DS::ParsedSubjectInfo>& OutTouched);

    // ── Control channel (docs/adr/0011-control-channel.md, item 8) ────────────────────
    // Everything below is game thread only. The transport's control sink (FControlSink) hops
    // here with a copy of the payload and a weak reference to this source.

    /** Effective "accept control" for this source: per-source setting, runtime override, project setting. */
    bool IsControlEnabled() const;
    /** Applies UO3DControlSettings to the core receiver (limits, allowlist). */
    void ApplyControlConfig();
    /** One control payload from the transport. Dropped (and counted) while control is disabled. */
    void HandleControlPayload(const TArray<uint8>& Payload, const FString& StreamId, double ReceiveTimeSec);
    /** Prunes, releases aligned changes, and discards state after control is turned off. */
    void TickControl(double NowS);
    /** Holds a change for alignment, or publishes it to FO3DControlBus at once. */
    void RouteControlChange(O3DS::Control::Change&& Change, double NowS);
    void PublishControlChange(const O3DS::Control::Change& Change) const;
    /** Forgets every control source this receiver has seen, without broadcasting. */
    void DiscardControlState();
    /**
     * The sender time (sender clock, microseconds) of the pose LiveLink is presenting for the
     * mocap stream of a control source, or false when that source has no live mocap stream on
     * this receiver (its changes are then released at once).
     */
    bool GetPresentedSenderTimeUs(const std::string& SourceId, uint64_t& OutUs);

    O3DS::Control::ControlReceiver ControlReceiver;
    O3DS::Control::ControlAligner ControlAligner;
    TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ActiveControlSink;
    /** Source ids this receiver has published for, so they can be forgotten on the bus. */
    TSet<FString> ControlSourcesSeen;
    bool bControlWasEnabled = false;
    uint64 ControlPayloadsDroppedDisabled = 0;
    /** A mocap stream with no packet for this long counts as not live: control from its sender is not held. */
    static constexpr double AlignmentStreamLivenessSeconds = 0.2;
    /** Publishes only the subjects the packet touched (RCV-5). Returns how many were processed. */
    int32 PublishTouchedSubjects(O3DS::SubjectList& List, const std::vector<O3DS::ParsedSubjectInfo>& Touched, double WorldTimeSecondsOverride);
    // WorldTimeSecondsOverride < 0.0 means "unset" - PushSubjectFrameData falls back to
    // FPlatformTime::Seconds() (today's behavior, used by the legacy/ungated path); the
    // gated path (A2.a/A2.c) always passes a real mapped presentation time.
    // bFullDescriptor: the packet carried a full Subject for it, which invalidates the
    // cached bone names and parents (RCV-4).
    void ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, bool bFullDescriptor);
    void FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const;

private:
    // LiveLink bookkeeping
    FText SourceType;
    FText SourceMachineName;
    FText SourceStatus;
    /** Why the last StartTransport() refused the options; Ok otherwise (WP-A1 PR 5c). */
    FO3DTransportResult LastTransportResult;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    ULiveLinkSourceSettings* Settings = nullptr;

    std::atomic<bool> bIsValid{true};

    // Throttles "Rejected malformed packet" to one line per MalformedWarningIntervalSeconds,
    // with a count of the lines suppressed since. One timestamp per source, not one per subject:
    // the subject string can come from the network (a WebRTC data label), so a map keyed by it
    // would grow without bound under hostile input. Game thread.
    static constexpr double MalformedWarningIntervalSeconds = 10.0;
    double LastMalformedWarningTime = -1.0e300;
    int32 SuppressedMalformedWarnings = 0;

    // Transport state
    FO3DReceiverSourceConfig SourceSettings;
    FO3DTransportConfig ActiveConfig;
    TSharedPtr<IOpen3DReceiver> ActiveReceiver;
    TSharedPtr<ISerializedFrameConsumer> ActiveConsumer;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> ActiveAudioSink;
    /** OnTransportUnregistering subscription; valid while ActiveReceiver is set. */
    FDelegateHandle TransportUnregisteringHandle;

    // Per-sender parse and ordering state (WP-S4, RCV-5): one stream per sender with its own
    // SubjectList, reorder gate, clock estimator and legacy ordering (WP-A3 moved it into the
    // scheduler). Always set (created by the constructor). Game thread only.
    TUniquePtr<FO3DReceiverStreamScheduler> Scheduler;

    // C1: receiver-side concealment, fed by the gated path only (WP-A3). Always set.
    TUniquePtr<FO3DReceiverConcealment> Concealment;

    // Activity tracking
    mutable FCriticalSection ConnectionLastActiveSection;
    double ConnectionLastActive = 0.0;
    static constexpr double InactivityThresholdSeconds = 5.0;
    float TimeSinceLastActivityCheck = 0.0f;
    static constexpr float ActivityCheckIntervalSeconds = 5.0f;

    // Converts parsed subjects for LiveLink and caches what only changes with the topology:
    // bone names and parents (RCV-4), curve FNames and the subject FName (RCV-12), plus
    // per-frame scratch (RCV-11). WP-A3; always set (created by the constructor).
    TUniquePtr<FO3DReceiverFrameDecoder> FrameDecoder;

    // Creates LiveLink subjects, pushes static data and frames, removes inactive subjects, and
    // holds the WP-S4 test push hooks (WP-A3). Always set (created by the constructor).
    TUniquePtr<FO3DLiveLinkPublisher> Publisher;

    bool bLoggedActiveState = false;
};
