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

#include <atomic>

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
    friend struct FO3DReceiverSourceTestAccessor;

    class FSerializedConsumer;
    class FAudioSink;

    // ArrivalEpochUsOverride == 0 means "capture the arrival time now" (the top-level
    // call from the serialized consumer); a nonzero value is used when re-invoking
    // after the transport-thread -> game-thread AsyncTask hop below, so the gated
    // path's Frame.local_recv_us reflects the frame's true wire-arrival instant, not
    // whenever the game thread got around to running the dispatched task.
    void HandleSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds, uint64 ArrivalEpochUsOverride = 0);
    void HandleLegacyFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds);
    void EmitGatedFrame(O3DS::Frame&& Frame);
    void ReportGateMetricsDelta();
    bool StartTransport();
    void StopTransport();
    FO3DTransportConfig BuildTransportConfig() const;
    void UpdateConnectionLastActive();
    void RemoveInactiveSubjects();

    void PushSubjectStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, uint64 DescriptorHash);
    void PushSubjectFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues, double TimestampSeconds, double WorldTimeSecondsOverride, uint64 CurveHash);

    void ResetOrderingState();
    void EnsureValidTransportName();

    bool ParseSubjectListBuffer(const FString& Subject, const TArray<uint8>& Buffer);
    bool ParseSubjectListRaw(const FString& Subject, const char* Data, size_t Len);
    bool ShouldProcessFrame(double SubjectListTime, double NowSeconds);
    bool ShouldResetOrderingWindow(double NowSeconds, double SubjectListTime) const;
    double GetLastConnectionActive() const;
    bool BuildSubjectPose(O3DS::Subject* SubjectPtr, TArray<FName>& OutBoneNames, TArray<int32>& OutBoneParents, TArray<FTransform>& OutBoneTransforms) const;
    void BuildSubjectCurves(O3DS::Subject* SubjectPtr, TArray<FName>& OutCurveNames, TArray<float>& OutCurveValues) const;
    // WorldTimeSecondsOverride < 0.0 means "unset" - PushSubjectFrameData falls back to
    // FPlatformTime::Seconds() (today's behavior, used by the legacy/ungated path); the
    // gated path (A2.a/A2.c) always passes a real mapped presentation time.
    void ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, double WorldTimeSecondsOverride, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues);
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

    // Frame parsing helpers
    O3DS::SubjectList SubjectScratch;

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
    FName LastObservedSubjectName;

    // PHASE 4 OPTIMIZATION: Per-subject bone structure cache to avoid re-building identical skeletons
    // When skeleton structure doesn't change (same parent IDs and count), we can reuse
    // the cached bone names and parents instead of re-parsing each frame.
    // Expected impact: Skips most of BuildSubjectPose() for ~99% of frames (only rebuilds on skeleton change)
    struct FSubjectTransformCache
    {
        TArray<FName> BoneNames;
        TArray<int32> BoneParents;
        uint64 SkeletonFingerprint = 0;  // Quick check: transform count + parent ID hash
    };
    TMap<FName, FSubjectTransformCache> SubjectTransformCaches;

    // Timestamp ordering (legacy path, used when a sender doesn't set tx_seq)
    double LastAppliedSubjectListTime = -1.0;
    uint64 FrameCounter = 0;
    bool bLoggedActiveState = false;

    // A2.a/A2.b: reorder/dedup/stale-drop gate + clock-offset estimator for senders
    // that do set tx_seq. One gate per receiver source (not per multiplexed subject) -
    // see reorder_gate.h's own doc comment on the multi-sender caveat this doesn't
    // yet handle. Both are reset alongside LastAppliedSubjectListTime in
    // ResetOrderingState() so a transport restart starts a clean session.
    O3DS::ReorderGate ReceiverGate;
    O3DS::ClockOffsetEstimator ClockEstimator;
    O3DS::ReorderStats PrevGateStats; // last-reported snapshot, for delta metrics reporting
    FString LastGateSubjectLabel;     // diagnostic-only subject label for the gate's emit path
};
