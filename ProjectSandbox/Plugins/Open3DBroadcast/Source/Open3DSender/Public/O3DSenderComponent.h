// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/BitArray.h"
#include "O3DSenderSerializer.h"
#include "O3DSPoseFramePool.h"
#include "O3DSenderPipelineStats.h"
#include "Transport/O3DSenderInterface.h"
#include "O3DSenderLogs.h"
#include "Transport/O3DTransportTypes.h"
#include "Transport/O3DTransportOptionSet.h"
#include "O3DSecretStore.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "O3DControlPublisher.h"
#include "O3DControlTypes.h"
#include "Templates/UniquePtr.h"
#include "Templates/Function.h"
#include "O3DSenderComponent.generated.h"

class USkeletalMeshComponent;
class USkeleton;
class USkeletalMesh;
class USoundSubmix;
class FO3DSenderTransportController;
class FO3DSenderMetricsHandle;
class FO3DSenderCurveProcessor;
class FO3DSenderPipeline;
class FO3DSenderPoseSampler;
class FO3DSenderAudioBinding;
struct FO3DSenderAudioSettings;

/** Smart-pointer deleter that keeps FO3DSenderTransportController implementation details private. */
struct FO3DSenderTransportControllerDeleter
{
	void operator()(FO3DSenderTransportController* Ptr) const;
};

/** Smart-pointer deleter for the lazily created curve processor helper. */
struct FO3DSenderCurveProcessorDeleter
{
	void operator()(FO3DSenderCurveProcessor* Ptr) const;
};

/** Smart-pointer deleter that keeps FO3DSenderPoseSampler private (WP-A3). */
struct FO3DSenderPoseSamplerDeleter
{
	void operator()(FO3DSenderPoseSampler* Ptr) const;
};

/** Smart-pointer deleter that keeps FO3DSenderAudioBinding private (WP-A3). */
struct FO3DSenderAudioBindingDeleter
{
	void operator()(FO3DSenderAudioBinding* Ptr) const;
};

/** Predictor for residual/delta coding (roadmap doc §5/C2). Maps to O3DS::ResidualPredictorId
 *  (Hold=1/Linear=2/Quadratic=3 on the wire; None=0 is never used here, since Residual is only
 *  consulted at all when residual coding is enabled). */
UENUM(BlueprintType)
enum class EO3DSenderResidualPredictor : uint8
{
	Hold UMETA(DisplayName = "Hold (reduces to legacy last-sent delta)"),
	Linear UMETA(DisplayName = "Linear (recommended default)"),
	Quadratic UMETA(DisplayName = "Quadratic")
};

/** Describes the skeletal hierarchy and bone metadata emitted during capture. */
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

/** Wire encoding chosen for a frame. Residual takes precedence over quantized when both are enabled. */
enum class EO3DSenderEncodingMode : uint8
{
	Legacy,
	Residual,
	Quantized
};

/**
 * Immutable snapshot of everything the curve filter and the serializer need from the component
 * (SND-14, ADR 0008 item 6): the encoding and the curve filtering settings. The component copies
 * its properties into this on the game thread for every sampled frame, so one frame is always
 * filtered and encoded with one consistent set of settings, and neither stage reads a UObject.
 * The subject name is not here: it is sampled onto the frame (FO3DSPoseFrame::Subject).
 *
 * Copying it allocates nothing: the only heap data, the curve pattern lists, are shared and
 * immutable, and the component replaces them (never edits them) when the properties change.
 */
struct OPEN3DSENDER_API FO3DSenderEncodingSettings
{
	EO3DSenderEncodingMode Mode = EO3DSenderEncodingMode::Legacy;
	EO3DSenderResidualPredictor ResidualPredictor = EO3DSenderResidualPredictor::Linear;
	int32 ResidualKeyframeIntervalFrames = 300;
	float ResidualDeltaThreshold = 0.0001f;
	float QuantizationByteRange = 0.01f;
	float QuantizationHalfRange = 1.0f;
	float QuantizationDeltaThreshold = 0.0001f;
	float FullSyncIntervalSeconds = 1.0f;

	// Curve filtering (WP-A2a: filtering runs on the sampled frame, after sampling). Defaults match
	// the component's property defaults.
	bool bClampMorphCurvesToUnit = true;
	bool bDropNaNAndInfinity = true;
	bool bEnableCurveFiltering = false;
	/** Epsilon/delta value filters; the component turns them off for the persistent encodings. */
	bool bApplyCurveValueFilters = false;
	float CurveEpsilon = 0.0005f;
	float CurveDeltaThreshold = 0.001f;
	bool bLogFilteredCurves = false;
	/** Shared and immutable; null means no patterns. */
	TSharedPtr<const TArray<FString>> IncludeCurvePatterns;
	TSharedPtr<const TArray<FString>> ExcludeCurvePatterns;

	/**
	 * Fingerprint of the settings that need a fresh full sync (and, in residual mode, a fresh
	 * encoder) when they change: mode, residual predictor and keyframe interval, quantization
	 * ranges. Delta thresholds and the full-sync interval apply from the next frame without one.
	 * Curve settings are not part of it: a change that alters the curve list already forces a
	 * full sync through the curve name hash (SND-3).
	 */
	uint64 GetFullSyncFingerprint() const;
};

/**
 * The curves a mesh exposes, in capture order (WP-A2a). Built on the game thread when the curve
 * cache is refreshed and shared, immutable, by every frame sampled against it, so frames carry the
 * names without copying them and the curve filter detects a new list by pointer.
 */
struct FO3DSCurveList
{
	TArray<FName> Names;
	/** Bit per entry of Names: true for a morph target curve (clamped to [0,1] when enabled). */
	TBitArray<> MorphMask;
};

/** Per-frame pose payload containing bone transforms and curve values for a single subject. */
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

	/** The curves the serializer sends: the result of curve filtering (see CurveList). */
	UPROPERTY()
	TArray<FName> CurveNames;

	UPROPERTY()
	TArray<float> CurveValues;

	/**
	 * Raw sampled curves (WP-A2a): the list they were sampled against and one value per entry, in
	 * the same order, before clamping, NaN handling and filtering. The curve filter turns these into
	 * CurveNames/CurveValues. A frame without a CurveList (built by hand, or sampled without a mesh)
	 * is left as it is by the filter.
	 */
	TSharedPtr<const FO3DSCurveList> CurveList;
	TArray<float> RawCurveValues;

	/**
	 * Skeleton descriptor the bones were sampled against (ADR 0005 (i), pull-based descriptor
	 * delivery). The serializer builds names and parents from this and drops the frame when it is
	 * missing or its bone count differs from BoneLocalTransforms. Shared and immutable.
	 */
	TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor;

	/**
	 * Sampling time on the sender clock (FPlatformTime::Seconds(), ADR 0008 item 7). It drives the
	 * periodic full sync and is the time the serializer writes on the wire (WP-A2a).
	 */
	double CaptureTimeSec = 0.0;

	/** Encoding and curve filtering settings this frame is filtered and serialized with. */
	FO3DSenderEncodingSettings Encoding;

	/** Empties the frame for reuse. Arrays and strings keep their allocation (FO3DSPoseFramePool). */
	void Reset()
	{
		Subject.Reset();
		FrameIndex = 0;
		BoneLocalTransforms.Reset();
		CurveNames.Reset();
		CurveValues.Reset();
		CurveList.Reset();
		RawCurveValues.Reset();
		Descriptor.Reset();
		CaptureTimeSec = 0.0;
		Encoding = FO3DSenderEncodingSettings();
	}
};

/** Event emitted whenever the skeletal descriptor changes (usually first frame or mesh swap). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DDescriptorReady, const FString& /*Subject*/, const FO3DSSkeletonDescriptor& /*Descriptor*/);
/**
 * Per-frame event carrying the captured pose prior to serialization, on the game thread. With
 * o3d.Sender.AsyncPipeline on (the default since WP-A2c) curve filtering runs later on the worker,
 * so the frame carries the raw sampled curves (CurveList, RawCurveValues) and CurveNames/
 * CurveValues are empty; with it off the curves are filtered first, as before.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DPoseFrameReady, const FString& /*Subject*/, const FO3DSPoseFrame& /*Frame*/);

/**
 * Captures skeletal pose data (and optionally audio) from an actor, serialises it into the
 * Open3DStream wire format, and forwards frames to a user-selectable transport implementation.
 */
UCLASS(ClassGroup = (Open3DBroadcast), meta = (BlueprintSpawnableComponent))
class OPEN3DSENDER_API UO3DSenderComponent : public UActorComponent
{
	GENERATED_BODY()

	friend class FO3DSenderTransportController;

public:
	UO3DSenderComponent();
	virtual ~UO3DSenderComponent();

	/** Start gathering pose/audio frames. Safe to call when already capturing. */
	UFUNCTION(BlueprintCallable, meta = (CallInEditor), Category = "Open3DBroadcast|Sender")
	void StartCapture();

	/** Halt capture and detach from the active transport/audio sinks. */
	UFUNCTION(BlueprintCallable, meta = (CallInEditor), Category = "Open3DBroadcast|Sender")
	void StopCapture();

	/** Convenience accessor mirroring internal capture state. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Sender")
	bool IsCapturing() const { return bIsCapturing; }

	/** Skeletal mesh that supplies bone transforms; auto-located from the owner if unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender")
	TWeakObjectPtr<USkeletalMeshComponent> TargetMesh;

	/** Subject identifier embedded in serialized frames for downstream routing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender", meta = (DisplayName = "Subject Name"))
	FString SubjectName;

	/** Desired pose capture rate in Hz (final rate clamped by world tick). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender")
	float CaptureRateHz = 60.0f;

	/** Start capture automatically as soon as the component is registered / BeginPlay runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender")
	bool bAutoStartCapture = true;

	/** When true, the component will spawn and manage a transport instance automatically. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Transport")
	bool bAutoCreateTransport = false;

	/** Name of the registered transport factory to use (loopback, sockets, webrtc, ...). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Transport", meta = (HideInDetailPanel))
	FName TransportName = TEXT("loopback");

	/**
	 * Transport-provided key/value overrides populated by modular transport UIs. Hidden from the generic details panel.
	 * Saved with the asset, so it never holds a secret: keys the transport declares secret go to FO3DSecretStore
	 * instead (ADR 0004). "<transport>.credentialProfile" selects which stored secret applies.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Open3DBroadcast|Sender|Transport", meta = (HideInDetailPanel))
	TMap<FString, FString> TransportOptions;

	/**
	 * The options of transports this component used before, by transport name (SND-35, WP-A1 PR 5a).
	 * Switching away from a transport puts its TransportOptions here and switching back restores
	 * them, so a switch no longer loses what the user set. Saved with the asset, so it never holds
	 * a secret (ADR 0004): the outgoing transport's declared secret keys are dropped first.
	 */
	UPROPERTY()
	TMap<FName, FO3DTransportOptionSet> InactiveTransportOptions;

	/** Enable PCM capture and forwarding when the active transport supports it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Audio")
	bool bEnableAudio = false;

	/** Select the audio capture source (game mix vs microphone). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Audio", meta = (EditCondition = "bEnableAudio"))
	EO3DSenderCaptureMode AudioCaptureMode = EO3DSenderCaptureMode::Mix;

	/** Friendly microphone name surfaced to users; resolved back to a device index at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Audio", meta = (GetOptions = "GetAvailableAudioInputDeviceOptions", EditCondition = "bEnableAudio", EditConditionHides))
	FName AudioInputDevice;

	/** Full audio capture configuration (sample rate, bitrate, gains, etc.). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Audio", meta = (EditCondition = "bEnableAudio", ShowOnlyInnerProperties))
	FO3DSenderAudioCaptureConfig AudioCaptureConfig;

	/** Preferred audio codec for transport delivery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Audio", meta = (EditCondition = "bEnableAudio", GetOptions = "GetAvailableAudioCodecOptions"))
	FName AudioCodec = TEXT("PCM16");

	/** Clamp morph target values to [0,1] before serialisation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves")
	bool bClampMorphCurvesToUnit = true;

	/** Treat NaN/Inf curve values as 0 to prevent propagating bad data to receivers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves")
	bool bDropNaNAndInfinity = true;

	/** Enable delta/regex filtering for animation curves prior to emission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering")
	bool bEnableCurveFiltering = false;

	/** Ignore curve delta magnitudes smaller than this epsilon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering", ClampMin = "0.0"))
	float CurveEpsilon = 0.0005f;

	/** Emit a new value only when it changes by more than this threshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering", ClampMin = "0.0"))
	float CurveDeltaThreshold = 0.001f;

	/** Wildcard patterns that whitelist curves for emission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering"))
	TArray<FString> IncludeCurvePatterns;

	/** Wildcard patterns that blacklist curves from emission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering", meta = (EditCondition = "bEnableCurveFiltering"))
	TArray<FString> ExcludeCurvePatterns;

	/** Emit verbose log entries when curves are filtered out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Curves|Filtering")
	bool bLogFilteredCurves = false;

	/** Enable delta/residual transmission (roadmap doc §5/C2) instead of a full snapshot every
	 *  frame. Used only on transports that deliver reliably and in order (Loopback, TCP, NNG pair
	 *  or push, WebRTC's reliable channel): a lost residual frame breaks the predictor history, and
	 *  receivers then hold the subject until the next full sync. On any other transport (UDP, NNG
	 *  pub, MoQ, WebRTC with webrtc.prefer_lossy) the sender logs one warning and sends what it would
	 *  with residual coding off: full snapshots, or quantized updates if quantization below is
	 *  enabled (ADR 0005 (iii)). A full sync every
	 *  FullSyncIntervalSeconds resets the encoder. Per-frame curve epsilon/delta filtering is off
	 *  in this mode. Does not compose with
	 *  quantization below - if both are enabled, Residual takes precedence. Residual frames need
	 *  receivers that implement wire protocol 2 (Open3DBroadcast core 1.1.0 or later); older
	 *  receivers drop them (ADR 0009). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Residual")
	bool bEnableResidualCoding = false;

	/** Predictor for residual coding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Residual", meta = (EditCondition = "bEnableResidualCoding"))
	EO3DSenderResidualPredictor ResidualPredictor = EO3DSenderResidualPredictor::Linear;

	/** Force a residual keyframe (absolute values, re-anchors drift) every N frames. 0 disables
	 *  periodic keyframes (only the first frame / a topology change forces one). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Residual", meta = (EditCondition = "bEnableResidualCoding", ClampMin = "0"))
	int32 ResidualKeyframeIntervalFrames = 300;

	/** Per-channel residual magnitude below which a channel is omitted from the wire. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Residual", meta = (EditCondition = "bEnableResidualCoding", ClampMin = "0.0"))
	float ResidualDeltaThreshold = 0.0001f;

	/** Enable adaptive variable-bit channel quantization (roadmap doc §6/D1) on the legacy
	 *  delta-threshold path instead of a full snapshot every frame. Translations are quantized
	 *  relative to the last full sync, which both ends re-anchor to. Safe on lossy transports: a
	 *  receiver that joins late or misses a full sync holds the subject until the next full sync
	 *  (every FullSyncIntervalSeconds), and a lost update only delays a channel's change until it
	 *  changes again or the next full sync. Per-frame curve epsilon/delta filtering is off in this mode. Does
	 *  not compose with Residual above yet - if both are enabled, Residual takes precedence and this
	 *  is ignored. Quantized frames need receivers that implement wire protocol 2 (Open3DBroadcast
	 *  core 1.1.0 or later); older receivers drop them (ADR 0009). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Quantization")
	bool bEnableQuantization = false;

	/** Max |delta| (from a transform's translation at the last full sync, in the transform's own
	 *  local-space units) representable at the 8-bit quantization tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Quantization", meta = (EditCondition = "bEnableQuantization", ClampMin = "0.0"))
	float QuantizationByteRange = 0.01f;

	/** Max |delta| representable at the 16-bit quantization tier; beyond this a channel falls back
	 *  to full float32 precision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Quantization", meta = (EditCondition = "bEnableQuantization", ClampMin = "0.0"))
	float QuantizationHalfRange = 1.0f;

	/** Per-channel magnitude below which a channel is omitted from the wire entirely - the same
	 *  "send nothing" floor the legacy delta scheme already has; quantization only decides how
	 *  precisely to encode a channel that already cleared this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Quantization", meta = (EditCondition = "bEnableQuantization", ClampMin = "0.0"))
	float QuantizationDeltaThreshold = 0.0001f;

	/** With residual coding or quantization on, send a full skeleton descriptor and pose at least
	 *  this often (ADR 0005 (ii)), so receivers that join late or lost a packet recover within this
	 *  interval. A full sync is also sent on start, on a subject rename, and whenever the skeleton,
	 *  the curve list or the encoding settings change. The legacy encoding sends a full pose every
	 *  frame and ignores this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Encoding", meta = (ClampMin = "0.25", ClampMax = "10.0", UIMin = "0.25", UIMax = "10.0", Units = "s"))
	float FullSyncIntervalSeconds = 1.0f;

	/** Why the last StartCapture() call did not start capture, or empty if it did. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Sender")
	FString GetLastStartCaptureError() const { return LastStartCaptureError; }

	/**
	 * Result of the last automatic transport start (bAutoCreateTransport), for example
	 * InvalidConfig when an option's Validate refused it (WP-A1 PR 5c). Ok before any start. C++
	 * only; capture runs whether or not the transport started.
	 */
	FO3DTransportResult GetLastTransportResult() const;

	// ── Control channel (docs/adr/0011-control-channel.md, item 8) ───────────────────────
	// Cues (events) and parameters (values) for remote clients, carried on this sender's stream.
	// Names, keys and targets are case-sensitive strings. Receivers must accept control (it is off
	// by default there) and use UO3DRemoteControlComponent to react.

	/**
	 * Fire an event on every receiver of this stream, for example a VFX, lighting or audio cue.
	 * TargetSubject (optional) aims it at one character's subject. Needs a running transport;
	 * returns false (and logs why) when the event cannot be sent.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Control")
	bool FireControlEvent(const FString& EventName, const FO3DControlValue& Payload, FString TargetSubject = FString(TEXT("")));

	/**
	 * Set a value on every receiver of this stream, for example an environment or character
	 * parameter. Receivers that join later get it too. May be called before capture starts; it is
	 * sent when the transport starts. Setting the same value again sends nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Control")
	bool SetControlValue(const FString& Key, const FO3DControlValue& Value, FString TargetSubject = FString(TEXT("")));

	/** Remove a value from every receiver. */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Control")
	void ClearControlValue(const FString& Key, FString TargetSubject = FString(TEXT("")));

	/** Remove every value this sender set. */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Control")
	void ClearAllControlValues();

	/** The value this sender holds for Key, if it set one. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Sender|Control")
	bool GetControlValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const;

	/** This component's control source id (a fresh GUID per instance, never saved). */
	FString GetControlSourceId() const;

	/**
	 * Start the transport for control alone when there is no skeletal mesh and audio is off, for
	 * an actor that only sends cues and parameters (a stage or lighting controller).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Control")
	bool bAllowControlOnly = false;

	/** How often the full set of values is re-sent, so late or lossy receivers catch up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Control", meta = (ClampMin = "0.25", ClampMax = "10.0", UIMin = "0.25", UIMax = "10.0", Units = "s"))
	float ControlSnapshotIntervalSeconds = 1.0f;

	/** Copies of each event, on consecutive ticks; receivers drop duplicates. Covers loss on unreliable transports. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Control", meta = (ClampMin = "1", ClampMax = "5"))
	int32 ControlEventRedundancy = 3;

	/** Most times per second one value is re-sent while it keeps changing; the latest value always wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Sender|Control", meta = (ClampMin = "1.0", ClampMax = "120.0"))
	float ControlMaxValueRateHz = 30.0f;

	FOnO3DDescriptorReady OnDescriptorReady;
	FOnO3DPoseFrameReady OnPoseFrameReady;

	/**
	 * Fires after each frame is serialized, before it is sent. Since WP-A2c it fires on the sender
	 * pipeline's worker thread while o3d.Sender.AsyncPipeline is on (the default), so a listener
	 * must be thread-safe, must not touch UObjects and must not wait for the game thread; with it
	 * off it fires on the game thread. Bind and unbind it only while capture is stopped. Kept for
	 * one release (ADR 0008 open question 5), then removed if nothing uses it.
	 */
	FOnO3DSerializedFrame OnSerializedFrame;

	/**
	 * The serializer of this component's pose pipeline (created by the first StartCapture). The
	 * pipeline's worker owns it: only its stats getters may be called while capture runs.
	 */
	FO3DSenderSerializer& GetSerializer() const;

	/** Counters of the pose pipeline (queue, drops, worker time, capture-to-send latency); zero before the first StartCapture. Any thread. */
	FO3DSenderPipelineStats GetPipelineStats() const;

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
	/**
	 * Makes Mesh (null: none) the one skeletal mesh this component's tick waits for (ADR 0008 item 9,
	 * SND-12, WP-A2b). The prerequisite on the previously bound mesh is removed first; binding the
	 * mesh that is already bound changes nothing. Game thread only.
	 */
	void SetTickPrerequisiteMesh(USkeletalMeshComponent* Mesh);
	void HandleBoneTransformsFinalized();
	void NotifyOnScreen(const FString& Message, const FColor& Color = FColor::Green, float DisplayTime = 2.0f) const;

	/** Rebuilds the skeleton descriptor when the mesh changed (the pose sampler), and then the curve cache. */
	void EnsureSkeletonCache(USkeletalMeshComponent* SkelComp);
	/**
	 * Brings EncodingSnapshot up to date with the properties and returns it (SND-14, WP-A2a). Run
	 * for every sampled frame, because Blueprint can write the properties without any notification;
	 * a curve pattern list is copied into a new shared array only when its contents changed.
	 */
	const FO3DSenderEncodingSettings& UpdateEncodingSnapshot();

	/**
	 * Skeleton descriptor, subject name, frame index and bone sampling (WP-A3 step 6). Always set
	 * (created by the constructor); its callbacks reach this component's delegate, audio label and
	 * pipeline.
	 */
	TUniquePtr<FO3DSenderPoseSampler, FO3DSenderPoseSamplerDeleter> PoseSampler;
	FString LastStartCaptureError;

	bool bIsCapturing = false;
	double LastCaptureTime = 0.0;

	/**
	 * The pose pipeline (ADR 0008 item 3, WP-A2c): frame pool, queue, curve filter, serializer and
	 * the worker that sends. Created by the first StartCapture and kept across Stop/Start; a task
	 * in flight holds its own reference, so releasing it never waits.
	 */
	TSharedPtr<FO3DSenderPipeline> Pipeline;

	/** Settings snapshot copied onto every sampled frame (see UpdateEncodingSnapshot). */
	FO3DSenderEncodingSettings EncodingSnapshot;

	FDelegateHandle BoneTransformsFinalizedHandle;

	/** The skeletal mesh this component's tick waits for (SetTickPrerequisiteMesh); unset when none. */
	TWeakObjectPtr<USkeletalMeshComponent> TickPrerequisiteMesh;
	/**
	 * That mesh's tick function. Only compared, never dereferenced: it finds the entry to remove
	 * when the mesh was garbage-collected before it could be unbound.
	 */
	const FTickFunction* TickPrerequisiteFunction = nullptr;

	TUniquePtr<FO3DSenderTransportController, FO3DSenderTransportControllerDeleter> TransportController;

	/**
	 * This component's sender metrics (ADR 0012 item 4), from the default runtime context until
	 * PR 4. Acquired on the first transport start and kept across restarts; passed to the
	 * transport in FO3DTransportConfig::SenderMetrics. Game thread.
	 */
	TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> SenderMetricsHandle;

public:
	/** This component's sender metrics handle, or null before its first transport start. Game thread. */
	TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> GetSenderMetricsHandle() const { return SenderMetricsHandle; }

private:

	/** Control publisher (ADR 0011); created on first use. Not a UPROPERTY: a duplicated component gets its own. */
	TUniquePtr<FO3DControlPublisher> ControlPublisher;
	FO3DControlPublisher& EnsureControlPublisher();
	/** Starts control on the running transport when it carries control. */
	void StartControl();
	void TickControl();
	/** Curve capture (game thread: reads the mesh). Filtering is in the pipeline (WP-A2c). */
	TUniquePtr<FO3DSenderCurveProcessor, FO3DSenderCurveProcessorDeleter> CurveProcessor;
	UPROPERTY(Transient)
	UO3DSenderAudioCaptureComponent* AudioCaptureComponent = nullptr;
	/** Audio capture to transport binding (WP-A3 step 7). Always set (created by the constructor). */
	TUniquePtr<FO3DSenderAudioBinding, FO3DSenderAudioBindingDeleter> AudioBinding;

	void UpdateEditConditionHelpers();
	void TeardownTransport();
	void InitializeTransport();
	FO3DTransportConfig BuildTransportConfig() const;
	/** BuildTransportConfig; without bResolveSecrets the secret store is not read (Config.Secrets stays empty). */
	FO3DTransportConfig BuildTransportConfigImpl(bool bResolveSecrets) const;
	/** The running sender's delivery guarantee; Unknown when no sender is attached. */
	EO3DDeliveryGuarantee GetActiveDeliveryGuarantee() const;
	/** Logs the residual fallback once per transport and guarantee within a capture (ADR 0005 (iii)). */
	void WarnResidualFallback(EO3DDeliveryGuarantee Delivery);
	/** What WarnResidualFallback last warned about ("<transport>/<guarantee>"); cleared by StartCapture. */
	FString ResidualFallbackWarnedFor;
	void UpdateAudioCaptureBinding();
	/** Stops the pipeline from calling into this component (listener) and from sending (sender). Waits for a frame being processed. */
	void DetachPipeline();

public:
	/**
	 * The encoding a frame is sent with (ADR 0005 (iii)): residual only when the transport
	 * delivers reliably and in order; otherwise what the settings give without it (quantized when
	 * quantization is enabled, else full snapshots). Residual takes precedence over quantization
	 * when both are enabled.
	 */
	static EO3DSenderEncodingMode ResolveEncodingMode(bool bResidual, bool bQuantization, EO3DDeliveryGuarantee Delivery);

	/**
	 * The warning shown and logged when residual coding is enabled on a transport that does not
	 * deliver reliably and in order; empty when it does (ADR 0005 (iii)).
	 */
	static FText GetResidualFallbackWarning(FName InTransportName, EO3DDeliveryGuarantee Delivery);

	/**
	 * The delivery guarantee of the selected transport with the current options, from the
	 * transport registry, without starting it or reading secrets (Unknown when the transport is
	 * not registered). For the details panel.
	 */
	EO3DDeliveryGuarantee GetConfiguredDeliveryGuarantee() const;

	/** GetResidualFallbackWarning for the selected transport and options; empty when residual coding is off. */
	FText GetConfiguredResidualFallbackWarning() const;

	/**
	 * Retrieve a transport option by key (case-sensitive). Returns empty string if missing.
	 * Always empty for a key the active transport declares secret (ADR 0004).
	 */
	FString GetTransportOption(const FString& Key) const;

	/**
	 * Set or update a transport option. Passing an empty value removes the key.
	 * A key the active transport declares secret is routed to FO3DSecretStore for this session
	 * (no Modify(), never stored in TransportOptions); an empty value then clears it.
	 */
	void SetTransportOption(const FString& Key, const FString& Value);

	/** True when the active transport's customization declares Key as a secret option. */
	bool IsTransportSecretKey(const FString& Key) const;

	/** Credential profile selected by "<transport>.credentialProfile"; "default" when unset. */
	FString GetCredentialProfile() const;

	/** Stores a secret for the active transport and profile. An empty value clears it. */
	void SetTransportSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence = EO3DSecretPersistence::Session);

	/** Moves an existing session secret to or from the per-user "remember on this machine" store. */
	bool SetTransportSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence);

	/** Clears the session secret and any remembered copy. An environment variable still applies. */
	void ClearTransportSecret(const FString& Key);

	/** Where a secret for the active transport and profile would resolve from. Never returns the value. */
	FO3DSecretStatus GetTransportSecretStatus(const FString& Key) const;

	/** Remove all transport options of the selected transport. */
	void ClearTransportOptions();

	/**
	 * Puts the options of From away in InactiveTransportOptions and restores those of To (SND-35).
	 * Records the change for undo. From's declared secret keys are never put away.
	 */
	void SwitchTransportOptions(FName From, FName To);

	/** The cached capture device names (ADR 0008 item 8); never enumerates. */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Audio")
	TArray<FName> GetAvailableAudioInputDeviceOptions() const;

	/**
	 * Enumerate the audio capture devices again (ADR 0008 item 8). StartCapture does this once
	 * when it captures from an input device; call it after plugging in a device to update the
	 * picker. Game thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Sender|Audio")
	static void RefreshAudioInputDevices();

	UFUNCTION()
	TArray<FName> GetAvailableAudioCodecOptions() const;


private:
	bool CanCaptureThisFrame(double NowSeconds, USkeletalMeshComponent*& OutMesh);
	FString ResolveSubjectName(const USkeletalMeshComponent* SkelComp);
	/** Fills the frame's subject, index, descriptor, sampling time and settings snapshot. */
	void FillFrameShell(const USkeletalMeshComponent* SkelComp, double CaptureTimeSec, FO3DSPoseFrame& Frame);
	/** The pose sampler's subject-name change: audio stream label (SND-16), then rename handling (SND-1). */
	void HandleSubjectNameChanged(const FString& PreviousName, const FString& NewName);
	/** Samples raw curve values (no filtering) into Frame.CurveList / Frame.RawCurveValues. */
	void PopulatePoseFrameCurves(USkeletalMeshComponent* SkelComp, FO3DSPoseFrame& Frame, bool bDebugCurves);
	/** A pooled frame from the pipeline; null without a pipeline or when every frame is out. */
	TUniquePtr<FO3DSPoseFrame> AcquirePoseFrame();
	/**
	 * Hands a sampled frame to the pipeline (WP-A2c) and fires OnPoseFrameReady on the game thread.
	 * Asynchronous: the delegate sees the raw curves and the worker filters, serializes and sends.
	 * Synchronous (o3d.Sender.AsyncPipeline 0 at StartCapture): filtered first, then the delegate,
	 * then serialized and sent inside this call, as in WP-A2b.
	 */
	void DispatchSampledFrame(TUniquePtr<FO3DSPoseFrame>&& Frame);
	/** Tests: waits (event with a timeout) until the pipeline's worker has nothing left. True without a pipeline. */
	bool WaitForPipelineIdle(double TimeoutSeconds) const;
	/** The audio properties as one value, for FO3DSenderAudioBinding. */
	FO3DSenderAudioSettings GetAudioSettings() const;
	FO3DSenderAudioCaptureConfig BuildAudioCaptureConfig() const;
	void TeardownAudioCapture();
	void SyncAudioConfigSource();
	void EnsureSubjectNameCached(const USkeletalMeshComponent* SkelComp);
	void InvalidateSubjectNameCache();
	void PurgeSerializerCacheForSubject(const FString& Subject);

	static bool ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz);

	// Test-only white-box access, defined in Public/Testing/O3DSenderTesting.h (WP-T2) and in the
	// Open3DBroadcastTests secrets tests (WP-S9). Unconditional: a friend declaration must not
	// depend on WITH_DEV_AUTOMATION_TESTS.
	friend struct FO3DSenderComponentTestAccess;
	friend struct FO3DSenderSecretsTestAccess;

	/**
	 * Migration (ADR 0004 item 4): moves any declared secret key found in loaded TransportOptions
	 * into the session store, removes it from the map and logs one Warning naming this component's
	 * package (never the value). Does not mark the package dirty or save it. Returns the number of
	 * keys moved.
	 */
	int32 MigrateLegacySecretOptions();

	/** Records a change of this component for undo (Modify), except on the class default object. */
	void RecordChangeForUndo();

	/** Transport name the options and secrets belong to (TransportName, or the default when None). */
	FName GetSelectedTransportName() const;

	void EnsureValidTransportName();

#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** The selected transport before an edit of TransportName (PreEditChange); None otherwise. */
	FName TransportNameBeforeEdit;
#endif
};
