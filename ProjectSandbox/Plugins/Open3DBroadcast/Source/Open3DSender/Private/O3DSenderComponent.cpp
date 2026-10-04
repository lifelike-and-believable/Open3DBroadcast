// Copyright (c) Open3DStream Contributors

#include "O3DSenderComponent.h"

#include "O3DHelpers.h"
#include "O3DRuntimeContext.h"
#include "O3DRuntimeSubsystem.h"
#include "O3DSenderLogs.h"
#include "O3DSenderSerializer.h"
#include "O3DSenderCurveProcessor.h"
#include "O3DSenderPipeline.h"
#include "O3DSenderPoseSampler.h"
#include "O3DSenderAudioBinding.h"
#include "O3DSenderTransportSettings.h"
#include "O3DSenderTransportController.h"
#include "Transport/O3DTransportRegistry.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "O3DAudioFrameCodec.h"
#include "O3DAudioInputDevices.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/sender_sync.h"
THIRD_PARTY_INCLUDES_END

#define LOCTEXT_NAMESPACE "O3DSenderComponent"

static TAutoConsoleVariable<int32> CVarO3DSenderDebugPose(
	TEXT("o3ds.Sender.DebugPose"),
	0,
	TEXT("Enable per-frame pose debug logging for UO3DSenderComponent (0/1)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSenderDebugCurves(
	TEXT("o3ds.Sender.DebugCurves"),
	0,
	TEXT("Enable per-frame curve debug logging for UO3DSenderComponent (0/1)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSenderOnScreen(
	TEXT("o3ds.Sender.OnScreen"),
	0,
	TEXT("Show on-screen notifications for sender component state changes (0/1)."),
	ECVF_Default);

static const FName DefaultSenderTransportName(TEXT("loopback"));

void FO3DSenderTransportControllerDeleter::operator()(FO3DSenderTransportController* Ptr) const
{
	delete Ptr;
}

void FO3DSenderPoseSamplerDeleter::operator()(FO3DSenderPoseSampler* Ptr) const
{
	delete Ptr;
}

void FO3DSenderAudioBindingDeleter::operator()(FO3DSenderAudioBinding* Ptr) const
{
	delete Ptr;
}

void FO3DSenderCurveProcessorDeleter::operator()(FO3DSenderCurveProcessor* Ptr) const
{
	delete Ptr;
}

uint64 FO3DSenderEncodingSettings::GetFullSyncFingerprint() const
{
	uint64 Hash = 1469598103934665603ull;
	auto Mix = [&Hash](uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
	};
	auto FloatBits = [](float Value) -> uint64
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return (uint64)Bits;
	};

	Mix((uint64)Mode);
	if (Mode == EO3DSenderEncodingMode::Residual)
	{
		Mix((uint64)ResidualPredictor);
		Mix((uint64)(uint32)ResidualKeyframeIntervalFrames);
	}
	else if (Mode == EO3DSenderEncodingMode::Quantized)
	{
		Mix(FloatBits(QuantizationByteRange));
		Mix(FloatBits(QuantizationHalfRange));
	}
	return Hash;
}

UO3DSenderComponent::~UO3DSenderComponent()
{
	// A task still in flight keeps the pipeline alive (WP-A2c); it must no longer reach this
	// component's delegate or send through a transport this component is about to release.
	DetachPipeline();
}

UO3DSenderComponent::UO3DSenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// SND-12, ADR 0008 item 9 (WP-A2b): capture runs after animation evaluation and physics in the
	// frame, so it samples the pose the target mesh ends the frame with. BindToTarget also makes the
	// mesh's tick a prerequisite, which orders the two even if the mesh is moved to this group.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	SetComponentTickEnabled(false);
	EnsureValidTransportName();
	TransportController.Reset(new FO3DSenderTransportController());
	CurveProcessor.Reset(new FO3DSenderCurveProcessor());
	PoseSampler.Reset(new FO3DSenderPoseSampler());
	AudioBinding.Reset(new FO3DSenderAudioBinding());
	PoseSampler->SetCallbacks(
		[this](const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor) { OnDescriptorReady.Broadcast(Subject, Descriptor); },
		[this](const FString& PreviousName, const FString& NewName) { HandleSubjectNameChanged(PreviousName, NewName); });
	SyncAudioConfigSource();
	PoseSampler->InvalidateSubjectName(SubjectName);
}

void UO3DSenderComponent::BeginPlay()
{
	Super::BeginPlay();
	SyncAudioConfigSource();

	if (!TargetMesh.IsValid())
	{
		if (AActor* Owner = GetOwner())
		{
			TargetMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		}
	}

	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender component BeginPlay on %s"), *GetNameSafe(GetOwner()));

	if (bAutoStartCapture)
	{
		StartCapture();
	}
}

void UO3DSenderComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopCapture();

	// The pipeline (and its serializer) goes with the play session, as the serializer did before
	// WP-A2c. A task still running holds its own reference and ends on its own; it no longer calls
	// this component or the transport.
	DetachPipeline();
	if (TransportController.IsValid())
	{
		TransportController->SetPipeline(nullptr);
	}
	Pipeline.Reset();

	TeardownTransport();

	Super::EndPlay(EndPlayReason);
}

void UO3DSenderComponent::OnRegister()
{
	Super::OnRegister();
	EnsureValidTransportName();
	SyncAudioConfigSource();
	UpdateEditConditionHelpers();
}

void UO3DSenderComponent::PostInitProperties()
{
	Super::PostInitProperties();
	EnsureValidTransportName();
	SyncAudioConfigSource();
	UpdateEditConditionHelpers();
}

#if WITH_EDITORONLY_DATA
void UO3DSenderComponent::PostLoad()
{
	Super::PostLoad();
	EnsureValidTransportName();
	MigrateLegacySecretOptions();
	SyncAudioConfigSource();
	UpdateEditConditionHelpers();
}
#endif

void UO3DSenderComponent::NotifyOnScreen(const FString& Message, const FColor& Color, float DisplayTime) const
{
	if (CVarO3DSenderOnScreen.GetValueOnAnyThread() == 0)
	{
		return;
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, DisplayTime, Color, Message);
	}
}

/** Orchestrates transport/audio bootstrapping and begins sampling skeletal data. */
void UO3DSenderComponent::StartCapture()
{
	if (bIsCapturing)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (!World->IsGameWorld())
		{
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Skip StartCapture outside of game world (editor change)"));
			return;
		}
	}

	// SND-19: check the preconditions before creating the serializer or
	// starting a transport (which may open sockets), so a failed start leaves
	// nothing running.
	LastStartCaptureError.Reset();
	ResidualFallbackWarnedFor.Reset();
	if (!TargetMesh.IsValid() && !bEnableAudio && !bAllowControlOnly)
	{
		LastStartCaptureError = TEXT("No valid TargetMesh, audio is disabled and control-only is off.");
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender capture not started on %s: %s"), *GetNameSafe(GetOwner()), *LastStartCaptureError);
		NotifyOnScreen(FString::Printf(TEXT("O3D Sender: not started (%s)"), *LastStartCaptureError), FColor::Red, 4.0f);
		return;
	}

	if (!Pipeline.IsValid())
	{
		Pipeline = MakeShared<FO3DSenderPipeline>();
	}

	// The pipeline holds no pointer back to this component (SND-22, ADR 0008 item 2) except
	// OnSerializedFrame's address, which StopCapture, EndPlay and the destructor remove, waiting for
	// a frame the worker is processing. The mode (o3d.Sender.AsyncPipeline) is latched here; the
	// Start item resets the curve filter, as this function did before WP-A2c.
	Pipeline->SetStatsLabel(GetPathName());
	Pipeline->SetSerializedFrameListener(&OnSerializedFrame);
	Pipeline->Start(FO3DSenderPipeline::IsAsyncEnabledByConsole());

	if (CurveProcessor.IsValid())
	{
		CurveProcessor->Reset();
	}

	// ADR 0008 item 8 (SND-18): enumerate the capture devices once per start, before the device
	// index is resolved (transport config and capture config both read the cache).
	if (bEnableAudio && AudioCaptureMode == EO3DSenderCaptureMode::Input)
	{
		FO3DAudioInputDevices::Get().Refresh();
	}

	InitializeTransport();
	// Configures the capture component and opens the device once per start (SND-18), then binds
	// the started transport's sink, if any.
	UpdateAudioCaptureBinding();

	BindToTarget();
	const bool bHasValidMesh = TargetMesh.IsValid();
	bIsCapturing = bHasValidMesh || bEnableAudio || bAllowControlOnly;
	LastCaptureTime = 0.0;
	PoseSampler->ResetFrameCounter();

	if (bIsCapturing)
	{
		// Tick drives HandleBoneTransformsFinalized()/frame sampling, so it must track bIsCapturing
		// directly. Do not rely on InitializeTransport() to enable it: that only happens on the
		// auto-create-transport success path, leaving capture silently inert whenever
		// bAutoCreateTransport is false or the transport fails to start.
		SetComponentTickEnabled(true);

		if (bHasValidMesh)
		{
			UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture started on %s"), *GetNameSafe(TargetMesh.Get()));
			NotifyOnScreen(FString::Printf(TEXT("O3D Sender: Started on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Green, 2.0f);
		}
		else
		{
			UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender audio capture started without a skeletal mesh."));
			NotifyOnScreen(TEXT("O3D Sender: Audio capture active"), FColor::Green, 2.0f);
		}
	}
	else
	{
		// Defensive (SND-19): the precondition check above should make this
		// unreachable. Undo everything started above so no transport or
		// serializer is left running while bIsCapturing is false, which
		// StopCapture() would not clean up.
		LastStartCaptureError = TEXT("No valid skeletal mesh.");
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender capture failed to start on %s: %s"), *GetNameSafe(GetOwner()), *LastStartCaptureError);
		NotifyOnScreen(FString::Printf(TEXT("O3D Sender: not started (%s)"), *LastStartCaptureError), FColor::Red, 4.0f);
		if (Pipeline.IsValid())
		{
			Pipeline->Stop();
			Pipeline->SetSerializedFrameListener(nullptr);
		}
		UnbindFromTarget();
		PoseSampler->ResetSkeleton();
		TeardownTransport();
	}
}

/** Stop ticking transports, detach delegates, and release capture helpers. */
void UO3DSenderComponent::StopCapture()
{
	if (!bIsCapturing)
	{
		return;
	}

	InvalidateSubjectNameCache();
	UnbindFromTarget();
	bIsCapturing = false;

	// Keep tick state tied directly to bIsCapturing; TeardownTransport() below also disables tick
	// as a side effect, but this makes the invariant explicit regardless of transport state.
	SetComponentTickEnabled(false);

	// ADR 0008 item 10: queued frames are discarded and a Stop item clears the serializer caches
	// and the curve filter; nothing waits for the network. Removing the listener waits for a frame
	// the worker is processing, so OnSerializedFrame never fires after StopCapture returns.
	if (Pipeline.IsValid())
	{
		Pipeline->Stop();
		Pipeline->SetSerializedFrameListener(nullptr);
	}

	// SND-1: forget the cached skeleton so the next StartCapture() rebuilds
	// the descriptor (and re-broadcasts OnDescriptorReady) even for the same
	// mesh.
	PoseSampler->ResetSkeleton();

	if (CurveProcessor.IsValid())
	{
		CurveProcessor->Reset();
	}

	// Detaches the transport from the pipeline before stopping it (FO3DSenderTransportController::Stop).
	TeardownTransport();

	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture stopped on %s"), *GetNameSafe(TargetMesh.Get()));
	NotifyOnScreen(FString::Printf(TEXT("O3D Sender: Stopped on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Yellow, 2.0f);
}

/** Stop ticking the active transport and release audio capture bindings. */
void UO3DSenderComponent::TeardownTransport()
{
	if (ControlPublisher.IsValid())
	{
		ControlPublisher->Stop(); // values are kept for the next start
	}
	TeardownAudioCapture();
	if (TransportController.IsValid())
	{
		TransportController->Stop();
	}

	SetComponentTickEnabled(false);
}

/** Prepare or refresh the active transport instance and hook up audio sinks if available. */
void UO3DSenderComponent::InitializeTransport()
{
	TeardownTransport();

	if (!bAutoCreateTransport)
	{
		return;
	}

	if (!Pipeline.IsValid())
	{
		return;
	}

	if (!TransportController.IsValid())
	{
		TransportController.Reset(new FO3DSenderTransportController());
	}
	// The started sender is handed to the pipeline, whose worker sends the pose frames (WP-A2c).
	TransportController->SetPipeline(Pipeline);

	// If the transport unregisters while active (its module shuts down), drop everything that
	// references the sender or its sinks before the controller releases the sender, so the
	// registry's drain finds no live instance (ADR 0007 item 5, WP-A1 PR 2).
	TransportController->SetOnTransportUnregistering([WeakThis = TWeakObjectPtr<UO3DSenderComponent>(this)]()
	{
		if (UO3DSenderComponent* Self = WeakThis.Get())
		{
			Self->TeardownTransport();
		}
	});

	FO3DTransportConfig Config = BuildTransportConfig();
	// The context ContextName names (ADR 0012 item 5). One handle per context for this component's
	// life, so its counts survive transport restarts and DumpMetrics names the component, not the
	// transport instance (item 4); a new context needs a new handle, since a transport refuses a
	// handle from another context.
	const FO3DRuntimeContextRef Context = UO3DRuntimeSubsystem::Resolve(ContextName);
	if (!SenderMetricsHandle.IsValid() || &SenderMetricsHandle->GetAggregate() != &Context->GetMetrics())
	{
		const AActor* OwnerActor = GetOwner();
		SenderMetricsHandle = Context->GetMetrics().AcquireSenderMetrics(
			FString::Printf(TEXT("Sender (%s, subject %s)"), OwnerActor ? *OwnerActor->GetName() : TEXT("no owner"), *SubjectName));
	}
	SenderContext = Context;
	Config.Context = Context;
	Config.SenderMetrics = SenderMetricsHandle;
	if (!TransportController->Start(Config))
	{
		return;
	}

	// Note: tick enablement is driven by StartCapture() based on bIsCapturing, not by transport
	// start success here, so pose capture still runs for externally-managed transports and even
	// when auto-transport creation fails. StartCapture binds the audio sink right after this
	// returns; binding it here as well opened the capture device twice per start (SND-18).

	StartControl();
	UE_LOG(LogO3DSenderComponent, Log, TEXT("Auto transport '%s' initialized."), *TransportController->GetConfig().Transport.ToString());
}

EO3DSenderEncodingMode UO3DSenderComponent::ResolveEncodingMode(bool bResidual, bool bQuantization, EO3DDeliveryGuarantee Delivery)
{
	if (bResidual && Delivery == EO3DDeliveryGuarantee::ReliableOrdered)
	{
		return EO3DSenderEncodingMode::Residual;
	}
	// Residual is off, or the transport cannot carry it: what the settings give without it.
	// Quantization is used only when it was enabled itself (CORE-12: never switched on implicitly).
	return bQuantization ? EO3DSenderEncodingMode::Quantized : EO3DSenderEncodingMode::Legacy;
}

FText UO3DSenderComponent::GetResidualFallbackWarning(FName InTransportName, EO3DDeliveryGuarantee Delivery)
{
	if (Delivery == EO3DDeliveryGuarantee::ReliableOrdered)
	{
		return FText::GetEmpty();
	}
	return FText::Format(NSLOCTEXT("O3DSenderComponent", "ResidualFallbackWarning",
		"Residual coding needs a transport that delivers reliably and in order; '{0}' is {1}. Frames are sent without residual coding (full snapshots, or quantized updates when quantization is enabled)."),
		FText::FromName(InTransportName),
		FText::FromString(Delivery == EO3DDeliveryGuarantee::Unreliable ? TEXT("unreliable") : TEXT("of unknown reliability")));
}

EO3DDeliveryGuarantee UO3DSenderComponent::GetConfiguredDeliveryGuarantee() const
{
	const FName SelectedTransport = GetSelectedTransportName();
	if (SelectedTransport.IsNone())
	{
		return EO3DDeliveryGuarantee::Unknown;
	}
	FO3DTransportCapabilities Capabilities;
	if (!FO3DTransportRegistry::Get().GetCapabilities(SelectedTransport, BuildTransportConfigImpl(false), Capabilities))
	{
		return EO3DDeliveryGuarantee::Unknown;
	}
	return Capabilities.Delivery;
}

FText UO3DSenderComponent::GetConfiguredResidualFallbackWarning() const
{
	return bEnableResidualCoding ? GetResidualFallbackWarning(GetSelectedTransportName(), GetConfiguredDeliveryGuarantee()) : FText::GetEmpty();
}

EO3DDeliveryGuarantee UO3DSenderComponent::GetActiveDeliveryGuarantee() const
{
	const TSharedPtr<IOpen3DSender> SenderInstance = TransportController.IsValid() ? TransportController->GetSender() : TSharedPtr<IOpen3DSender>();
	return SenderInstance.IsValid() ? SenderInstance->GetCapabilities().Delivery : EO3DDeliveryGuarantee::Unknown;
}

void UO3DSenderComponent::WarnResidualFallback(EO3DDeliveryGuarantee Delivery)
{
	// Without a sender nothing is sent, so there is nothing to warn about yet.
	const TSharedPtr<IOpen3DSender> SenderInstance = TransportController.IsValid() ? TransportController->GetSender() : TSharedPtr<IOpen3DSender>();
	if (!SenderInstance.IsValid())
	{
		return;
	}
	const FName Transport = TransportController->GetConfig().Transport;
	const FString Key = FString::Printf(TEXT("%s/%s"), *Transport.ToString(), LexToString(Delivery));
	if (Key == ResidualFallbackWarnedFor)
	{
		return;
	}
	ResidualFallbackWarnedFor = Key;
	UE_LOG(LogO3DSenderComponent, Warning, TEXT("%s: %s"), *GetPathName(), *GetResidualFallbackWarning(Transport, Delivery).ToString());
}

FO3DTransportConfig UO3DSenderComponent::BuildTransportConfig() const
{
	return BuildTransportConfigImpl(true);
}

FO3DTransportConfig UO3DSenderComponent::BuildTransportConfigImpl(bool bResolveSecrets) const
{
	const FO3DSenderAudioCaptureConfig CaptureConfig = BuildAudioCaptureConfig();

	const FName SelectedTransport = GetSelectedTransportName();
	// The registered name and the side (TRB-27, WP-A1 PR 5c).
	FO3DTransportConfig Config(SelectedTransport, EO3DTransportRole::Sender);
	// The transport may use it as a default (MoQ: the stream id) without reading this component
	// (WP-A1 PR 5a).
	Config.SubjectName = SubjectName;

	// Declared secret keys are never copied into the options; they are resolved from the secret
	// store into Config.Secrets (ADR 0004).
	TMap<FString, FString> Options;
	if (bResolveSecrets)
	{
		FO3DSenderTransportSettings::BuildConfigOptions(TransportOptions, SelectedTransport, Options, Config.Secrets);
	}
	else
	{
		FO3DSenderTransportSettings::BuildPublicOptions(TransportOptions, SelectedTransport, Options);
	}
	Config.AdvancedParams = Options;

	Config.Audio = FO3DSenderAudioBinding::BuildTransportConfig(GetAudioSettings(), CaptureConfig);

	// The descriptor is a shared, immutable snapshot, so the function stays valid while it runs
	// even if the transport unregisters meanwhile (RCV-27).
	const FO3DTransportDescriptorPtr Descriptor = Config.Transport.IsNone() ? FO3DTransportDescriptorPtr() : FO3DTransportRegistry::Get().Find(SelectedTransport);
	if (Descriptor.IsValid())
	{
		Config.OptionSchema = MakeShared<FO3DTransportOptionSchema>(Descriptor->SenderOptions.OptionSchema);
		if (Descriptor->ConfigureSender)
		{
			// The view is over this function's own copy, so it stays valid whatever the configure
			// function adds to Config.AdvancedParams (WP-A1 PR 5a).
			Descriptor->ConfigureSender(FO3DTransportOptionsView(Options, Config.OptionSchema.Get()), Config);
		}
	}

	return Config;
}

/**
 * Frames reach the transport through the pose pipeline (ADR 0008, WP-A2c): its worker serializes
 * each frame once (full sync or delta/residual update, decided by FO3DSenderSerializer, not per
 * transport), broadcasts OnSerializedFrame and hands the bytes to IOpen3DSender::SendSerialized.
 * This replaces HandleSerializedFrameForward, which did the same on the game thread.
 */
FO3DSenderSerializer& UO3DSenderComponent::GetSerializer() const
{
	check(Pipeline.IsValid());
	return Pipeline->GetSerializer();
}

FO3DSenderPipelineStats UO3DSenderComponent::GetPipelineStats() const
{
	return Pipeline.IsValid() ? Pipeline->GetStats() : FO3DSenderPipelineStats();
}

void UO3DSenderComponent::DetachPipeline()
{
	if (!Pipeline.IsValid())
	{
		return;
	}
	Pipeline->SetSerializedFrameListener(nullptr);
	Pipeline->DetachSender();
}

bool UO3DSenderComponent::WaitForPipelineIdle(double TimeoutSeconds) const
{
	return !Pipeline.IsValid() || Pipeline->WaitForIdle(TimeoutSeconds);
}

TArray<FName> UO3DSenderComponent::GetAvailableAudioInputDeviceOptions() const
{
	// The cached list (ADR 0008 item 8): a GetOptions callback never enumerates.
	return FO3DAudioInputDevices::Get().GetNames();
}

void UO3DSenderComponent::RefreshAudioInputDevices()
{
	FO3DAudioInputDevices::Get().Refresh();
}

TArray<FName> UO3DSenderComponent::GetAvailableAudioCodecOptions() const
{
	TArray<FName> Options;
	Options.Add(FName(TEXT("PCM16")));
#if O3D_WITH_OPUS
	Options.Add(FName(TEXT("Opus")));
#endif
	return Options;
}

void UO3DSenderComponent::UpdateAudioCaptureBinding()
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}

	if (!bEnableAudio)
	{
		if (AudioCaptureComponent)
		{
			AudioCaptureComponent->SetAudioSink(nullptr, FString());
		}
		return;
	}

	AudioCaptureComponent = FO3DSenderAudioBinding::FindOrCreateCaptureComponent(GetOwner(), AudioCaptureComponent);
	if (!AudioCaptureComponent)
	{
		return;
	}

	const FO3DSenderAudioSettings Settings = GetAudioSettings();
	FO3DSenderAudioBinding::Configure(*AudioCaptureComponent, Settings, FO3DSenderAudioBinding::BuildCaptureConfig(Settings));

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;
	if (TransportController.IsValid() && TransportController->IsActive())
	{
		AudioSink = TransportController->GetAudioSink();
	}

	// SND-16: the audio stream label is the resolved pose subject name (sanitized, or generated
	// from World/Actor/Component when SubjectName is empty), the same name pose frames carry.
	// With no mesh and no SubjectName there is no pose subject; the capture component then
	// uses its default label.
	USkeletalMeshComponent* Mesh = TargetMesh.Get();
	const FString AudioLabel = (Mesh || !SubjectName.IsEmpty()) ? ResolveSubjectName(Mesh) : FString();
	AudioBinding->AttachSink(*AudioCaptureComponent, AudioSink, AudioLabel, TransportName, FPlatformTime::Seconds());
}

FO3DSenderAudioSettings UO3DSenderComponent::GetAudioSettings() const
{
	FO3DSenderAudioSettings Settings;
	Settings.bEnableAudio = bEnableAudio;
	Settings.Mode = AudioCaptureMode;
	Settings.InputDevice = AudioInputDevice;
	Settings.Codec = AudioCodec;
	Settings.CaptureConfig = AudioCaptureConfig;
	return Settings;
}

FO3DSenderAudioCaptureConfig UO3DSenderComponent::BuildAudioCaptureConfig() const
{
	return FO3DSenderAudioBinding::BuildCaptureConfig(GetAudioSettings());
}

void UO3DSenderComponent::TeardownAudioCapture()
{
	AudioBinding->Detach(AudioCaptureComponent);
}

void UO3DSenderComponent::SyncAudioConfigSource()
{
	FO3DSenderAudioBinding::SyncSource(AudioCaptureMode, AudioInputDevice, AudioCaptureConfig);
}

FString UO3DSenderComponent::GetTransportOption(const FString& Key) const
{
	// A secret is never returned (ADR 0004 item 4).
	return FO3DSenderTransportSettings::GetOption(TransportOptions, GetSelectedTransportName(), Key);
}

void UO3DSenderComponent::SetTransportOption(const FString& Key, const FString& Value)
{
	FO3DSenderTransportSettings::SetOption(TransportOptions, GetSelectedTransportName(), Key, Value, [this]() { RecordChangeForUndo(); });
}

void UO3DSenderComponent::RecordChangeForUndo()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		Modify();
	}
}

FO3DTransportResult UO3DSenderComponent::GetLastTransportResult() const
{
	return TransportController.IsValid() ? TransportController->GetLastResult() : FO3DTransportResult::Ok();
}

FName UO3DSenderComponent::GetSelectedTransportName() const
{
	return TransportName.IsNone() ? DefaultSenderTransportName : TransportName;
}

bool UO3DSenderComponent::IsTransportSecretKey(const FString& Key) const
{
	return FO3DSenderTransportSettings::IsSecretKey(GetSelectedTransportName(), Key);
}

FString UO3DSenderComponent::GetCredentialProfile() const
{
	return FO3DSenderTransportSettings::GetCredentialProfile(TransportOptions, GetSelectedTransportName());
}

void UO3DSenderComponent::SetTransportSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence)
{
	FO3DSenderTransportSettings::SetSecret(TransportOptions, GetSelectedTransportName(), Key, Value, Persistence, [this]() { RecordChangeForUndo(); });
}

bool UO3DSenderComponent::SetTransportSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence)
{
	return FO3DSenderTransportSettings::SetSecretPersistence(TransportOptions, GetSelectedTransportName(), Key, Persistence);
}

void UO3DSenderComponent::ClearTransportSecret(const FString& Key)
{
	FO3DSenderTransportSettings::ClearSecret(TransportOptions, GetSelectedTransportName(), Key);
}

FO3DSecretStatus UO3DSenderComponent::GetTransportSecretStatus(const FString& Key) const
{
	return FO3DSenderTransportSettings::GetSecretStatus(TransportOptions, GetSelectedTransportName(), Key);
}

int32 UO3DSenderComponent::MigrateLegacySecretOptions()
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return 0;
	}

	const TArray<FString> Moved = FO3DSenderTransportSettings::MigrateLegacySecrets(TransportOptions, GetSelectedTransportName());
	if (Moved.Num() > 0)
	{
		// Names the asset and the keys, never a value. Not saved automatically (ADR 0004 item 4).
		const UPackage* Package = GetPackage();
		UE_LOG(LogO3DSenderComponent, Warning,
			TEXT("Moved credential option(s) [%s] of '%s' out of the saved data into this session's secret store. ")
			TEXT("Resave '%s' so the credential is removed from the asset on disk; it is not saved automatically."),
			*FString::Join(Moved, TEXT(", ")), *GetPathName(), Package ? *Package->GetName() : TEXT("<unknown>"));
	}
	return Moved.Num();
}

void UO3DSenderComponent::ClearTransportOptions()
{
	RecordChangeForUndo();
	TransportOptions.Empty();
}

void UO3DSenderComponent::SwitchTransportOptions(FName From, FName To)
{
	if (From == To)
	{
		return;
	}
	RecordChangeForUndo();
	FO3DSenderTransportSettings::SwitchOptions(TransportOptions, InactiveTransportOptions, From, To);
}

void UO3DSenderComponent::SetTransportName(FName InName)
{
	const FName NormalizedName = InName.IsNone() ? DefaultSenderTransportName : InName;
	if (TransportName == NormalizedName)
	{
		return;
	}

	RecordChangeForUndo();

	// SND-35: the outgoing transport's options are kept, and the incoming one's come back.
	const FName Previous = GetSelectedTransportName();
	TransportName = NormalizedName;
	SwitchTransportOptions(Previous, GetSelectedTransportName());
}

void UO3DSenderComponent::EnsureValidTransportName()
{
	if (!TransportName.IsNone())
	{
		return;
	}

	const TArray<FName> RegisteredTransports = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender);
	if (RegisteredTransports.Num() > 0)
	{
		TransportName = RegisteredTransports[0];
	}
	else
	{
		TransportName = DefaultSenderTransportName;
	}
}

void UO3DSenderComponent::BindToTarget()
{
	if (!TargetMesh.IsValid())
	{
		SetTickPrerequisiteMesh(nullptr);
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("No TargetMesh set for sender component on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	EnsureSkeletonCache(TargetMesh.Get());

	// Pose capture runs in TickComponent (TG_PostUpdateWork), after the mesh's own tick
	// (ADR 0008 item 9). No bone-transforms-finalized delegate is used.
	SetTickPrerequisiteMesh(TargetMesh.Get());
}

void UO3DSenderComponent::UnbindFromTarget()
{
	SetTickPrerequisiteMesh(nullptr);
}

void UO3DSenderComponent::SetTickPrerequisiteMesh(USkeletalMeshComponent* Mesh)
{
	// Get(true): a mesh that is being destroyed but has not been collected yet is still found, so its
	// entry is removed the normal way.
	USkeletalMeshComponent* const PreviousMesh = TickPrerequisiteMesh.Get(true);
	if (Mesh != nullptr && PreviousMesh == Mesh)
	{
		return;
	}

	if (PreviousMesh != nullptr)
	{
		RemoveTickPrerequisiteComponent(PreviousMesh);
	}
	else if (TickPrerequisiteFunction != nullptr)
	{
		// The mesh was collected while bound. The tick system already skips a prerequisite whose
		// object is gone, but the entry would stay in the list; remove it so rebinding never
		// accumulates entries.
		const FTickFunction* const StaleFunction = TickPrerequisiteFunction;
		PrimaryComponentTick.GetPrerequisites().RemoveAll([StaleFunction](const FTickPrerequisite& Prerequisite)
		{
			return Prerequisite.PrerequisiteTickFunction == StaleFunction && Prerequisite.PrerequisiteObject.Get(true) == nullptr;
		});
	}
	TickPrerequisiteMesh.Reset();
	TickPrerequisiteFunction = nullptr;

	if (Mesh != nullptr)
	{
		AddTickPrerequisiteComponent(Mesh);
		TickPrerequisiteMesh = Mesh;
		TickPrerequisiteFunction = &Mesh->PrimaryComponentTick;
	}
}

void UO3DSenderComponent::EnsureSubjectNameCached(const USkeletalMeshComponent* SkelComp)
{
	PoseSampler->ResolveSubjectName(SkelComp, SubjectName);
}

void UO3DSenderComponent::HandleSubjectNameChanged(const FString& PreviousName, const FString& NewName)
{
	// SND-16: keep the audio stream label equal to the pose subject name.
	if (AudioCaptureComponent && bEnableAudio)
	{
		AudioCaptureComponent->SetStreamLabel(NewName);
	}

	if (!PreviousName.IsEmpty())
	{
		// Rename (SND-1): the serializer starts the new name with a full sync
		// because it has no state for it; frames carry their own descriptor.
		// Re-broadcast for any other OnDescriptorReady listener.
		PurgeSerializerCacheForSubject(PreviousName);
		if (PoseSampler->GetDescriptor().IsValid())
		{
			OnDescriptorReady.Broadcast(NewName, PoseSampler->GetDescriptor());
		}
	}
}

void UO3DSenderComponent::InvalidateSubjectNameCache()
{
	PurgeSerializerCacheForSubject(PoseSampler->InvalidateSubjectName(SubjectName));
}

void UO3DSenderComponent::PurgeSerializerCacheForSubject(const FString& Subject)
{
	if (Subject.IsEmpty())
	{
		return;
	}

	// In order with the frames already handed to the pipeline (WP-A2c).
	if (Pipeline.IsValid())
	{
		Pipeline->RemoveSubject(Subject);
	}
}

void UO3DSenderComponent::EnsureSkeletonCache(USkeletalMeshComponent* SkelComp)
{
	const bool bDebug = (CVarO3DSenderDebugPose.GetValueOnAnyThread() != 0);
	if (PoseSampler->EnsureSkeleton(SkelComp, SubjectName, bDebug) && CurveProcessor.IsValid())
	{
		CurveProcessor->InvalidateCache();
	}
}

namespace
{
	/** Case-sensitive, like the pattern matching itself (O3DHelpers::NameMatchesPattern). */
	bool SameCurvePatterns(const TArray<FString>& A, const TArray<FString>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Equals(B[Index], ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/** Replaces Snapshot with a new shared copy of Current only when the contents differ. */
	void UpdateSharedCurvePatterns(TSharedPtr<const TArray<FString>>& Snapshot, const TArray<FString>& Current)
	{
		if (!Snapshot.IsValid() || !SameCurvePatterns(*Snapshot, Current))
		{
			Snapshot = MakeShared<TArray<FString>>(Current);
		}
	}
}

/**
 * Snapshot the encoding and curve filtering properties for one frame (SND-14, WP-A2a). The scalar
 * fields are copied every time; a pattern list gets a new shared array only when it changed, so the
 * frames already sampled keep the lists they were sampled with.
 */
const FO3DSenderEncodingSettings& UO3DSenderComponent::UpdateEncodingSnapshot()
{
	FO3DSenderEncodingSettings& Settings = EncodingSnapshot;
	// Residual takes precedence when both are enabled (see bEnableQuantization), and only on a
	// transport that delivers reliably and in order (ADR 0005 (iii)). A change of the effective
	// mode changes the encoding fingerprint, so the next frame is a full sync (SND-14).
	const EO3DDeliveryGuarantee Delivery = bEnableResidualCoding ? GetActiveDeliveryGuarantee() : EO3DDeliveryGuarantee::Unknown;
	Settings.Mode = ResolveEncodingMode(bEnableResidualCoding, bEnableQuantization, Delivery);
	if (bEnableResidualCoding && Settings.Mode != EO3DSenderEncodingMode::Residual)
	{
		WarnResidualFallback(Delivery);
	}
	Settings.ResidualPredictor = ResidualPredictor;
	Settings.ResidualKeyframeIntervalFrames = ResidualKeyframeIntervalFrames;
	Settings.ResidualDeltaThreshold = ResidualDeltaThreshold;
	Settings.QuantizationByteRange = QuantizationByteRange;
	Settings.QuantizationHalfRange = QuantizationHalfRange;
	Settings.QuantizationDeltaThreshold = QuantizationDeltaThreshold;
	Settings.FullSyncIntervalSeconds = FullSyncIntervalSeconds;

	Settings.bClampMorphCurvesToUnit = bClampMorphCurvesToUnit;
	Settings.bDropNaNAndInfinity = bDropNaNAndInfinity;
	Settings.bEnableCurveFiltering = bEnableCurveFiltering;
	// ADR 0005 (ii), SND-3: per-frame epsilon/delta filtering changes which
	// curves a frame carries, which the residual and quantized encodings
	// would have to answer with a full sync every time. Those encodings send
	// every curve value on each update, so the filter is off there; include
	// and exclude patterns still apply.
	Settings.bApplyCurveValueFilters = bEnableCurveFiltering && !bEnableResidualCoding && !bEnableQuantization;
	Settings.CurveEpsilon = CurveEpsilon;
	Settings.CurveDeltaThreshold = CurveDeltaThreshold;
	Settings.bLogFilteredCurves = bLogFilteredCurves;
	UpdateSharedCurvePatterns(Settings.IncludeCurvePatterns, IncludeCurvePatterns);
	UpdateSharedCurvePatterns(Settings.ExcludeCurvePatterns, ExcludeCurvePatterns);
	return Settings;
}

/**
 * Limits capture cadence to the configured rate (SND-5). Accumulator-based with a 0.5 ms tolerance
 * (O3DS::ConsumeCaptureBudget), so a tick rate equal to the capture rate captures every tick despite
 * jitter. InOutLastCaptureTime holds the last capture slot; 0 means "capture now and anchor".
 */
bool UO3DSenderComponent::ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz)
{
	return O3DS::ConsumeCaptureBudget(NowSeconds, InOutLastCaptureTime, (double)CaptureRateHz);
}

/** Validate capture preconditions (transport, target mesh, rate limiting) before emitting a frame. */
bool UO3DSenderComponent::CanCaptureThisFrame(double NowSeconds, USkeletalMeshComponent*& OutMesh)
{
	OutMesh = nullptr;
	if (!bIsCapturing)
	{
		return false;
	}

	USkeletalMeshComponent* SkelComp = TargetMesh.Get();

	// WP-A2b: TargetMesh is BlueprintReadWrite, so it can change, or its mesh be destroyed, without
	// a restart. Keep the tick prerequisite on the mesh that is actually sampled; takes effect from
	// the next frame.
	if (TickPrerequisiteMesh.Get(true) != SkelComp || (SkelComp == nullptr && TickPrerequisiteFunction != nullptr))
	{
		SetTickPrerequisiteMesh(SkelComp);
	}

	if (!SkelComp)
	{
		return false;
	}

	EnsureSkeletonCache(SkelComp);

	if (!ConsumeCaptureBudget(NowSeconds, LastCaptureTime, CaptureRateHz))
	{
		return false;
	}

	OutMesh = SkelComp;
	return true;
}

FString UO3DSenderComponent::ResolveSubjectName(const USkeletalMeshComponent* SkelComp)
{
	return PoseSampler->ResolveSubjectName(SkelComp, SubjectName);
}

void UO3DSenderComponent::FillFrameShell(const USkeletalMeshComponent* SkelComp, double CaptureTimeSec, FO3DSPoseFrame& Frame)
{
	PoseSampler->FillShell(SkelComp, SubjectName, CaptureTimeSec, UpdateEncodingSnapshot(), Frame);
	// RCV-8 (ADR 0013): the engine timecode of this tick, read with the sampling time. Not
	// FApp::GetTimecode(), which returns a default when no timecode provider is synchronized.
	Frame.SceneTime = FApp::GetCurrentFrameTime();
}

void UO3DSenderComponent::PopulatePoseFrameCurves(USkeletalMeshComponent* SkelComp, FO3DSPoseFrame& Frame, bool bDebugCurves)
{
	Frame.CurveList.Reset();
	Frame.RawCurveValues.Reset();
	Frame.CurveNames.Reset();
	Frame.CurveValues.Reset();
	if (!SkelComp)
	{
		return;
	}

	if (!CurveProcessor.IsValid())
	{
		CurveProcessor.Reset(new FO3DSenderCurveProcessor());
	}

	// Capture only: raw values against the shared curve list. Filtering runs on the sampled frame
	// afterwards (FO3DSenderCurveFilter, WP-A2a).
	CurveProcessor->EnsureCurveCache(SkelComp);
	CurveProcessor->CaptureCurves(SkelComp, bDebugCurves, Frame.RawCurveValues);
	Frame.CurveList = CurveProcessor->GetCurveList();
}

TUniquePtr<FO3DSPoseFrame> UO3DSenderComponent::AcquirePoseFrame()
{
	return Pipeline.IsValid() ? Pipeline->AcquireFrame() : TUniquePtr<FO3DSPoseFrame>();
}

void UO3DSenderComponent::DispatchSampledFrame(TUniquePtr<FO3DSPoseFrame>&& Frame)
{
	if (!Frame.IsValid() || !Pipeline.IsValid())
	{
		return;
	}

	if (Pipeline->IsAsync())
	{
		// ADR 0008 item 1: the delegate gets the sampled frame (raw curves); the worker filters,
		// serializes and sends it. The frame is not touched here after it is handed over.
		OnPoseFrameReady.Broadcast(Frame->Subject, *Frame);
		Pipeline->SubmitFrame(MoveTemp(Frame), false);
		return;
	}

	// o3d.Sender.AsyncPipeline 0: the WP-A2b order on the game thread. Filter, then the delegate
	// (filtered curves), then serialize and send inside SubmitFrame.
	Pipeline->FilterFrameInline(*Frame);
	OnPoseFrameReady.Broadcast(Frame->Subject, *Frame);
	Pipeline->SubmitFrame(MoveTemp(Frame), true);
}

/**
 * Samples the skeletal mesh into a pooled frame and hands it to the pose pipeline (ADR 0008 item
 * 1). Only sampling runs here, on the game thread; filtering, serialization and the send run on the
 * pipeline's worker (WP-A2c), or right after this in synchronous mode. Nothing after sampling reads
 * this component: the filter and the serializer work only from the frame and its settings snapshot.
 */
void UO3DSenderComponent::HandleBoneTransformsFinalized()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Sample");
	USkeletalMeshComponent* SkelComp = nullptr;
	const double NowSeconds = FPlatformTime::Seconds();
	if (!CanCaptureThisFrame(NowSeconds, SkelComp))
	{
		return;
	}

	TUniquePtr<FO3DSPoseFrame> Frame = AcquirePoseFrame();
	if (!Frame.IsValid())
	{
		// Every pooled frame is out. The pool holds the queue depth plus two (ADR 0008 item 4) and
		// the queue drops its oldest frame first, so this is not expected; the sample is skipped
		// rather than allocating past the pool's bound.
		UE_LOG(LogO3DSenderComponent, Verbose, TEXT("No free pose frame; sample skipped on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	const bool bDebugPose = (CVarO3DSenderDebugPose.GetValueOnAnyThread() != 0);
	const bool bDebugCurves = (CVarO3DSenderDebugCurves.GetValueOnAnyThread() != 0);

	FillFrameShell(SkelComp, NowSeconds, *Frame);
	PoseSampler->SampleBones(SkelComp, *Frame, bDebugPose);
	PopulatePoseFrameCurves(SkelComp, *Frame, bDebugCurves);

	DispatchSampledFrame(MoveTemp(Frame));
}

/** Called every frame; forwards upkeep ticks to the live transport instance. */
void UO3DSenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Samples the pose: this tick runs in TG_PostUpdateWork, after the target mesh's tick (WP-A2b).
	HandleBoneTransformsFinalized();

	if (TransportController.IsValid())
	{
		TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
		if (SenderInstance.IsValid())
		{
			SenderInstance->Tick(DeltaTime);
		}
	}

	TickControl();
}

// ── Control channel (docs/adr/0011-control-channel.md, item 8) ───────────────────────────

FO3DControlPublisher& UO3DSenderComponent::EnsureControlPublisher()
{
	if (!ControlPublisher.IsValid())
	{
		const AActor* Owner = GetOwner();
		ControlPublisher = MakeUnique<FO3DControlPublisher>(Owner ? Owner->GetName() : GetName());
	}
	return *ControlPublisher;
}

FString UO3DSenderComponent::GetControlSourceId() const
{
	return const_cast<UO3DSenderComponent*>(this)->EnsureControlPublisher().GetSourceId();
}

void UO3DSenderComponent::StartControl()
{
	if (!TransportController.IsValid() || !TransportController->IsActive())
	{
		return;
	}
	const TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
	if (!SenderInstance.IsValid() || !SenderInstance->GetCapabilities().bControl)
	{
		return;
	}
	FO3DControlPublisher& Publisher = EnsureControlPublisher();
	Publisher.SetConfig(ControlSnapshotIntervalSeconds, ControlEventRedundancy, ControlMaxValueRateHz);
	Publisher.Start(); // sends a snapshot of any values set before capture started
}

void UO3DSenderComponent::TickControl()
{
	if (!ControlPublisher.IsValid() || !ControlPublisher->IsRunning() || !TransportController.IsValid())
	{
		return;
	}
	const TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
	if (!SenderInstance.IsValid())
	{
		return;
	}
	// The subject this sender streams, exactly as it goes on the wire, so receivers can align
	// control to its mocap. Empty for a control-only sender.
	TArray<FString> Subjects;
	if (!PoseSampler->GetSubjectName().IsEmpty())
	{
		Subjects.Add(PoseSampler->GetSubjectName());
	}
	ControlPublisher->SetMocapSubjects(Subjects);
	ControlPublisher->SetConfig(ControlSnapshotIntervalSeconds, ControlEventRedundancy, ControlMaxValueRateHz);
	ControlPublisher->Tick(*SenderInstance);
}

bool UO3DSenderComponent::FireControlEvent(const FString& EventName, const FO3DControlValue& Payload, FString TargetSubject)
{
	FString Error;
	if (!EnsureControlPublisher().FireEvent(EventName, TargetSubject, Payload, &Error))
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("FireControlEvent('%s') on %s was not sent: %s"), *EventName, *GetNameSafe(GetOwner()), *Error);
		return false;
	}
	return true;
}

bool UO3DSenderComponent::SetControlValue(const FString& Key, const FO3DControlValue& Value, FString TargetSubject)
{
	FString Error;
	if (!EnsureControlPublisher().SetValue(Key, TargetSubject, Value, &Error))
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("SetControlValue('%s') on %s was refused: %s"), *Key, *GetNameSafe(GetOwner()), *Error);
		return false;
	}
	return true;
}

void UO3DSenderComponent::ClearControlValue(const FString& Key, FString TargetSubject)
{
	EnsureControlPublisher().ClearValue(Key, TargetSubject);
}

void UO3DSenderComponent::ClearAllControlValues()
{
	EnsureControlPublisher().ClearAll();
}

bool UO3DSenderComponent::GetControlValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const
{
	if (!ControlPublisher.IsValid())
	{
		OutValue = FO3DControlValue();
		return false;
	}
	return ControlPublisher->FindValue(Key, TargetSubject, OutValue);
}

void UO3DSenderComponent::UpdateEditConditionHelpers()
{
	// Reserved for future per-transport edit condition logic.
}

#if WITH_EDITOR
void UO3DSenderComponent::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
	// Remembered so PostEditChangeProperty knows whose options TransportOptions holds (SND-35).
	if (PropertyAboutToChange && PropertyAboutToChange->GetFName() == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName))
	{
		TransportNameBeforeEdit = GetSelectedTransportName();
	}
}

void UO3DSenderComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateEditConditionHelpers();

	const FName Prop = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	const bool bInGameWorld = (GetWorld() && GetWorld()->IsGameWorld());
	const bool bWasCapturing = bIsCapturing;

	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName))
	{
		EnsureValidTransportName();
		// SND-35 (WP-A1 PR 5a): the outgoing transport's options are put away, not cleared, inside
		// the property-edit transaction, so undo restores both maps. Without a PreEditChange (an
		// edit that did not come through the property system) there is no outgoing name, and the
		// options are dropped as before.
		SwitchTransportOptions(TransportNameBeforeEdit, GetSelectedTransportName());
		TransportNameBeforeEdit = NAME_None;
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureMode))
	{
		SyncAudioConfigSource();
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig))
	{
		SyncAudioConfigSource();
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioInputDevice))
	{
		AudioCaptureConfig.DeviceIndex = FO3DSenderAudioBinding::ResolveDeviceIndex(AudioInputDevice);
	}

	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName) ||
		Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh))
	{
		InvalidateSubjectNameCache();
	}

	if (FO3DSenderTransportSettings::IsRestartProperty(Prop))
	{
		StopCapture();
		if (bInGameWorld && (bAutoStartCapture || bWasCapturing))
		{
			StartCapture();
		}
	}
}
#endif

#undef LOCTEXT_NAMESPACE
