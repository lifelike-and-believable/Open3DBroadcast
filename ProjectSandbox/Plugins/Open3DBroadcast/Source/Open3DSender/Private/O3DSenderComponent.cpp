// Copyright (c) Open3DStream Contributors

#include "O3DSenderComponent.h"

#include "O3DHelpers.h"
#include "O3DSenderLogs.h"
#include "O3DSenderRegistry.h"
#include "O3DSenderSerializer.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderCurveProcessor.h"
#include "O3DSenderTransportController.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "AudioCaptureCore.h"
#include "O3DAudioFrameCodec.h"

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

void FO3DSenderCurveProcessorDeleter::operator()(FO3DSenderCurveProcessor* Ptr) const
{
	delete Ptr;
}

UO3DSenderComponent::~UO3DSenderComponent() = default;

UO3DSenderComponent::UO3DSenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetComponentTickEnabled(false);
	EnsureValidTransportName();
	TransportController.Reset(new FO3DSenderTransportController());
	CurveProcessor.Reset(new FO3DSenderCurveProcessor());
	SyncAudioConfigSource();
	LastSubjectSourceValue = SubjectName;
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

	if (Serializer)
	{
		Serializer->Detach(this);
		if (SerializerRelayHandle.IsValid())
		{
			Serializer->OnSerializedFrame.Remove(SerializerRelayHandle);
			SerializerRelayHandle.Reset();
		}
		if (SubjectListHandle.IsValid())
		{
			Serializer->OnSubjectListReady.Remove(SubjectListHandle);
			SubjectListHandle.Reset();
		}
		Serializer->ClearAllCaches();
		Serializer.Reset();
	}

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

	if (!Serializer)
	{
		Serializer = MakeUnique<FO3DSenderSerializer>();
	}

	if (Serializer)
	{
		Serializer->Attach(this);
		if (!SerializerRelayHandle.IsValid())
		{
			SerializerRelayHandle = Serializer->OnSerializedFrame.AddUObject(this, &UO3DSenderComponent::HandleSerializedFrameForward);
		}
		if (!SubjectListHandle.IsValid())
		{
			SubjectListHandle = Serializer->OnSubjectListReady.AddUObject(this, &UO3DSenderComponent::OnSubjectListReady);
		}
	}

	if (CurveProcessor.IsValid())
	{
		CurveProcessor->Reset();
	}

	InitializeTransport();
	UpdateAudioCaptureBinding();

	BindToTarget();
	const bool bHasValidMesh = TargetMesh.IsValid();
	bIsCapturing = bHasValidMesh || bEnableAudio;
	LastCaptureTime = 0.0;
	FrameCounter = 0;

	if (bIsCapturing)
	{
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
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender capture failed to start (no valid skeletal mesh)."));
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

	if (Serializer)
	{
		Serializer->Detach(this);
		Serializer->ClearAllCaches();
	}

	if (CurveProcessor.IsValid())
	{
		CurveProcessor->Reset();
	}

	TeardownTransport();

	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture stopped on %s"), *GetNameSafe(TargetMesh.Get()));
	NotifyOnScreen(FString::Printf(TEXT("O3D Sender: Stopped on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Yellow, 2.0f);
}

/** Stop ticking the active transport and release audio capture bindings. */
void UO3DSenderComponent::TeardownTransport()
{
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

	if (!Serializer)
	{
		return;
	}

	if (!TransportController.IsValid())
	{
		TransportController.Reset(new FO3DSenderTransportController());
	}

	FO3DTransportConfig Config = BuildTransportConfig();
	if (!TransportController->Start(Config))
	{
		return;
	}

	SetComponentTickEnabled(true);

	if (!SubjectListHandle.IsValid())
	{
		SubjectListHandle = Serializer->OnSubjectListReady.AddUObject(this, &UO3DSenderComponent::OnSubjectListReady);
	}
	if (!SerializerRelayHandle.IsValid())
	{
		SerializerRelayHandle = Serializer->OnSerializedFrame.AddUObject(this, &UO3DSenderComponent::HandleSerializedFrameForward);
	}

	UpdateAudioCaptureBinding();
	UE_LOG(LogO3DSenderComponent, Log, TEXT("Auto transport '%s' initialized."), *TransportController->GetConfig().Transport);
}

FO3DTransportConfig UO3DSenderComponent::BuildTransportConfig() const
{
	FO3DTransportConfig Config;
	const FO3DSenderAudioCaptureConfig CaptureConfig = BuildAudioCaptureConfig();

	FName SelectedTransport = TransportName.IsNone() ? DefaultSenderTransportName : TransportName;
	Config.Transport = SelectedTransport.ToString();
	Config.Role = TEXT("sender");
	Config.AdvancedParams.Empty();
	Config.Backend.Reset();
	Config.Uri.Reset();
	Config.StreamId.Reset();
	Config.Token.Reset();
	Config.bPersistToken = false;

	for (const TPair<FString, FString>& Option : TransportOptions)
	{
		Config.AdvancedParams.Add(Option.Key, Option.Value);
	}

	Config.Audio = BuildTransportAudioConfig(CaptureConfig);

	if (!Config.Transport.IsEmpty())
	{
		if (const FO3DSenderTransportCustomization* Customization = O3DSender::FindTransportCustomization(SelectedTransport))
		{
			if (Customization && Customization->ConfigureTransport)
			{
				Customization->ConfigureTransport(this, Config);
			}
		}
	}

	return Config;
}

void UO3DSenderComponent::HandleSerializedFrameForward(const FString& Subject, const TArray<uint8>& Buffer, double Timestamp)
{
	OnSerializedFrame.Broadcast(Subject, Buffer, Timestamp);
}

void UO3DSenderComponent::OnSubjectListReady(const FString& Subject, const TSharedPtr<O3DS::SubjectList>& Payload)
{
	if (!TransportController.IsValid() || !TransportController->IsActive() || !Payload.IsValid())
	{
		return;
	}

	TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
	if (!SenderInstance.IsValid())
	{
		return;
	}

	if (!SenderInstance->Send(*Payload.Get()))
	{
		UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Transport '%s' reported backpressure while sending subject '%s'."), *TransportController->GetConfig().Transport, *Subject);
	}
}

TArray<FName> UO3DSenderComponent::GetAvailableAudioInputDeviceOptions() const
{
	TArray<FName> Options;
	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) > 0)
	{
		for (const Audio::FCaptureDeviceInfo& Info : Devices)
		{
			Options.Add(FName(*Info.DeviceName));
		}
	}
	return Options;
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
			AudioCaptureComponent->SetAudioSink(nullptr, FO3DTransportAudioConfig());
		}
		return;
	}

	EnsureAudioCaptureComponent();
	if (!AudioCaptureComponent)
	{
		return;
	}

	const FO3DSenderAudioCaptureConfig CaptureConfig = BuildAudioCaptureConfig();
	FO3DTransportAudioConfig TransportAudioConfig = BuildTransportAudioConfig(CaptureConfig);
	if (TransportAudioConfig.StreamLabel.IsEmpty())
	{
		TransportAudioConfig.StreamLabel = AudioStreamLabel.IsEmpty() ? TEXT("o3ds:audio") : AudioStreamLabel;
	}

	ConfigureAudioCaptureComponent(CaptureConfig, TransportAudioConfig);

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;
	if (TransportController.IsValid() && TransportController->IsActive())
	{
		AudioSink = TransportController->GetAudioSink();
	}

	AudioCaptureComponent->SetAudioSink(AudioSink, TransportAudioConfig);

	if (!AudioSink.IsValid())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastAudioSinkWarningTime > 2.0)
		{
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Audio capture enabled but no active transport sink (transport=%s)."), *TransportName.ToString());
			LastAudioSinkWarningTime = Now;
		}
	}
	else
	{
		LastAudioSinkWarningTime = 0.0;
	}
}

FO3DSenderAudioCaptureConfig UO3DSenderComponent::BuildAudioCaptureConfig() const
{
	FO3DSenderAudioCaptureConfig ConfigCopy = AudioCaptureConfig;
	ConfigCopy.Source = (AudioCaptureMode == EO3DSenderCaptureMode::Mix)
		? EO3DSenderAudioSource::GameSubmix
		: EO3DSenderAudioSource::Microphone;

	if (AudioCaptureMode == EO3DSenderCaptureMode::Input)
	{
		ConfigCopy.DeviceIndex = ResolveAudioDeviceIndex(AudioInputDevice);
	}

	return ConfigCopy;
}

FO3DTransportAudioConfig UO3DSenderComponent::BuildTransportAudioConfig(const FO3DSenderAudioCaptureConfig& CaptureConfig) const
{
	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.bEnableAudio = bEnableAudio;
	if (!AudioConfig.bEnableAudio)
	{
		return AudioConfig;
	}

	AudioConfig.SampleRate = CaptureConfig.SampleRate;
	AudioConfig.NumChannels = CaptureConfig.NumChannels;
	AudioConfig.BitrateKbps = CaptureConfig.BitrateKbps;
	AudioConfig.Mode = (AudioCaptureMode == EO3DSenderCaptureMode::Mix) ? TEXT("mix") : TEXT("input");
	AudioConfig.StreamLabel = AudioStreamLabel.IsEmpty() ? TEXT("o3ds:audio") : AudioStreamLabel;
	if (AudioCaptureMode == EO3DSenderCaptureMode::Input)
	{
		AudioConfig.InputDevice = AudioInputDevice.IsNone() ? FString() : AudioInputDevice.ToString();
	}
	else
	{
		AudioConfig.InputDevice.Reset();
	}

	const FString CodecString = O3DAudio::SanitizeCodecString(AudioCodec.IsNone() ? FString() : AudioCodec.ToString());
	AudioConfig.AdvancedParams.Empty();
	AudioConfig.AdvancedParams.Add(TEXT("game_gain"), FString::SanitizeFloat(CaptureConfig.GameGain));
	AudioConfig.AdvancedParams.Add(TEXT("mic_gain"), FString::SanitizeFloat(CaptureConfig.MicGain));
	if (CaptureConfig.DeviceIndex >= 0)
	{
		AudioConfig.AdvancedParams.Add(TEXT("device_index"), FString::FromInt(CaptureConfig.DeviceIndex));
	}
	if (CaptureConfig.SubmixToTap)
	{
		AudioConfig.AdvancedParams.Add(TEXT("submix"), CaptureConfig.SubmixToTap->GetPathName());
	}
	if (!CodecString.IsEmpty())
	{
		AudioConfig.Codec = CodecString;
		AudioConfig.AdvancedParams.Add(TEXT("codec"), CodecString);
	}
	else
	{
		AudioConfig.Codec.Reset();
	}

	return AudioConfig;
}

void UO3DSenderComponent::EnsureAudioCaptureComponent()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || !bEnableAudio)
	{
		return;
	}

	if (AudioCaptureComponent)
	{
		if (!IsValid(AudioCaptureComponent) || AudioCaptureComponent->IsBeingDestroyed())
		{
			AudioCaptureComponent = nullptr;
		}
		else
		{
			return;
		}
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (!AudioCaptureComponent)
	{
		AudioCaptureComponent = Owner->FindComponentByClass<UO3DSenderAudioCaptureComponent>();
	}

	if (AudioCaptureComponent && AudioCaptureComponent->GetOwner() != Owner)
	{
		AudioCaptureComponent = nullptr;
	}

	if (!AudioCaptureComponent)
	{
		AudioCaptureComponent = NewObject<UO3DSenderAudioCaptureComponent>(Owner, TEXT("O3DSenderAudioCapture"));
		if (AudioCaptureComponent)
		{
			AudioCaptureComponent->SetFlags(RF_Transactional);
			AudioCaptureComponent->OnComponentCreated();
			AudioCaptureComponent->RegisterComponent();
			Owner->AddInstanceComponent(AudioCaptureComponent);
		}
	}
}

void UO3DSenderComponent::ConfigureAudioCaptureComponent(const FO3DSenderAudioCaptureConfig& CaptureConfig, const FO3DTransportAudioConfig& TransportAudioConfig)
{
	if (!AudioCaptureComponent)
	{
		return;
	}

	AudioCaptureComponent->InputDeviceName = AudioInputDevice;
	AudioCaptureComponent->Config = CaptureConfig;
	AudioCaptureComponent->SetStreamLabel(TransportAudioConfig.StreamLabel.IsEmpty() ? AudioStreamLabel : TransportAudioConfig.StreamLabel);
	AudioCaptureComponent->StartCaptureWithMode(AudioCaptureMode);
}

void UO3DSenderComponent::TeardownAudioCapture()
{
	if (AudioCaptureComponent)
	{
		AudioCaptureComponent->SetAudioSink(nullptr, FO3DTransportAudioConfig());
	}
	LastAudioSinkWarningTime = 0.0;
}

void UO3DSenderComponent::SyncAudioConfigSource()
{
	AudioCaptureConfig.Source = (AudioCaptureMode == EO3DSenderCaptureMode::Mix)
		? EO3DSenderAudioSource::GameSubmix
		: EO3DSenderAudioSource::Microphone;
	if (AudioCaptureMode == EO3DSenderCaptureMode::Input)
	{
		AudioCaptureConfig.DeviceIndex = ResolveAudioDeviceIndex(AudioInputDevice);
	}
}

int32 UO3DSenderComponent::ResolveAudioDeviceIndex(const FName& DeviceName) const
{
	if (DeviceName.IsNone())
	{
		return -1;
	}

	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) > 0)
	{
		for (int32 Index = 0; Index < Devices.Num(); ++Index)
		{
			if (Devices[Index].DeviceName.Equals(DeviceName.ToString(), ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
	}

	return -1;
}

FString UO3DSenderComponent::GetTransportOption(const FString& Key) const
{
	if (Key.IsEmpty())
	{
		return FString();
	}

	if (const FString* Value = TransportOptions.Find(Key))
	{
		return *Value;
	}

	return FString();
}

void UO3DSenderComponent::SetTransportOption(const FString& Key, const FString& Value)
{
	if (Key.IsEmpty())
	{
		return;
	}

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		Modify();
	}

	if (Value.IsEmpty())
	{
		TransportOptions.Remove(Key);
	}
	else
	{
		TransportOptions.Add(Key, Value);
	}
}

void UO3DSenderComponent::ClearTransportOptions()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		Modify();
	}
	TransportOptions.Empty();
}

void UO3DSenderComponent::SetTransportName(FName InName)
{
	const FName NormalizedName = InName.IsNone() ? DefaultSenderTransportName : InName;
	if (TransportName == NormalizedName)
	{
		return;
	}

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		Modify();
	}

	TransportName = NormalizedName;
	ClearTransportOptions();
}

void UO3DSenderComponent::EnsureValidTransportName()
{
	if (!TransportName.IsNone())
	{
		return;
	}

	TArray<FName> RegisteredTransports;
	O3DSender::GetRegisteredTransportNames(RegisteredTransports);
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
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("No TargetMesh set for sender component on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	EnsureSkeletonCache(TargetMesh.Get());

	// Note: In UE 5.4+, RegisterOnBoneTransformsFinalizedDelegate was removed.
	// Pose updates are now handled in TickComponent instead.
}

void UO3DSenderComponent::UnbindFromTarget()
{
	// Note: Delegate unbinding no longer needed in UE 5.4+
}

FString UO3DSenderComponent::BuildSubjectName(const USkeletalMeshComponent* SkelComp) const
{
	if (!SubjectName.IsEmpty())
	{
		return SanitizeSubjectName(SubjectName);
	}

	const UWorld* World = SkelComp ? SkelComp->GetWorld() : nullptr;
	const FString WorldName = World ? World->GetName() : TEXT("World");
	const FString ActorName = SkelComp && SkelComp->GetOwner() ? SkelComp->GetOwner()->GetName() : TEXT("Actor");
	const FString CompName = SkelComp ? SkelComp->GetName() : TEXT("SkeletalMeshComponent");
	return SanitizeSubjectName(FString::Printf(TEXT("%s/%s/%s"), *WorldName, *ActorName, *CompName));
}

FString UO3DSenderComponent::SanitizeSubjectName(const FString& Raw) const
{
	return O3DHelpers::SanitizeSubjectName(Raw);
}

void UO3DSenderComponent::EnsureSubjectNameCached(const USkeletalMeshComponent* SkelComp)
{
	USkeletalMesh* Mesh = SkelComp ? SkelComp->GetSkeletalMeshAsset() : nullptr;
	const bool bSubjectOverrideChanged = (LastSubjectSourceValue != SubjectName);
	const bool bMeshChanged = CachedSubjectMeshForName.Get() != Mesh;

	if (!bSubjectOverrideChanged && !bMeshChanged && !CachedSubjectName.IsEmpty())
	{
		return;
	}

	const FString PreviousName = CachedSubjectName;
	CachedSubjectName = BuildSubjectName(SkelComp);
	CachedSubjectMeshForName = Mesh;
	LastSubjectSourceValue = SubjectName;

	if (!PreviousName.IsEmpty() && !PreviousName.Equals(CachedSubjectName, ESearchCase::CaseSensitive))
	{
		PurgeSerializerCacheForSubject(PreviousName);
	}
}

void UO3DSenderComponent::InvalidateSubjectNameCache()
{
	if (!CachedSubjectName.IsEmpty())
	{
		PurgeSerializerCacheForSubject(CachedSubjectName);
	}
	CachedSubjectName.Reset();
	CachedSubjectMeshForName.Reset();
	LastSubjectSourceValue = SubjectName;
}

void UO3DSenderComponent::PurgeSerializerCacheForSubject(const FString& Subject)
{
	if (Subject.IsEmpty())
	{
		return;
	}

	if (Serializer)
	{
		Serializer->RemoveSubjectCache(Subject);
	}
}

uint64 UO3DSenderComponent::ComputeDescriptorHash(const TArray<FName>& InNames, const TArray<int32>& InParents) const
{
	return O3DHelpers::HashNamesAndParents(InNames, InParents);
}

void UO3DSenderComponent::EnsureSkeletonCache(USkeletalMeshComponent* SkelComp)
{
	if (!SkelComp)
	{
		return;
	}

	USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
	USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
	const FName CurrentMeshName = Mesh ? Mesh->GetFName() : NAME_None;

	if (CachedSkeletalMesh.Get() != Mesh || CachedSkeleton.Get() != Skeleton || CachedSkeletalMeshName != CurrentMeshName)
	{
		RefreshSkeletonCache(SkelComp);
		if (CurveProcessor.IsValid())
		{
			CurveProcessor->InvalidateCache();
		}
	}
}

void UO3DSenderComponent::RefreshSkeletonCache(USkeletalMeshComponent* SkelComp)
{
	USkeletalMesh* Mesh = SkelComp ? SkelComp->GetSkeletalMeshAsset() : nullptr;
	USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
	CachedSkeletalMesh = Mesh;
	CachedSkeleton = Skeleton;
	CachedSkeletalMeshName = Mesh ? Mesh->GetFName() : NAME_None;

	if (!Mesh)
	{
		DescriptorCache.Reset();
		bDescriptorDirty = true;
		return;
	}

	const uint64 PreviousHash = DescriptorCache.Hash;
	const int32 PreviousCount = DescriptorCache.BoneNames.Num();

	DescriptorCache.BoneNames.Reset();
	DescriptorCache.ParentIndices.Reset();

	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	DescriptorCache.BoneNames.Reserve(NumBones);
	DescriptorCache.ParentIndices.Reserve(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		DescriptorCache.BoneNames.Add(RefSkel.GetBoneName(BoneIndex));
		DescriptorCache.ParentIndices.Add(RefSkel.GetParentIndex(BoneIndex));
	}

	const uint64 NewHash = ComputeDescriptorHash(DescriptorCache.BoneNames, DescriptorCache.ParentIndices);
	const bool bChanged = (PreviousHash != NewHash) || (PreviousCount != DescriptorCache.BoneNames.Num());
	DescriptorCache.Hash = NewHash;
	bDescriptorDirty = bChanged;

	const bool bDebug = (CVarO3DSenderDebugPose.GetValueOnAnyThread() != 0);
	if (bDebug)
	{
		UE_LOG(LogO3DSenderComponent, Log, TEXT("Cached skeleton for %s: %d bones, Hash=0x%llx%s"),
			*GetNameSafe(SkelComp), NumBones, (unsigned long long)DescriptorCache.Hash, bDescriptorDirty ? TEXT(" [Changed]") : TEXT(""));
	}

	if (bDescriptorDirty)
	{
		const FString Subject = BuildSubjectName(SkelComp);
		OnDescriptorReady.Broadcast(Subject, DescriptorCache);
		bDescriptorDirty = false;
	}
}

/** Build the runtime curve processing configuration from component-level settings. */
FO3DSenderCurveConfig UO3DSenderComponent::BuildCurveConfig() const
{
	FO3DSenderCurveConfig Config;
	Config.bClampMorphCurvesToUnit = bClampMorphCurvesToUnit;
	Config.bDropNaNAndInfinity = bDropNaNAndInfinity;
	Config.bEnableCurveFiltering = bEnableCurveFiltering;
	Config.CurveEpsilon = CurveEpsilon;
	Config.CurveDeltaThreshold = CurveDeltaThreshold;
	Config.IncludeCurvePatterns = &IncludeCurvePatterns;
	Config.ExcludeCurvePatterns = &ExcludeCurvePatterns;
	Config.bLogFilteredCurves = bLogFilteredCurves;
	return Config;
}

/** Limits capture cadence to the configured rate while preserving first-frame responsiveness. */
bool UO3DSenderComponent::ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz)
{
	if (CaptureRateHz <= 0.0f)
	{
		InOutLastCaptureTime = NowSeconds;
		return true;
	}

	if (InOutLastCaptureTime <= 0.0)
	{
		InOutLastCaptureTime = NowSeconds;
		return true;
	}

	const double ClampedRate = FMath::Max(1e-6f, CaptureRateHz);
	const double MinDelta = 1.0 / ClampedRate;
	if ((NowSeconds - InOutLastCaptureTime) < MinDelta)
	{
		return false;
	}

	InOutLastCaptureTime = NowSeconds;
	return true;
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
	EnsureSubjectNameCached(SkelComp);
	return CachedSubjectName;
}

FO3DSPoseFrame UO3DSenderComponent::CreateFrameShell(const USkeletalMeshComponent* SkelComp)
{
	FO3DSPoseFrame Frame;
	Frame.Subject = ResolveSubjectName(SkelComp);
	Frame.FrameIndex = ++FrameCounter;
	return Frame;
}

void UO3DSenderComponent::BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
	const TArray<int32>& CachedParentIndices,
	int32 NumBones,
	TFunctionRef<int32(int32)> ResolveFallbackParent,
	TArray<FTransform>& OutLocalTransforms,
	TArray<int32>* OutResolvedParents)
{
	if (NumBones <= 0)
	{
		OutLocalTransforms.Reset();
		if (OutResolvedParents)
		{
			OutResolvedParents->Reset();
		}
		return;
	}

	OutLocalTransforms.SetNum(NumBones, EAllowShrinking::No);
	if (OutResolvedParents)
	{
		OutResolvedParents->SetNum(NumBones, EAllowShrinking::No);
	}

	const int32 TransformCount = ComponentSpaceTransforms.Num();

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		int32 ParentIndex = (BoneIndex >= 0 && BoneIndex < CachedParentIndices.Num()) ? CachedParentIndices[BoneIndex] : INDEX_NONE;
		if (ParentIndex < 0 || ParentIndex >= TransformCount)
		{
			ParentIndex = ResolveFallbackParent(BoneIndex);
		}
		if (ParentIndex < 0 || ParentIndex >= TransformCount)
		{
			ParentIndex = INDEX_NONE;
		}

		if (OutResolvedParents)
		{
			(*OutResolvedParents)[BoneIndex] = ParentIndex;
		}

		const FTransform& ComponentTransform = ComponentSpaceTransforms[BoneIndex];
		FTransform Relative = ComponentTransform;
		if (ParentIndex != INDEX_NONE)
		{
			Relative = ComponentTransform.GetRelativeTransform(ComponentSpaceTransforms[ParentIndex]);
		}

		FQuat Rotation = Relative.GetRotation();
		if (!Rotation.IsNormalized())
		{
			Rotation.Normalize();
			Relative.SetRotation(Rotation);
		}

		OutLocalTransforms[BoneIndex] = Relative;
	}
}

void UO3DSenderComponent::PopulatePoseFrameBones(const USkeletalMeshComponent* SkelComp, FO3DSPoseFrame& Frame, bool bDebugPose)
{
	if (!SkelComp)
	{
		Frame.BoneLocalTransforms.Reset();
		return;
	}

	const TArray<FTransform>& ComponentSpace = SkelComp->GetComponentSpaceTransforms();
	const TArray<FName>& CachedBoneNames = DescriptorCache.BoneNames;
	const TArray<int32>& CachedParentIndices = DescriptorCache.ParentIndices;
	const int32 NumBones = FMath::Min(ComponentSpace.Num(), CachedBoneNames.Num());
	if (NumBones <= 0)
	{
		Frame.BoneLocalTransforms.Reset();
		return;
	}

	TArray<int32> ResolvedParents;
	TArray<int32>* ResolvedParentsPtr = nullptr;
	if (bDebugPose)
	{
		ResolvedParentsPtr = &ResolvedParents;
	}

	const auto ResolveFallbackParent = [&](int32 BoneIndex) -> int32
	{
		if (!SkelComp)
		{
			return INDEX_NONE;
		}
		if (!CachedBoneNames.IsValidIndex(BoneIndex))
		{
			return INDEX_NONE;
		}
		const FName BoneName = CachedBoneNames[BoneIndex];
		if (BoneName == NAME_None)
		{
			return INDEX_NONE;
		}
		const FName ParentBoneName = SkelComp->GetParentBone(BoneName);
		if (ParentBoneName == NAME_None)
		{
			return INDEX_NONE;
		}
		return SkelComp->GetBoneIndex(ParentBoneName);
	};

	BuildLocalBoneTransforms(ComponentSpace, CachedParentIndices, NumBones, ResolveFallbackParent, Frame.BoneLocalTransforms, ResolvedParentsPtr);

	if (bDebugPose)
	{
		const int32 DebugCount = FMath::Min(NumBones, 5);
		for (int32 BoneIndex = 0; BoneIndex < DebugCount; ++BoneIndex)
		{
			const int32 ParentIndex = ResolvedParentsPtr ? (*ResolvedParentsPtr)[BoneIndex] : INDEX_NONE;
			const FTransform& Relative = Frame.BoneLocalTransforms[BoneIndex];
			const FVector Translation = Relative.GetTranslation();
			const FVector Scale = Relative.GetScale3D();
			const FName BoneName = CachedBoneNames.IsValidIndex(BoneIndex) ? CachedBoneNames[BoneIndex] : NAME_None;
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("[%d] %s Parent=%d Pos(%.2f,%.2f,%.2f) Scale(%.2f,%.2f,%.2f)"),
				BoneIndex,
				*BoneName.ToString(),
				ParentIndex,
				Translation.X, Translation.Y, Translation.Z,
				Scale.X, Scale.Y, Scale.Z);
		}
	}
}

void UO3DSenderComponent::PopulatePoseFrameCurves(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& CurveConfig, FO3DSPoseFrame& Frame, bool bDebugCurves)
{
	if (!SkelComp)
	{
		Frame.CurveNames.Reset();
		Frame.CurveValues.Reset();
		return;
	}

	if (!CurveProcessor.IsValid())
	{
		CurveProcessor.Reset(new FO3DSenderCurveProcessor());
	}

	CurveProcessor->EnsureCurveCache(SkelComp, CurveConfig);
	CurveProcessor->CaptureCurves(SkelComp, bDebugCurves);

	TArray<FName> FilteredCurveNames;
	TArray<float> FilteredCurveValues;
	CurveProcessor->BuildFilteredCurves(CurveConfig, FilteredCurveNames, FilteredCurveValues);

	Frame.CurveNames = MoveTemp(FilteredCurveNames);
	Frame.CurveValues = MoveTemp(FilteredCurveValues);
}

/** Callback after animation updates; samples the skeletal mesh and pushes serializer events. */
void UO3DSenderComponent::HandleBoneTransformsFinalized()
{
	USkeletalMeshComponent* SkelComp = nullptr;
	const double NowSeconds = FPlatformTime::Seconds();
	if (!CanCaptureThisFrame(NowSeconds, SkelComp))
	{
		return;
	}

	const bool bDebugPose = (CVarO3DSenderDebugPose.GetValueOnAnyThread() != 0);
	const bool bDebugCurves = (CVarO3DSenderDebugCurves.GetValueOnAnyThread() != 0);

	FO3DSPoseFrame Frame = CreateFrameShell(SkelComp);
	PopulatePoseFrameBones(SkelComp, Frame, bDebugPose);

	const FO3DSenderCurveConfig CurveConfig = BuildCurveConfig();
	PopulatePoseFrameCurves(SkelComp, CurveConfig, Frame, bDebugCurves);

	OnPoseFrameReady.Broadcast(Frame.Subject, Frame);
}

/** Called every frame; forwards upkeep ticks to the live transport instance. */
void UO3DSenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// In UE 5.4+, capture bone transforms in tick instead of via deprecated callback
	HandleBoneTransformsFinalized();

	if (TransportController.IsValid())
	{
		TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
		if (SenderInstance.IsValid())
		{
			SenderInstance->Tick(DeltaTime);
		}
	}
}

void UO3DSenderComponent::UpdateEditConditionHelpers()
{
	// Reserved for future per-transport edit condition logic.
}

#if WITH_EDITOR
void UO3DSenderComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateEditConditionHelpers();

	const FName Prop = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	static const TSet<FName> RestartProps = {
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CaptureRateHz),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoCreateTransport),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bEnableAudio),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureMode),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioInputDevice),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioStreamLabel)
	};

	const bool bInGameWorld = (GetWorld() && GetWorld()->IsGameWorld());
	const bool bWasCapturing = bIsCapturing;

	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName))
	{
		EnsureValidTransportName();
		ClearTransportOptions();
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
		AudioCaptureConfig.DeviceIndex = ResolveAudioDeviceIndex(AudioInputDevice);
	}

	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName) ||
		Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh))
	{
		InvalidateSubjectNameCache();
	}

	if (RestartProps.Contains(Prop))
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
