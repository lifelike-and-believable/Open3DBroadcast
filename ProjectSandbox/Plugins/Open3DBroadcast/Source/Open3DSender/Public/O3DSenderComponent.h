// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSenderSerializer.h"
#include "O3DSenderInterface.h"
#include "O3DSenderLogs.h"
#include "O3DTransportTypes.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "Templates/UniquePtr.h"
#include "Templates/Function.h"
#include "O3DSenderComponent.generated.h"

class USkeletalMeshComponent;
class USkeleton;
class USkeletalMesh;
class USoundSubmix;
class FO3DSenderTransportController;
class FO3DSenderCurveProcessor;
struct FO3DSenderCurveConfig;

struct FO3DSenderTransportControllerDeleter
{
	void operator()(FO3DSenderTransportController* Ptr) const;
};

struct FO3DSenderCurveProcessorDeleter
{
	void operator()(FO3DSenderCurveProcessor* Ptr) const;
};

USTRUCT()
struct OPEN3DSENDER_API FO3DSSkeletonDescriptor
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FName> BoneNames;

	UPROPERTY()
	TArray<int32> ParentIndices;

	UPROPERTY()
	uint64 Hash = 0;

	void Reset()
	{
		BoneNames.Reset();
		ParentIndices.Reset();
		Hash = 0;
	}

	bool IsValid() const { return BoneNames.Num() > 0 && BoneNames.Num() == ParentIndices.Num(); }
};

USTRUCT()
struct OPEN3DSENDER_API FO3DSPoseFrame
{
	GENERATED_BODY()

	UPROPERTY()
	FString Subject;

	UPROPERTY()
	uint64 FrameIndex = 0;

	UPROPERTY()
	TArray<FTransform> BoneLocalTransforms;

	UPROPERTY()
	TArray<FName> CurveNames;

	UPROPERTY()
	TArray<float> CurveValues;

	void Reset()
	{
		Subject.Reset();
		FrameIndex = 0;
		BoneLocalTransforms.Reset();
		CurveNames.Reset();
		CurveValues.Reset();
	}
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DDescriptorReady, const FString& /*Subject*/, const FO3DSSkeletonDescriptor& /*Descriptor*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DPoseFrameReady, const FString& /*Subject*/, const FO3DSPoseFrame& /*Frame*/);

UCLASS(ClassGroup = (Open3DStream), meta = (BlueprintSpawnableComponent))
class OPEN3DSENDER_API UO3DSenderComponent : public UActorComponent
{
	GENERATED_BODY()

	friend class FO3DSenderTransportController;

public:
	UO3DSenderComponent();
	virtual ~UO3DSenderComponent();

	UFUNCTION(BlueprintCallable, meta = (CallInEditor), Category = "Open3DStream|Sender")
	void StartCapture();

	UFUNCTION(BlueprintCallable, meta = (CallInEditor), Category = "Open3DStream|Sender")
	void StopCapture();

	UFUNCTION(BlueprintPure, Category = "Open3DStream|Sender")
	bool IsCapturing() const { return bIsCapturing; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender")
	TWeakObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender", meta = (DisplayName = "Subject Name"))
	FString SubjectName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender")
	float CaptureRateHz = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender")
	bool bAutoStartCapture = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Transport")
	bool bAutoCreateTransport = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Transport", meta = (HideInDetailPanel))
	FName TransportName = TEXT("loopback");

	/** Transport-provided key/value overrides populated by modular transport UIs. Hidden from the generic details panel. */
	UPROPERTY(VisibleAnywhere, Category = "Open3DStream|Sender|Transport", meta = (HideInDetailPanel))
	TMap<FString, FString> TransportOptions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Audio")
	bool bEnableAudio = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Audio", meta = (EditCondition = "bEnableAudio"))
	EO3DSenderCaptureMode AudioCaptureMode = EO3DSenderCaptureMode::Mix;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Audio", meta = (GetOptions = "GetAvailableAudioInputDeviceOptions", EditCondition = "bEnableAudio && AudioCaptureMode == EO3DSenderCaptureMode::Input", EditConditionHides))
	FName AudioInputDevice;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Audio", meta = (EditCondition = "bEnableAudio", ShowOnlyInnerProperties))
	FO3DSenderAudioCaptureConfig AudioCaptureConfig;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Audio", meta = (EditCondition = "bEnableAudio"))
	FString AudioStreamLabel = TEXT("o3ds:audio");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves")
	bool bClampMorphCurvesToUnit = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves")
	bool bDropNaNAndInfinity = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering")
	bool bEnableCurveFiltering = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering", ClampMin = "0.0"))
	float CurveEpsilon = 0.0005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering", ClampMin = "0.0"))
	float CurveDeltaThreshold = 0.001f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering"))
	TArray<FString> IncludeCurvePatterns;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering"))
	TArray<FString> ExcludeCurvePatterns;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Sender|Curves|Filtering")
	bool bLogFilteredCurves = false;

	FOnO3DDescriptorReady OnDescriptorReady;
	FOnO3DPoseFrameReady OnPoseFrameReady;
	FOnO3DSerializedFrame OnSerializedFrame;

	FO3DSenderSerializer& GetSerializer() const { return *Serializer; }

	FName GetTransportName() const { return TransportName; }
	void SetTransportName(FName InName);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
#if WITH_EDITORONLY_DATA
	virtual void PostLoad() override;
#endif
	virtual void OnRegister() override;
	virtual void PostInitProperties() override;

private:
	void BindToTarget();
	void UnbindFromTarget();
	void HandleBoneTransformsFinalized();
	void NotifyOnScreen(const FString& Message, const FColor& Color = FColor::Green, float DisplayTime = 2.0f) const;

	void EnsureSkeletonCache(USkeletalMeshComponent* SkelComp);
	void RefreshSkeletonCache(USkeletalMeshComponent* SkelComp);
	FString BuildSubjectName(const USkeletalMeshComponent* SkelComp) const;
	FString SanitizeSubjectName(const FString& Raw) const;
	FO3DSenderCurveConfig BuildCurveConfig() const;

	uint64 ComputeDescriptorHash(const TArray<FName>& InNames, const TArray<int32>& InParents) const;

	TArray<FName> BoneNames;
	TArray<int32> ParentIndices;
	TWeakObjectPtr<USkeleton> CachedSkeleton;
	TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;

	FO3DSSkeletonDescriptor DescriptorCache;
	bool bDescriptorDirty = false;

	bool bIsCapturing = false;
	double LastCaptureTime = 0.0;
	uint64 FrameCounter = 0;

	TUniquePtr<FO3DSenderSerializer> Serializer;

	FDelegateHandle BoneTransformsFinalizedHandle;
	FDelegateHandle SerializerRelayHandle;
	FDelegateHandle SubjectListHandle;

	TUniquePtr<FO3DSenderTransportController, FO3DSenderTransportControllerDeleter> TransportController;
	TUniquePtr<FO3DSenderCurveProcessor, FO3DSenderCurveProcessorDeleter> CurveProcessor;
	UPROPERTY(Transient)
	UO3DSenderAudioCaptureComponent* AudioCaptureComponent = nullptr;
	double LastAudioSinkWarningTime = 0.0;

	void UpdateEditConditionHelpers();
	void TeardownTransport();
	void InitializeTransport();
	FO3DTransportConfig BuildTransportConfig() const;
	void HandleSerializedFrameForward(const FString& Subject, const TArray<uint8>& Buffer, double Timestamp);
	void OnSubjectListReady(const FString& Subject, const TSharedPtr<O3DS::SubjectList>& Payload);
	void UpdateAudioCaptureBinding();

public:
	/** Retrieve a transport option by key (case-sensitive). Returns empty string if missing. */
	FString GetTransportOption(const FString& Key) const;

	/** Set or update a transport option. Passing an empty value removes the key. */
	void SetTransportOption(const FString& Key, const FString& Value);

	/** Remove all transport options (used when switching transports). */
	void ClearTransportOptions();

	UFUNCTION(BlueprintCallable, Category = "Open3DStream|Sender|Audio")
	TArray<FName> GetAvailableAudioInputDeviceOptions() const;

private:
	bool CanCaptureThisFrame(double NowSeconds, USkeletalMeshComponent*& OutMesh);
	FString ResolveSubjectName(const USkeletalMeshComponent* SkelComp) const;
	FO3DSPoseFrame CreateFrameShell(const USkeletalMeshComponent* SkelComp);
	void PopulatePoseFrameBones(const USkeletalMeshComponent* SkelComp, FO3DSPoseFrame& Frame, bool bDebugPose);
	void PopulatePoseFrameCurves(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& CurveConfig, FO3DSPoseFrame& Frame, bool bDebugCurves);
	FO3DSenderAudioCaptureConfig BuildAudioCaptureConfig() const;
	FO3DTransportAudioConfig BuildTransportAudioConfig(const FO3DSenderAudioCaptureConfig& CaptureConfig) const;
	void EnsureAudioCaptureComponent();
	void ConfigureAudioCaptureComponent(const FO3DSenderAudioCaptureConfig& CaptureConfig, const FO3DTransportAudioConfig& TransportAudioConfig);
	void TeardownAudioCapture();
	void SyncAudioConfigSource();
	int32 ResolveAudioDeviceIndex(const FName& DeviceName) const;

	static bool ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz);
	static void BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
		const TArray<int32>& CachedParentIndices,
		int32 NumBones,
		TFunctionRef<int32(int32)> ResolveFallbackParent,
		TArray<FTransform>& OutLocalTransforms,
		TArray<int32>* OutResolvedParents);

#if WITH_AUTOMATION_TESTS
	friend struct FO3DSenderComponentTestHelper;
#endif

	void EnsureValidTransportName();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
