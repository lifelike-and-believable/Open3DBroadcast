// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "ILiveLinkSource.h"
#include "Tickable.h"

#include "Transport/O3DReceiverInterface.h"
#include "O3DReceiverLogs.h"
#include "O3DBlueprintTransportTypes.h"
#include "O3DPerformanceMetrics.h"
#include "O3DRuntimeContext.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DUnifiedMessage.h"

#include <atomic>
#include <string>
#include <vector>

// The Open3DStream core (Open3DStreamCore) is a private dependency of this module (WP-A3, RCV-29):
// this header names a few core types in private member functions and includes no core header.
namespace O3DS
{
    class Subject;
    class SubjectList;
    struct Frame;
    struct LegacyOrderingConfig;
    struct ParseContext;
    struct SceneTime;
    struct ParsedSubjectInfo;
    struct ReceiverStream;
    namespace Control
    {
        struct AlignerStats;
    }
}

class ILiveLinkClient;
class FO3DReceiverFrameDecoder;
class FO3DLiveLinkPublisher;
class FO3DReceiverConcealment;
class FO3DReceiverStreamScheduler;
class FO3DReceiverControlRouter;

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
    /** False after a transport that could not start, so LiveLink shows the source as broken (RCV-16). */
    virtual bool IsSourceStillValid() const override { return Client != nullptr && bIsValid.load() && !bStartFailed; }
    virtual void Update() override;

    // Tickable interface
    virtual void Tick(float DeltaTime) override;
    /** Not after RequestSourceShutdown: a Blueprint handle can keep the source alive after LiveLink removed it. */
    virtual bool IsTickable() const override { return bIsValid.load(); }
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

    /**
     * Source status (RCV-16), game thread. A start that fails says why ("Error: ..."); a started
     * source shows "Waiting for data via X" until its first frame, "Receiving via X" while frames
     * arrive, "No data received (via X)" after StalledAfterSeconds without one, and the transport's
     * Reconnecting and Failed states (with the reason) as it reports them.
     */
    void SetStatus(const FText& Status, bool bReceiving);
    /** Records Result as the reason the transport did not start and shows it. */
    void FailStart(const FO3DTransportResult& Result);
    /** Applies the connection-state changes the transport posted since the last tick. */
    void DrainConnectionState();
    /** Shows "No data received" once no frame has arrived for StalledAfterSeconds. */
    void UpdateStalledStatus(double NowSeconds);
    /** Clears subjects idle for longer than the source's Inactive Subject Timeout (RCV-6). */
    void ClearInactiveSubjects(double NowSeconds);

    /** Sets the source GUID here and in the publisher's subject keys. */
    void SetSourceGuid(const FGuid& InSourceGuid);
    /** Test seam (WP-S4): routes the publisher's static and frame pushes to these instead of LiveLink. */
    void SetTestPushHooks(
        TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)> StaticHook,
        TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double WorldTime,
            const TOptional<FQualifiedFrameTime>& SceneTime)> FrameHook);

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

    /** Tick with the clock passed in, for tests (Tick passes FPlatformTime::Seconds()). */
    void TickAt(float DeltaTime, double NowSeconds);

    bool ParseSubjectListRaw(O3DS::SubjectList& List, const FString& Subject, const char* Data, size_t Len, std::vector<O3DS::ParsedSubjectInfo>& OutTouched,
        const O3DS::ParseContext* Context = nullptr);

    // ── Control channel (docs/adr/0011-control-channel.md, item 8) ────────────────────
    // Everything below is game thread only. The transport's control sink (FControlSink) hops
    // here with a copy of the payload and a weak reference to this source.

    /** Effective "accept control" for this source: per-source setting, runtime override, project setting. */
    bool IsControlEnabled() const;
    /** One control payload from the transport, handed to the control router. */
    void HandleControlPayload(const TArray<uint8>& Payload, const FString& StreamId, double ReceiveTimeSec);
    /**
     * The sender time (sender clock, microseconds) of the pose LiveLink presents for the mocap
     * stream carrying these subjects, or false when there is no live one on this receiver.
     */
    bool GetPresentedSenderTimeUs(const std::vector<std::string>& MocapSubjects, uint64_t& OutUs);
    // Read by the test accessor.
    uint64 GetControlPayloadsDroppedDisabled() const;
    const O3DS::Control::AlignerStats& GetControlAlignerStats() const;
    size_t GetNumHeldControlChanges() const;

    TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ActiveControlSink;
    /** Control parsing, alignment and publishing (WP-A3). Always set (created by the constructor). */
    TUniquePtr<FO3DReceiverControlRouter> ControlRouter;
    /** A mocap stream with no packet for this long counts as not live: control from its sender is not held. */
    static constexpr double AlignmentStreamLivenessSeconds = 0.2;
    /** Publishes only the subjects the packet touched (RCV-5). Returns how many were processed. */
    int32 PublishTouchedSubjects(O3DS::SubjectList& List, const std::vector<O3DS::ParsedSubjectInfo>& Touched, double WorldTimeSecondsOverride);
    // WorldTimeSecondsOverride < 0.0 means "unset" - PushSubjectFrameData falls back to
    // FPlatformTime::Seconds() (today's behavior, used by the legacy/ungated path); the
    // gated path (A2.a/A2.c) always passes a real mapped presentation time.
    // bFullDescriptor: the packet carried a full Subject for it, which invalidates the
    // cached bone names and parents (RCV-4).
    // SenderSceneTime: the frame's SubjectList.scene_time, or null (RCV-8, ADR 0013).
    void ProcessParsedSubject(O3DS::Subject* SubjectPtr, double WorldTimeSecondsOverride, bool bFullDescriptor, const O3DS::SceneTime* SenderSceneTime);
    void FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const;

public:
    /** This source's metrics handle (ADR 0012 item 4). Any thread. */
    const FO3DReceiverMetricsHandleRef& GetMetricsHandle() const { return MetricsHandle; }

    /** This source's runtime context (ADR 0012 item 5). Any thread. */
    const FO3DRuntimeContextRef& GetContext() const { return RuntimeContext; }

private:
    // LiveLink bookkeeping
    FText SourceType;
    FText SourceMachineName;
    FText SourceStatus;
    /** Why the last StartTransport() refused the options; Ok otherwise (WP-A1 PR 5c). */
    FO3DTransportResult LastTransportResult;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    /** Owned by LiveLink's entry for this source, which can go while the source lives on (WP-R1). */
    TWeakObjectPtr<ULiveLinkSourceSettings> Settings;

    std::atomic<bool> bIsValid{true};
    /** The last start failed (RCV-16); IsSourceStillValid is false until a start succeeds. */
    std::atomic<bool> bStartFailed{false};
    /** The status currently says "Receiving via X". */
    bool bStatusIsReceiving = false;
    /** The status says the last packet could not be read (RR-3); set once, not per packet. */
    bool bStatusIsUnreadable = false;
    static constexpr double StalledAfterSeconds = 2.0;
    /** The transport's connection-state changes, posted from any thread, drained in Tick (RCV-16). */
    TSharedRef<FO3DConnectionStateMailbox, ESPMode::ThreadSafe> StateMailbox = MakeShared<FO3DConnectionStateMailbox, ESPMode::ThreadSafe>();

    // The runtime context named by the settings' ContextName (ADR 0012 item 5): this source's
    // audio and control go to its buses. Declared before everything that refers into it, so it is
    // created first and destroyed last.
    FO3DRuntimeContextRef RuntimeContext;

    // This source's metrics (ADR 0012 item 4), from RuntimeContext. Counters also add to the context's
    // aggregate. Handed to the decoder, scheduler and concealment, never the context.
    FO3DReceiverMetricsHandleRef MetricsHandle;

    // Throttles "Rejected malformed packet" to one line per MalformedWarningIntervalSeconds,
    // with a count of the lines suppressed since. One timestamp per source, not one per subject:
    // the subject string can come from the network (a WebRTC data label), so a map keyed by it
    // would grow without bound under hostile input. Game thread.
    static constexpr double MalformedWarningIntervalSeconds = 10.0;
    double LastMalformedWarningTime = -1.0e300;
    int32 SuppressedMalformedWarnings = 0;
    // The same for "Parse failed": a packet that passes the check but not the full parse (WP-R1).
    double LastParseFailedWarningTime = -1.0e300;
    int32 SuppressedParseFailedWarnings = 0;

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
    float TimeSinceLastActivityCheck = 0.0f;
    static constexpr float ActivityCheckIntervalSeconds = 1.0f;

    // Converts parsed subjects for LiveLink and caches what only changes with the topology:
    // bone names and parents (RCV-4), curve FNames and the subject FName (RCV-12), plus
    // per-frame scratch (RCV-11). WP-A3; always set (created by the constructor).
    TUniquePtr<FO3DReceiverFrameDecoder> FrameDecoder;

    // Creates LiveLink subjects, pushes static data and frames, removes inactive subjects, and
    // holds the WP-S4 test push hooks (WP-A3). Always set (created by the constructor).
    TUniquePtr<FO3DLiveLinkPublisher> Publisher;

};
