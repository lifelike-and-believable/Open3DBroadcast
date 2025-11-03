// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSBroadcastSerializer.h"
#include "O3DSBroadcastTransportAdapter.h" // for transport enums
#include "O3DSBroadcastComponent.generated.h"

class USkeletalMeshComponent;
class USkeleton;
class USkeletalMesh;
class USoundSubmix;
class FO3DSBroadcastSerializer; // forward decl

class IBroadcastTransport; // fwd

UENUM()
enum class EO3DSWebRTCAudioMode : uint8
{
 Mix,
 Input
};

// Sender-side WebRTC backend selection (mirrors shared enum; kept local for reflection/UI)
UENUM(BlueprintType)
enum class EO3DSWebRtcBackendSender : uint8
{
	LibDataChannel UMETA(DisplayName="Peer-to-Peer (libdatachannel)"),
	LiveKit       UMETA(DisplayName="LiveKit SFU")
};

// Descriptor capturing a skeleton's stable description
USTRUCT()
struct OPEN3DBROADCAST_API FO3DSSkeletonDescriptor
{
 GENERATED_BODY()

 UPROPERTY()
 TArray<FName> BoneNames;

 UPROPERTY()
 TArray<int32> ParentIndices;

 // Simple hash to detect changes quickly
 UPROPERTY()
 uint64 Hash =0;

 void Reset()
 {
 BoneNames.Reset();
 ParentIndices.Reset();
 Hash =0;
 }

 bool IsValid() const { return BoneNames.Num() >0 && BoneNames.Num() == ParentIndices.Num(); }
};

// Per-frame pose payload (parent-relative transforms in skeleton order)
USTRUCT()
struct OPEN3DBROADCAST_API FO3DSPoseFrame
{
 GENERATED_BODY()

 // Subject this frame applies to (sanitized)
 UPROPERTY()
 FString Subject;

 // Frame counter for debugging/ordering
 UPROPERTY()
 uint64 FrameIndex =0;

 // Parent-relative transforms, same order as FO3DSSkeletonDescriptor.BoneNames
 UPROPERTY()
 TArray<FTransform> BoneLocalTransforms;

 // Optional curve names/values aligned arrays
 UPROPERTY()
 TArray<FName> CurveNames;

 UPROPERTY()
 TArray<float> CurveValues;

 void Reset()
 {
 Subject.Reset();
 FrameIndex =0;
 BoneLocalTransforms.Reset();
 CurveNames.Reset();
 CurveValues.Reset();
 }
};

// Delegates to publish descriptor and frame events
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DSDescriptorReady, const FString& /*Subject*/, const FO3DSSkeletonDescriptor& /*Descriptor*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DSPoseFrameReady, const FString& /*Subject*/, const FO3DSPoseFrame& /*Frame*/);

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSBroadcastComponent : public UActorComponent
{
 GENERATED_BODY()

public:
 UO3DSBroadcastComponent();

 // Start/Stop capture controls (C++)
 UFUNCTION(BlueprintCallable, meta=(CallInEditor), Category="Open3DStream|Broadcast")
 void StartCapture();

 UFUNCTION(BlueprintCallable, meta=(CallInEditor), Category="Open3DStream|Broadcast")
 void StopCapture();

 UFUNCTION(BlueprintPure, Category="Open3DStream|Broadcast")
 bool IsCapturing() const { return bIsCapturing; }

 // Target skeletal mesh component to capture; if not set, auto-discovers one on owner at BeginPlay
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast")
 TWeakObjectPtr<USkeletalMeshComponent> TargetMesh;

 // Optional explicit subject name to use for broadcasting. If empty, a synthesized name is used.
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast", meta=(DisplayName="Subject Name"))
 FString SubjectName;

 // Optional capture rate limit (Hz). <=0 means capture every evaluation.
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast")
 float CaptureRateHz =60.0f;

 // Start capture automatically on BeginPlay (helpful for quick editor testing)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast")
 bool bAutoStartCapture = true;

 // Built-in transport (optional): enable to auto-send serialized frames without adding a second component
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
 bool bAutoCreateTransport = false;

 // Legacy transport (hidden/deprecated)
 UPROPERTY(meta=(DisplayName="Transport (Deprecated)", DeprecatedProperty, DeprecationMessage="Use Transport Family + Mode instead", EditCondition="false", EditConditionHides), EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
 EO3DSTransportKind Transport = EO3DSTransportKind::Disabled;

 // New Transport Family UX (preferred)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bAutoCreateTransport"))
 EO3DSTransportFamily TransportFamily = EO3DSTransportFamily::TCP;

 // Mode selection per family (conditional visibility)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bShowNngProps", EditConditionHides))
 EO3DSNngMode NngMode = EO3DSNngMode::Publisher;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bShowTcpProps", EditConditionHides))
 EO3DSTcpMode TcpMode = EO3DSTcpMode::Client;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bShowWebRtcProps", EditConditionHides))
 EO3DSWebRtcMode WebRtcMode = EO3DSWebRtcMode::Client;

 // Backend selection (controls connector backend and LiveKit UI visibility)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(EditCondition="bShowWebRtcProps", EditConditionHides))
 EO3DSWebRtcBackendSender WebRtcBackend = EO3DSWebRtcBackendSender::LibDataChannel;

 // Common WebRTC room across backends (shown under WebRTC heading)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(EditCondition="bShowWebRtcProps", EditConditionHides))
 FString WebRtcRoom = TEXT("room1");

 // LiveKit-specific configuration (only token kept; server URL uses Url)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|LiveKit", meta=(EditCondition="bShowLiveKitProps", EditConditionHides))
 FString LiveKitToken;

 // Endpoint and key (new names)
 // TODO: Consider splitting Address and Port into separate fields (e.g., FString Address + int32 Port) and deriving Url at runtime to avoid formatting mistakes.
 // The component currently auto-corrects the common dot-vs-colon typo for tcp URLs (e.g., tcp://0.0.0.0.9000 -> tcp://0.0.0.0:9000).
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bAutoCreateTransport", ToolTip="Endpoint URL (e.g., tcp://127.0.0.1:9000). Note: we may split Address and Port in future to avoid formatting mistakes. The component auto-corrects tcp://host.port to tcp://host:port."))
 FString Url = TEXT("tcp://127.0.0.1:9000");

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="false", EditConditionHides))
 FString Key;

 // Queue size for backpressure (bytes) (new name)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="bAutoCreateTransport"))
 int32 MaxQueuedBytes =8 *1024 *1024;

 // Legacy fields (hidden/deprecated) kept to avoid breaking saved assets
 UPROPERTY(meta=(DeprecatedProperty, DeprecationMessage="Use Url", EditCondition="false", EditConditionHides), EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
 FString TransportUrl = TEXT("tcp://127.0.0.1:9000");

 UPROPERTY(meta=(DeprecatedProperty, DeprecationMessage="Use Key", EditCondition="false", EditConditionHides), EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
 FString TransportKey;

 UPROPERTY(meta=(DeprecatedProperty, DeprecationMessage="Use MaxQueuedBytes", EditCondition="false", EditConditionHides), EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
 int32 TransportMaxQueuedBytes =8 *1024 *1024;

 // WebRTC audio track settings (feature-gated; sender side)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(EditCondition="bShowWebRtcProps", EditConditionHides))
 bool bEnableWebRTCAudio = false;

 // Mode: Mix or Input
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(EditCondition="bShowWebRtcAudioProps", EditConditionHides))
 EO3DSWebRTCAudioMode WebRTCAudioMode = EO3DSWebRTCAudioMode::Mix;

 // Optional input device name (dropdown) when in Input mode
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(GetOptions="GetAvailableInputDeviceOptions", EditCondition="bShowWebRtcAudioProps && WebRTCAudioMode == EO3DSWebRTCAudioMode::Input", EditConditionHides))
 FName WebRTCInputDeviceName;

 // Optional custom submix when in Mix mode (defaults to main submix if null)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(EditCondition="bShowWebRtcAudioProps && WebRTCAudioMode == EO3DSWebRTCAudioMode::Mix", EditConditionHides))
 USoundSubmix* WebRTCSubmixToTap = nullptr;

 // Capture/encode parameters (targets; actual may be adjusted by stack)
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(ClampMin="8000", ClampMax="48000", EditCondition="bShowWebRtcAudioProps", EditConditionHides))
 int32 WebRTCAudioSampleRate =48000;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(ClampMin="1", ClampMax="2", EditCondition="bShowWebRtcAudioProps", EditConditionHides))
 int32 WebRTCAudioNumChannels =1;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(ClampMin="6", ClampMax="128", EditCondition="bShowWebRtcAudioProps", EditConditionHides))
 int32 WebRTCAudioBitrateKbps =32;

 // Extra buffering on receiver to trade latency for resilience
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|WebRTC", meta=(ClampMin="0", ClampMax="500", EditCondition="bShowWebRtcAudioProps", EditConditionHides))
 int32 WebRTCAudioPlayoutDelayMs =0;

 // Curve normalization and filtering configuration
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves")
 bool bClampMorphCurvesToUnit = true;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves")
 bool bDropNaNAndInfinity = true;

 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering")
 bool bEnableCurveFiltering = false;

 // Drop very small values: |v| < Epsilon
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering", meta=(EditCondition="bEnableCurveFiltering", ClampMin="0.0"))
 float CurveEpsilon =0.0005f;

 // Send only if |v - lastSent| >= DeltaThreshold
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering", meta=(EditCondition="bEnableCurveFiltering", ClampMin="0.0"))
 float CurveDeltaThreshold =0.001f;

 // If non-empty, only curves matching at least one include pattern will be considered
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering", meta=(EditCondition="bEnableCurveFiltering"))
 TArray<FString> IncludeCurvePatterns;

 // Curves matching any exclude pattern will be dropped
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering", meta=(EditCondition="bEnableCurveFiltering"))
 TArray<FString> ExcludeCurvePatterns;

 // Log filtered/dropped curves for debugging
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Curves|Filtering")
 bool bLogFilteredCurves = false;

 // Descriptor and Frame events
 FOnO3DSDescriptorReady OnDescriptorReady;
 FOnO3DSPoseFrameReady OnPoseFrameReady;

 // Serialized bytes event (component-level relay from serializer). Useful for dev loopback.
 FOnO3DSSerializedFrame OnSerializedFrame;

 // Options provider for device dropdown inside WebRTC audio UX
 UFUNCTION()
 TArray<FName> GetAvailableInputDeviceOptions() const;

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
 void NotifyOnScreen(const FString& Message, const FColor& Color = FColor::Green, float DisplayTime =2.0f) const;

 void EnsureSkeletonCache(USkeletalMeshComponent* SkelComp);
 void RefreshSkeletonCache(USkeletalMeshComponent* SkelComp);
 FString BuildSubjectName(const USkeletalMeshComponent* SkelComp) const;
 // Sanitize a subject name to policy: ASCII only, replace whitespace with '_', allow [-._A-Za-z0-9/]
 FString SanitizeSubjectName(const FString& Raw) const;

 // Curves (Morph + Named Anim Curves)
 void EnsureCurveCache(USkeletalMeshComponent* SkelComp);
 void RefreshCurveCache(USkeletalMeshComponent* SkelComp);
 void CaptureCurves(USkeletalMeshComponent* SkelComp);

 // Filtering helpers
 bool NameMatchesPattern(const FString& Text, const FString& Pattern) const;
 bool IsCurveAllowedByPatterns(const FName& Name) const;
 void BuildFilteredCurves(TArray<FName>& OutNames, TArray<float>& OutValues);

 // Hash helper for descriptor
 uint64 ComputeDescriptorHash(const TArray<FName>& InNames, const TArray<int32>& InParents) const;

 // Cache
 TArray<FName> BoneNames;
 TArray<int32> ParentIndices;
 TWeakObjectPtr<USkeleton> CachedSkeleton;
 TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;

 FO3DSSkeletonDescriptor DescriptorCache;
 bool bDescriptorDirty = false;

 // Curve cache and working buffers
 TArray<FName> CurveNames; // Stable order: Morphs then Anim Curves
 TArray<float> CurveValues; // Same length as CurveNames, latest evaluated values
 TArray<float> LastSentCurveValues; // Last sent values for delta filtering
 TArray<uint8> LastSentHasValue; //0/1 per index indicating LastSentCurveValues set
 TSet<FName> MorphNameSet; // For quick lookup when filling values
 TSet<FName> CurveNameSet; // To avoid duplicates across sources
 bool bCurveCacheInitialized = false;

 bool bIsCapturing = false;
 double LastCaptureTime =0.0;
 uint64 FrameCounter =0;

 // Serializer (M2)
 FO3DSBroadcastSerializer* Serializer = nullptr;

 // Reserved for future delegate-based capture when available
 FDelegateHandle BoneTransformsFinalizedHandle;

 // Optional built-in transport members
 TUniquePtr<IBroadcastTransport> InternalTransport;
 struct FQItem { TArray<uint8> Data; double Ts =0.0; };
 TQueue<FQItem, EQueueMode::Mpsc> SendQueue;
 TAtomic<uint64> QueuedBytes{0};
 TAtomic<uint64> DroppedFrames{0};
 FDelegateHandle SerializedFrameHandle;

 // Two-phase transport initialization for WebRTC audio support:
 // Phase 1: CreateInternalTransport() - Creates transport and prepares connector (WebRTC only)
 // Phase 2: StartInternalTransport() - Starts the actual connection
 // This split allows audio configuration (EnableAudioSend) to occur between phases,
 // ensuring audio tracks are added before PeerConnection creation.
 void CreateInternalTransport();
 void StartInternalTransport();
 void TeardownInternalTransport();
 void OnSerializedForTransport(const FString& /*Subject*/, const TArray<uint8>& Buffer, double Timestamp);

 // Helper properties used only for EditCondition evaluation (kept transient)
 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bTransportFamilyIsNNG = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bTransportFamilyIsTCP = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bTransportFamilyIsWebRTC = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bWebRtcBackendIsLiveKit = false;

 // Presentation helpers to avoid complex edit condition expressions
 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bShowNngProps = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bShowTcpProps = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bShowWebRtcProps = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bShowWebRtcAudioProps = false;

 UPROPERTY(Transient, meta=(AllowPrivateAccess="true"))
 bool bShowLiveKitProps = false;

 // Update helper flags when properties change
 void UpdateEditConditionHelpers();

 // Lifecycle hooks
 #if WITH_EDITOR
 virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
 #endif 	

 // Cached pointer when WebRTC transport is active (no RTTI at call sites)
 class FO3DSWebRtcTransport* WebRtcTransportRaw = nullptr;
};
