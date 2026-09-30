// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "Tickable.h"

#include "O3DReceiverInterface.h"
#include "O3DReceiverLogs.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DUnifiedMessage.h"

#include "o3ds/model.h"
#include "o3ds/reorder_gate.h"
#include "o3ds/clock_offset.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/predict/concealment.h"

#include <atomic>
#include <vector>

class ILiveLinkClient;

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
    void HandleSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds, uint64 ArrivalEpochUsOverride = 0);
    void HandleLegacyFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds, const O3DS::PacketMeta& Meta, O3DS::ReceiverStream& Stream);
    void EmitGatedFrame(uint64 StreamKey, O3DS::Frame&& Frame);
    void ReportGateMetricsDelta();
    bool StartTransport();
    void StopTransport();
    FO3DTransportConfig BuildTransportConfig() const;
    void UpdateConnectionLastActive();
    void RemoveInactiveSubjects();

    // bFirstPushThisSession: the subject is not in InitializedSubjects yet. Only then
    // may the LiveLink subject be created (RCV-7); later static changes re-push
    // static data only, so per-subject settings are never replaced.
    void PushSubjectStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, uint64 DescriptorHash, bool bFirstPushThisSession);
    void PushSubjectFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues, double TimestampSeconds, double WorldTimeSecondsOverride, uint64 CurveHash);

    /** Drops every per-sender stream (reorder gate, clock estimator, legacy ordering,
     *  parse state) and all concealment state. Called on transport start and stop only;
     *  a legacy-path timestamp reset stays inside its own stream (RCV-34). */
    void ResetStreamState();
    /** True when frames can be published: a LiveLink client, or a test push hook. */
    bool CanPublish() const;
    O3DS::LegacyOrderingConfig GetLegacyOrderingConfig() const;
    void EnsureValidTransportName();

    // C1: receiver-side concealment (roadmap doc §5/C1). One ConcealmentEngine
    // per subject, matching the same per-subject-map convention as
    // SubjectTransformCaches etc. Only active for the A2-gated path (a real
    // sender-clock-mapped presentation time is required - see
    // ObserveConcealmentRealFrame's WorldTimeSecondsOverride guard); the
    // legacy/ungated path has no reliable clock domain to reason about gaps
    // in, so it's left exactly as it behaves today (freeze-on-loss).
    O3DS::ConcealmentEngine& GetOrCreateSubjectConcealment(FName SubjectName);
    void ObserveConcealmentRealFrame(FName SubjectName, double PresentationTimeSeconds, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, bool bTopologyChanged);
    void TickConcealment();
    /** Casts Settings (populated by InitializeSettings(), always a UO3DReceiverSourceSettings -
     *  see GetSettingsClass()) to access concealment config; null before InitializeSettings() runs. */
    const class UO3DReceiverSourceSettings* GetConcealmentSettings() const;
    void ReportConcealmentMetricsDelta();

    bool ParseSubjectListRaw(O3DS::SubjectList& List, const FString& Subject, const char* Data, size_t Len, std::vector<O3DS::ParsedSubjectInfo>& OutTouched);
    /** Publishes only the subjects the packet touched (RCV-5). Returns how many were processed. */
    int32 PublishTouchedSubjects(O3DS::SubjectList& List, const std::vector<O3DS::ParsedSubjectInfo>& Touched, double WorldTimeSecondsOverride);
    bool BuildSubjectPose(O3DS::Subject* SubjectPtr, TArray<FName>& OutBoneNames, TArray<int32>& OutBoneParents, TArray<FTransform>& OutBoneTransforms) const;
    void BuildSubjectCurves(O3DS::Subject* SubjectPtr, TArray<FName>& OutCurveNames, TArray<float>& OutCurveValues) const;
    // WorldTimeSecondsOverride < 0.0 means "unset" - PushSubjectFrameData falls back to
    // FPlatformTime::Seconds() (today's behavior, used by the legacy/ungated path); the
    // gated path (A2.a/A2.c) always passes a real mapped presentation time.
    // bFullDescriptor: the packet carried a full Subject for it, which invalidates the
    // cached bone names and parents (RCV-4).
    void ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, bool bFullDescriptor, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues);
    void FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const;

private:
    // LiveLink bookkeeping
    FText SourceType;
    FText SourceMachineName;
    FText SourceStatus;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;
    ULiveLinkSourceSettings* Settings = nullptr;
    TSet<FName> InitializedSubjects;

    std::atomic<bool> bIsValid{true};

    // Transport state
    FO3DReceiverSourceConfig SourceSettings;
    FO3DTransportConfig ActiveConfig;
    TSharedPtr<IOpen3DReceiver> ActiveReceiver;
    TSharedPtr<ISerializedFrameConsumer> ActiveConsumer;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> ActiveAudioSink;

    // Per-sender parse and ordering state (WP-S4, RCV-5). Several senders can share
    // one transport channel; each gets its own SubjectList, ReorderGate, clock
    // estimator and legacy ordering, found by the subject names its packets carry
    // (see O3DS::ReceiverStreamTable). Game thread only, like the gate it holds.
    O3DS::ReceiverStreamTable Streams;

    // Activity tracking
    mutable FCriticalSection ConnectionLastActiveSection;
    double ConnectionLastActive = 0.0;
    TMap<FName, double> SubjectLastUpdateTime;
    static constexpr double InactivityThresholdSeconds = 5.0;
    float TimeSinceLastActivityCheck = 0.0f;
    static constexpr float ActivityCheckIntervalSeconds = 5.0f;

    // Descriptor caches
    TMap<FName, uint64> SubjectSkeletonHashes;
    TMap<FName, uint64> SubjectCurveHashes;

    // Per-subject bone structure cache, so FName construction for bone names only runs
    // when the skeleton changes. Reused only while the fingerprint (bone count, names
    // and parent ids, O3DS::SkeletonFingerprint) is unchanged and the packet did not
    // carry a full descriptor for the subject (RCV-4).
    struct FSubjectTransformCache
    {
        TArray<FName> BoneNames;
        TArray<int32> BoneParents;
        uint64 SkeletonFingerprint = 0;
    };
    TMap<FName, FSubjectTransformCache> SubjectTransformCaches;

    uint64 FrameCounter = 0;
    bool bLoggedActiveState = false;

    FString LastGateSubjectLabel;     // diagnostic-only subject label for the gate's emit path

    // C1: cached from the most recent stream clock Observe() (in
    // EmitGatedFrame). Real frames' PresentationTimeSeconds is already
    // MappedWorldTimeSeconds - a local-FPlatformTime::Seconds()-domain value,
    // since EmitGatedFrame converts mapped_presentation_time_us via a
    // NowEpochUs/NowPlatformS anchor taken at that same instant. So
    // TickConcealment()'s "now" is just a fresh FPlatformTime::Seconds()
    // reading, with no offset added - LastClockOffsetEstimateUs itself is
    // only used as bHasClockOffsetEstimate's payload (i.e. "has the gated
    // path observed at least one real frame"), not as a time-base correction.
    int64 LastClockOffsetEstimateUs = 0;
    bool bHasClockOffsetEstimate = false;

    TMap<FName, TUniquePtr<O3DS::ConcealmentEngine>> SubjectConcealment;
    TMap<FName, O3DS::ConcealmentMetrics> PrevConcealmentMetricsBySubject; // last-reported snapshot per subject, for delta metrics reporting

    // Test seam (WP-S4): when bound, static and frame pushes go to these instead of
    // the LiveLink client, and CanPublish() is true without a client. Unbound in
    // production. Bound only by FO3DReceiverCorrectnessTestAccessor.
    TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)> TestStaticPushHook;
    TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double WorldTime)> TestFramePushHook;
};
