// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "Tickable.h"

#include "O3DReceiverInterface.h"
#include "O3DReceiverLogs.h"
#include "O3DReceiverSourceSettings.h"

#include "o3ds/model.h"

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
    class FSerializedConsumer;

    void HandleSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds);
    bool StartTransport();
    void StopTransport();
    FO3DTransportConfig BuildTransportConfig() const;
    void UpdateConnectionLastActive();
    void RemoveInactiveSubjects();

    void PushSubjectStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, uint64 DescriptorHash);
    void PushSubjectFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues, double TimestampSeconds, uint64 CurveHash);

    void ResetOrderingState();
    void EnsureValidTransportName();

    bool ParseSubjectListBuffer(const FString& Subject, const TArray<uint8>& Buffer);
    bool ShouldProcessFrame(double SubjectListTime, double NowSeconds);
    bool ShouldResetOrderingWindow(double NowSeconds, double SubjectListTime) const;
    double GetLastConnectionActive() const;
    bool BuildSubjectPose(O3DS::Subject* SubjectPtr, TArray<FName>& OutBoneNames, TArray<int32>& OutBoneParents, TArray<FTransform>& OutBoneTransforms) const;
    void BuildSubjectCurves(O3DS::Subject* SubjectPtr, TArray<FName>& OutCurveNames, TArray<float>& OutCurveValues) const;
    void ProcessParsedSubject(O3DS::Subject* SubjectPtr, double SubjectListTime, TArray<FName>& BoneNames, TArray<int32>& BoneParents, TArray<FTransform>& BoneTransforms, TArray<FName>& CurveNames, TArray<float>& CurveValues);

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

    // Timestamp ordering
    double LastAppliedSubjectListTime = -1.0;
    uint64 FrameCounter = 0;
    bool bLoggedActiveState = false;
};
