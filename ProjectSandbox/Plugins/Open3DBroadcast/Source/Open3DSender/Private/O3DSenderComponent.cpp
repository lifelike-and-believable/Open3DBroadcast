// Copyright Lifelike & Believable. All Rights Reserved.

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
#include "UObject/Package.h"
#include "AudioCaptureCore.h"
#include "O3DAudioFrameCodec.h"

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
	if (!TargetMesh.IsValid() && !bEnableAudio)
	{
		LastStartCaptureError = TEXT("No valid TargetMesh and audio is disabled.");
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender capture not started on %s: %s"), *GetNameSafe(GetOwner()), *LastStartCaptureError);
		NotifyOnScreen(FString::Printf(TEXT("O3D Sender: not started (%s)"), *LastStartCaptureError), FColor::Red, 4.0f);
		return;
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
		if (Serializer)
		{
			Serializer->Detach(this);
		}
		UnbindFromTarget();
		ResetSkeletonCache();
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

	if (Serializer)
	{
		Serializer->Detach(this);
		Serializer->ClearAllCaches();
	}

	// SND-1: forget the cached skeleton so the next StartCapture() rebuilds
	// the descriptor (and re-broadcasts OnDescriptorReady) even for the same
	// mesh.
	ResetSkeletonCache();

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

	// Note: tick enablement is driven by StartCapture() based on bIsCapturing, not by transport
	// start success here, so pose capture still runs for externally-managed transports and even
	// when auto-transport creation fails.

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

	const FName SelectedTransport = GetSelectedTransportName();
	Config.Transport = SelectedTransport.ToString();
	Config.Role = TEXT("sender");
	Config.AdvancedParams.Empty();
	Config.Backend.Reset();
	Config.Uri.Reset();
	Config.StreamId.Reset();
	Config.Token.Reset();
	Config.Secrets.Reset();

	// Declared secret keys are never copied into AdvancedParams; they are resolved from the
	// secret store (session, environment, per-user settings) into Config.Secrets (ADR 0004).
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSender::GetTransportSecretDeclaration(SelectedTransport, SecretKeys, SecretEnvVars);
	for (const TPair<FString, FString>& Option : TransportOptions)
	{
		if (!SecretKeys.Contains(Option.Key))
		{
			Config.AdvancedParams.Add(Option.Key, Option.Value);
		}
	}
	FO3DSecretStore::Get().ResolveAll(Config.Transport, GetCredentialProfile(), SecretKeys, SecretEnvVars, Config.Secrets);

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

/** Forwards a frame's already-serialized bytes to both the active transport and this
 *  component's own public delegate. This is the sole per-frame transport dispatch
 *  point (C2, roadmap doc §5/C2): FO3DSenderSerializer decides once - not per-transport -
 *  whether a frame is a full-sync snapshot or a delta/residual update and produces the
 *  final wire bytes itself, so every transport just transmits what it's given via
 *  IOpen3DSender::SendSerialized() rather than re-deriving bytes from a SubjectList
 *  object (see OnSubjectListReady() below, which this supersedes for the normal frame
 *  pipeline - kept in place, just no longer invoked by FO3DSenderSerializer, since a
 *  transport handed a live SubjectList would otherwise call its own Serialize() and
 *  silently discard whichever encoding was actually chosen upstream). */
void UO3DSenderComponent::HandleSerializedFrameForward(const FString& Subject, const TArray<uint8>& Buffer, double Timestamp)
{
	OnSerializedFrame.Broadcast(Subject, Buffer, Timestamp);

	if (!TransportController.IsValid() || !TransportController->IsActive() || Buffer.Num() <= 0)
	{
		return;
	}

	TSharedPtr<IOpen3DSender> SenderInstance = TransportController->GetSender();
	if (!SenderInstance.IsValid())
	{
		return;
	}

	if (!SenderInstance->SendSerialized(Buffer.GetData(), Buffer.Num(), Subject, Timestamp))
	{
		// False can mean backpressure, a connection-state check, or simply
		// an unimplemented SendSerialized() (the interface's default
		// returns false/unsupported) - not backpressure specifically.
		UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Transport '%s' dropped or does not support subject '%s' via SendSerialized()."), *TransportController->GetConfig().Transport, *Subject);
	}
}

void UO3DSenderComponent::OnSubjectListReady(const FString& Subject, const TSharedPtr<O3DS::SubjectList>& Payload)
{
	// Not called by the normal frame pipeline (see HandleSerializedFrameForward's
	// comment above) - retained for any caller that still wants to hand a
	// transport a live SubjectList object directly.
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
			AudioCaptureComponent->SetAudioSink(nullptr, FString());
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

	ConfigureAudioCaptureComponent(CaptureConfig, TransportAudioConfig);

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
	AudioCaptureComponent->SetAudioSink(AudioSink, AudioLabel);

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
	// Audio stream label is automatically derived from SubjectName for logical association on receiver side
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
	// Audio stream label is automatically derived from SubjectName (no longer configurable per component)
	AudioCaptureComponent->StartCaptureWithMode(AudioCaptureMode);
}

void UO3DSenderComponent::TeardownAudioCapture()
{
	if (AudioCaptureComponent)
	{
		AudioCaptureComponent->SetAudioSink(nullptr, FString());
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

	// A secret is never returned (ADR 0004 item 4).
	if (IsTransportSecretKey(Key))
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

	// A declared secret goes to the session store, with no Modify(): it must not dirty or enter the asset.
	if (IsTransportSecretKey(Key))
	{
		SetTransportSecret(Key, Value, EO3DSecretPersistence::Session);
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

FName UO3DSenderComponent::GetSelectedTransportName() const
{
	return TransportName.IsNone() ? DefaultSenderTransportName : TransportName;
}

bool UO3DSenderComponent::IsTransportSecretKey(const FString& Key) const
{
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSender::GetTransportSecretDeclaration(GetSelectedTransportName(), SecretKeys, SecretEnvVars);
	return SecretKeys.Contains(Key);
}

FString UO3DSenderComponent::GetCredentialProfile() const
{
	const FString* Profile = TransportOptions.Find(FO3DSecretStore::MakeCredentialProfileOptionKey(GetSelectedTransportName().ToString()));
	return FO3DSecretStore::NormalizeProfile(Profile ? *Profile : FString());
}

void UO3DSenderComponent::SetTransportSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence)
{
	if (Key.IsEmpty())
	{
		return;
	}

	FO3DSecretStore::Get().Set(GetSelectedTransportName().ToString(), GetCredentialProfile(), Key, Value, Persistence);

	// A copy left in the map by older data must not be saved again.
	if (TransportOptions.Contains(Key))
	{
		if (!HasAnyFlags(RF_ClassDefaultObject))
		{
			Modify();
		}
		TransportOptions.Remove(Key);
	}
}

bool UO3DSenderComponent::SetTransportSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence)
{
	return FO3DSecretStore::Get().SetPersistence(GetSelectedTransportName().ToString(), GetCredentialProfile(), Key, Persistence);
}

void UO3DSenderComponent::ClearTransportSecret(const FString& Key)
{
	FO3DSecretStore::Get().Clear(GetSelectedTransportName().ToString(), GetCredentialProfile(), Key);
}

FO3DSecretStatus UO3DSenderComponent::GetTransportSecretStatus(const FString& Key) const
{
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSender::GetTransportSecretDeclaration(GetSelectedTransportName(), SecretKeys, SecretEnvVars);
	const FString* EnvVar = SecretEnvVars.Find(Key);
	return FO3DSecretStore::Get().Describe(GetSelectedTransportName().ToString(), GetCredentialProfile(), Key, EnvVar ? *EnvVar : FString());
}

int32 UO3DSenderComponent::MigrateLegacySecretOptions()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || TransportOptions.Num() == 0)
	{
		return 0;
	}

	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	if (!O3DSender::GetTransportSecretDeclaration(GetSelectedTransportName(), SecretKeys, SecretEnvVars))
	{
		return 0;
	}

	const FString Transport = GetSelectedTransportName().ToString();
	const FString Profile = GetCredentialProfile();
	FO3DSecretStore& Store = FO3DSecretStore::Get();

	TArray<FString> Moved;
	for (const FString& Key : SecretKeys)
	{
		FString LegacyValue;
		if (!TransportOptions.RemoveAndCopyValue(Key, LegacyValue))
		{
			continue;
		}

		Moved.Add(Key);
		// A value the user already set in this session wins over one found in old data.
		if (!LegacyValue.IsEmpty() && !Store.HasSessionValue(Transport, Profile, Key))
		{
			Store.Set(Transport, Profile, Key, LegacyValue, EO3DSecretPersistence::Session);
		}
	}

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

	// SND-16: keep the audio stream label equal to the pose subject name.
	if (AudioCaptureComponent && bEnableAudio && !PreviousName.Equals(CachedSubjectName, ESearchCase::CaseSensitive))
	{
		AudioCaptureComponent->SetStreamLabel(CachedSubjectName);
	}

	if (!PreviousName.IsEmpty() && !PreviousName.Equals(CachedSubjectName, ESearchCase::CaseSensitive))
	{
		// Rename (SND-1): the serializer starts the new name with a full sync
		// because it has no state for it; frames carry their own descriptor.
		// Re-broadcast for any other OnDescriptorReady listener.
		PurgeSerializerCacheForSubject(PreviousName);
		if (DescriptorCache.IsValid())
		{
			OnDescriptorReady.Broadcast(CachedSubjectName, DescriptorCache);
		}
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
		DescriptorSnapshot.Reset();
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
	DescriptorSnapshot = MakeShared<FO3DSSkeletonDescriptor>(DescriptorCache);

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

void UO3DSenderComponent::ResetSkeletonCache()
{
	CachedSkeletalMesh.Reset();
	CachedSkeleton.Reset();
	CachedSkeletalMeshName = NAME_None;
	DescriptorCache.Reset();
	DescriptorSnapshot.Reset();
	bDescriptorDirty = false;
}

/** Snapshot the encoding properties for one frame (SND-14). */
FO3DSenderEncodingSettings UO3DSenderComponent::BuildEncodingSettings() const
{
	FO3DSenderEncodingSettings Settings;
	// Residual takes precedence when both are enabled (see bEnableQuantization).
	if (bEnableResidualCoding)
	{
		Settings.Mode = EO3DSenderEncodingMode::Residual;
	}
	else if (bEnableQuantization)
	{
		Settings.Mode = EO3DSenderEncodingMode::Quantized;
	}
	else
	{
		Settings.Mode = EO3DSenderEncodingMode::Legacy;
	}
	Settings.ResidualPredictor = ResidualPredictor;
	Settings.ResidualKeyframeIntervalFrames = ResidualKeyframeIntervalFrames;
	Settings.ResidualDeltaThreshold = ResidualDeltaThreshold;
	Settings.QuantizationByteRange = QuantizationByteRange;
	Settings.QuantizationHalfRange = QuantizationHalfRange;
	Settings.QuantizationDeltaThreshold = QuantizationDeltaThreshold;
	Settings.FullSyncIntervalSeconds = FullSyncIntervalSeconds;
	return Settings;
}

/** Build the runtime curve processing configuration from component-level settings. */
FO3DSenderCurveConfig UO3DSenderComponent::BuildCurveConfig() const
{
	FO3DSenderCurveConfig Config;
	Config.bClampMorphCurvesToUnit = bClampMorphCurvesToUnit;
	Config.bDropNaNAndInfinity = bDropNaNAndInfinity;
	Config.bEnableCurveFiltering = bEnableCurveFiltering;
	// ADR 0005 (ii), SND-3: per-frame epsilon/delta filtering changes which
	// curves a frame carries, which the residual and quantized encodings
	// would have to answer with a full sync every time. Those encodings send
	// every curve value on each update, so the filter is off there; include
	// and exclude patterns still apply.
	Config.bApplyValueFilters = bEnableCurveFiltering && !bEnableResidualCoding && !bEnableQuantization;
	Config.CurveEpsilon = CurveEpsilon;
	Config.CurveDeltaThreshold = CurveDeltaThreshold;
	Config.IncludeCurvePatterns = &IncludeCurvePatterns;
	Config.ExcludeCurvePatterns = &ExcludeCurvePatterns;
	Config.bLogFilteredCurves = bLogFilteredCurves;
	return Config;
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

FO3DSPoseFrame UO3DSenderComponent::CreateFrameShell(const USkeletalMeshComponent* SkelComp, double CaptureTimeSec)
{
	FO3DSPoseFrame Frame;
	Frame.Subject = ResolveSubjectName(SkelComp);
	Frame.FrameIndex = ++FrameCounter;
	// ADR 0005 (i): the frame carries the descriptor it was sampled against,
	// so the serializer never depends on having seen OnDescriptorReady.
	Frame.Descriptor = DescriptorSnapshot;
	Frame.CaptureTimeSec = CaptureTimeSec;
	Frame.Encoding = BuildEncodingSettings();
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

	FO3DSPoseFrame Frame = CreateFrameShell(SkelComp, NowSeconds);
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
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig)
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
