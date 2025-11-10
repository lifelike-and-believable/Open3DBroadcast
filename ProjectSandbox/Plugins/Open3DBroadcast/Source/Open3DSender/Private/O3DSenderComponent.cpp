// Copyright (c) Open3DStream Contributors

#include "O3DSenderComponent.h"

#include "O3DHelpers.h"
#include "O3DSenderLogs.h"
#include "O3DSenderRegistry.h"
#include "O3DSenderSerializer.h"
#include "O3DSenderTransportCustomization.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

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

UO3DSenderComponent::UO3DSenderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetComponentTickEnabled(false);
	EnsureValidTransportName();
}

void UO3DSenderComponent::BeginPlay()
{
	Super::BeginPlay();

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
		Serializer.Reset();
	}

	TeardownTransport();

	Super::EndPlay(EndPlayReason);
}

void UO3DSenderComponent::OnRegister()
{
	Super::OnRegister();
	EnsureValidTransportName();
	UpdateEditConditionHelpers();
}

void UO3DSenderComponent::PostInitProperties()
{
	Super::PostInitProperties();
	EnsureValidTransportName();
	UpdateEditConditionHelpers();
}

#if WITH_EDITORONLY_DATA
void UO3DSenderComponent::PostLoad()
{
	Super::PostLoad();
	EnsureValidTransportName();
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

	InitializeTransport();

	BindToTarget();
	bIsCapturing = TargetMesh.IsValid();
	LastCaptureTime = 0.0;
	FrameCounter = 0;

	if (bIsCapturing)
	{
		UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture started on %s"), *GetNameSafe(TargetMesh.Get()));
		NotifyOnScreen(FString::Printf(TEXT("O3D Sender: Started on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Green, 2.0f);
	}
	else
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender capture failed to start (no valid skeletal mesh)."));
	}
}

void UO3DSenderComponent::StopCapture()
{
	if (!bIsCapturing)
	{
		return;
	}

	UnbindFromTarget();
	bIsCapturing = false;

	if (Serializer)
	{
		Serializer->Detach(this);
	}

	TeardownTransport();

	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture stopped on %s"), *GetNameSafe(TargetMesh.Get()));
	NotifyOnScreen(FString::Printf(TEXT("O3D Sender: Stopped on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Yellow, 2.0f);
}

void UO3DSenderComponent::TeardownTransport()
{
	if (ActiveSender.IsValid())
	{
		ActiveSender->Stop();
		ActiveSender.Reset();
	}

	SetComponentTickEnabled(false);
}

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

	ActiveConfig = BuildTransportConfig();
	if (ActiveConfig.Transport.IsEmpty())
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("No transport specified; skipping auto transport setup."));
		return;
	}

	const FName SelectedTransportName(*ActiveConfig.Transport);
	ActiveSender = O3DTransport::CreateSender(SelectedTransportName);
	if (!ActiveSender.IsValid())
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("No sender registered for transport '%s'."), *ActiveConfig.Transport);
		return;
	}

	if (!ActiveSender->Initialize(ActiveConfig))
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to initialize sender transport '%s'."), *ActiveConfig.Transport);
		ActiveSender.Reset();
		return;
	}

	if (!ActiveSender->Start())
	{
		UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to start sender transport '%s'."), *ActiveConfig.Transport);
		ActiveSender.Reset();
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

	UE_LOG(LogO3DSenderComponent, Log, TEXT("Auto transport '%s' initialized."), *ActiveConfig.Transport);
}

FO3DTransportConfig UO3DSenderComponent::BuildTransportConfig() const
{
	FO3DTransportConfig Config;

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
	if (!ActiveSender.IsValid() || !Payload.IsValid())
	{
		return;
	}

	if (!ActiveSender->Send(*Payload.Get()))
	{
		UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Transport '%s' reported backpressure while sending subject '%s'."), *ActiveConfig.Transport, *Subject);
	}
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

	if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
	{
		if (!BoneTransformsFinalizedHandle.IsValid())
		{
			BoneTransformsFinalizedHandle = Skinned->RegisterOnBoneTransformsFinalizedDelegate(
				FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UO3DSenderComponent::HandleBoneTransformsFinalized));
		}
	}
}

void UO3DSenderComponent::UnbindFromTarget()
{
	if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
	{
		if (BoneTransformsFinalizedHandle.IsValid())
		{
			Skinned->UnregisterOnBoneTransformsFinalizedDelegate(BoneTransformsFinalizedHandle);
			BoneTransformsFinalizedHandle.Reset();
		}
	}
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

	if (CachedSkeletalMesh.Get() != Mesh || CachedSkeleton.Get() != Skeleton)
	{
		RefreshSkeletonCache(SkelComp);
		bCurveCacheInitialized = false;
	}
}

void UO3DSenderComponent::RefreshSkeletonCache(USkeletalMeshComponent* SkelComp)
{
	BoneNames.Reset();
	ParentIndices.Reset();

	USkeletalMesh* Mesh = SkelComp ? SkelComp->GetSkeletalMeshAsset() : nullptr;
	USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
	CachedSkeletalMesh = Mesh;
	CachedSkeleton = Skeleton;

	if (!Mesh)
	{
		return;
	}

	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	BoneNames.Reserve(NumBones);
	ParentIndices.Reserve(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		BoneNames.Add(RefSkel.GetBoneName(BoneIndex));
		ParentIndices.Add(RefSkel.GetParentIndex(BoneIndex));
	}

	const uint64 NewHash = ComputeDescriptorHash(BoneNames, ParentIndices);
	const bool bChanged = !DescriptorCache.IsValid() || DescriptorCache.BoneNames.Num() != BoneNames.Num() || DescriptorCache.Hash != NewHash;

	DescriptorCache.BoneNames = BoneNames;
	DescriptorCache.ParentIndices = ParentIndices;
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

void UO3DSenderComponent::EnsureCurveCache(USkeletalMeshComponent* SkelComp)
{
	if (!bCurveCacheInitialized)
	{
		RefreshCurveCache(SkelComp);
	}
}

void UO3DSenderComponent::RefreshCurveCache(USkeletalMeshComponent* SkelComp)
{
	CurveNames.Reset();
	CurveValues.Reset();
	LastSentCurveValues.Reset();
	LastSentHasValue.Reset();
	MorphNameSet.Reset();
	CurveNameSet.Reset();

	if (!SkelComp)
	{
		bCurveCacheInitialized = true;
		return;
	}

	USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
	USkeleton* Skeleton = SkelMesh ? SkelMesh->GetSkeleton() : nullptr;

	if (SkelMesh)
	{
		const TArray<UMorphTarget*>& Morphs = SkelMesh->GetMorphTargets();
		CurveNames.Reserve(Morphs.Num());
		for (UMorphTarget* Morph : Morphs)
		{
			if (!Morph)
			{
				continue;
			}
			const FName Name = Morph->GetFName();
			if (!CurveNameSet.Contains(Name))
			{
				MorphNameSet.Add(Name);
				CurveNames.Add(Name);
				CurveNameSet.Add(Name);
			}
		}
	}

	if (Skeleton)
	{
		TArray<FName> SkeletonCurveNames;
		Skeleton->GetCurveMetaDataNames(SkeletonCurveNames);
		for (const FName& CurveName : SkeletonCurveNames)
		{
			if (CurveName != NAME_None && !CurveNameSet.Contains(CurveName))
			{
				CurveNames.Add(CurveName);
				CurveNameSet.Add(CurveName);
			}
		}
	}

	if (IncludeCurvePatterns.Num() > 0 || ExcludeCurvePatterns.Num() > 0)
	{
		TArray<FName> Filtered;
		Filtered.Reserve(CurveNames.Num());
		for (const FName& Name : CurveNames)
		{
			if (IsCurveAllowedByPatterns(Name))
			{
				Filtered.Add(Name);
			}
		}
		CurveNames = MoveTemp(Filtered);
		CurveNameSet.Reset();
		for (const FName& Name : CurveNames)
		{
			CurveNameSet.Add(Name);
		}
	}

	CurveNames.Sort([](const FName& A, const FName& B)
	{
		return FCString::Strcmp(*A.ToString(), *B.ToString()) < 0;
	});

	CurveValues.SetNumZeroed(CurveNames.Num());
	LastSentCurveValues.SetNumZeroed(CurveNames.Num());
	LastSentHasValue.SetNumZeroed(CurveNames.Num());
	bCurveCacheInitialized = true;
}

void UO3DSenderComponent::CaptureCurves(USkeletalMeshComponent* SkelComp)
{
	EnsureCurveCache(SkelComp);
	if (!SkelComp)
	{
		return;
	}

	const bool bDebugCurves = (CVarO3DSenderDebugCurves.GetValueOnAnyThread() != 0);

	for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
	{
		CurveValues[Index] = 0.0f;
	}

	const TMap<FName, float>& MorphOverrides = SkelComp->GetMorphTargetCurves();

	for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
	{
		const FName& Name = CurveNames[Index];
		float Value = 0.0f;

		if (MorphNameSet.Contains(Name))
		{
			if (const float* Override = MorphOverrides.Find(Name))
			{
				CurveValues[Index] = *Override;
				continue;
			}
		}

		float OutValue = 0.0f;
		if (SkelComp->GetCurveValue(Name, 0.0f, OutValue))
		{
			Value = OutValue;
		}
		else if (MorphNameSet.Contains(Name))
		{
			Value = SkelComp->GetMorphTarget(Name);
		}

		CurveValues[Index] = Value;

		if (bDebugCurves && Index < 5)
		{
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Curve[%d] %s = %.4f"), Index, *Name.ToString(), Value);
		}
	}
}

bool UO3DSenderComponent::NameMatchesPattern(const FString& Text, const FString& Pattern) const
{
	return O3DHelpers::NameMatchesPattern(Text, Pattern);
}

bool UO3DSenderComponent::IsCurveAllowedByPatterns(const FName& Name) const
{
	const FString Text = Name.ToString();
	for (const FString& Pattern : ExcludeCurvePatterns)
	{
		if (!Pattern.IsEmpty() && NameMatchesPattern(Text, Pattern))
		{
			return false;
		}
	}

	if (IncludeCurvePatterns.Num() == 0)
	{
		return true;
	}

	for (const FString& Pattern : IncludeCurvePatterns)
	{
		if (!Pattern.IsEmpty() && NameMatchesPattern(Text, Pattern))
		{
			return true;
		}
	}

	return false;
}

void UO3DSenderComponent::BuildFilteredCurves(TArray<FName>& OutNames, TArray<float>& OutValues)
{
	OutNames.Reset();
	OutValues.Reset();

	OutNames.Reserve(CurveNames.Num());
	OutValues.Reserve(CurveNames.Num());

	for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
	{
		const FName& Name = CurveNames[Index];
		float Value = CurveValues[Index];

		if (bDropNaNAndInfinity && !FMath::IsFinite(Value))
		{
			if (bLogFilteredCurves)
			{
				UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Dropped curve %s (NaN/Inf)"), *Name.ToString());
			}
			continue;
		}

		if (bClampMorphCurvesToUnit && MorphNameSet.Contains(Name))
		{
			Value = FMath::Clamp(Value, 0.0f, 1.0f);
		}

		if (bEnableCurveFiltering)
		{
			if (!IsCurveAllowedByPatterns(Name))
			{
				if (bLogFilteredCurves)
				{
					UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (pattern)"), *Name.ToString());
				}
				continue;
			}

			if (FMath::Abs(Value) < CurveEpsilon)
			{
				if (bLogFilteredCurves)
				{
					UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (epsilon %.6f) V=%.6f"), *Name.ToString(), CurveEpsilon, Value);
				}
				continue;
			}

			const bool bHasLast = LastSentHasValue.IsValidIndex(Index) ? (LastSentHasValue[Index] != 0) : false;
			if (bHasLast)
			{
				const float Last = LastSentCurveValues[Index];
				if (FMath::Abs(Value - Last) < CurveDeltaThreshold)
				{
					if (bLogFilteredCurves)
					{
						UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (delta %.6f < %.6f) V=%.6f Last=%.6f"), *Name.ToString(), FMath::Abs(Value - Last), CurveDeltaThreshold, Value, Last);
					}
					continue;
				}
			}
		}

		OutNames.Add(Name);
		OutValues.Add(Value);

		if (LastSentCurveValues.IsValidIndex(Index))
		{
			LastSentCurveValues[Index] = Value;
			LastSentHasValue[Index] = 1;
		}
	}
}

void UO3DSenderComponent::HandleBoneTransformsFinalized()
{
	if (!bIsCapturing)
	{
		return;
	}

	USkeletalMeshComponent* SkelComp = TargetMesh.Get();
	if (!SkelComp)
	{
		return;
	}

	EnsureSkeletonCache(SkelComp);

	if (CaptureRateHz > 0.0f)
	{
		const double Now = FPlatformTime::Seconds();
		const double MinDelta = 1.0 / FMath::Max(1.0f, CaptureRateHz);
		if ((Now - LastCaptureTime) < MinDelta)
		{
			return;
		}
		LastCaptureTime = Now;
	}

	const TArray<FTransform>& CompSpace = SkelComp->GetComponentSpaceTransforms();
	const int32 Count = CompSpace.Num();
	const int32 NumBones = FMath::Min(Count, BoneNames.Num());
	const FString Subject = BuildSubjectName(SkelComp);

	const bool bDebug = (CVarO3DSenderDebugPose.GetValueOnAnyThread() != 0);

	FO3DSPoseFrame Frame;
	Frame.Subject = Subject;
	Frame.FrameIndex = ++FrameCounter;
	Frame.BoneLocalTransforms.SetNumUninitialized(NumBones);

	auto GetEffectiveParentIndex = [&](int32 BoneIdx) -> int32
	{
		if (BoneIdx >= 0 && BoneIdx < ParentIndices.Num())
		{
			const int32 Parent = ParentIndices[BoneIdx];
			if (Parent >= 0 && Parent < Count)
			{
				return Parent;
			}
		}

		const FName BoneName = (BoneIdx >= 0 && BoneIdx < BoneNames.Num()) ? BoneNames[BoneIdx] : NAME_None;
		if (BoneName == NAME_None)
		{
			return INDEX_NONE;
		}

		const FName ParentName = SkelComp->GetParentBone(BoneName);
		if (ParentName == NAME_None)
		{
			return INDEX_NONE;
		}

		const int32 ParentByName = SkelComp->GetBoneIndex(ParentName);
		return (ParentByName >= 0 && ParentByName < Count) ? ParentByName : INDEX_NONE;
	};

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const int32 ParentIndex = GetEffectiveParentIndex(BoneIndex);
		const FTransform& ThisCS = CompSpace[BoneIndex];
		FTransform Relative;
		if (ParentIndex >= 0 && ParentIndex < CompSpace.Num())
		{
			Relative = ThisCS.GetRelativeTransform(CompSpace[ParentIndex]);
		}
		else
		{
			Relative = ThisCS;
		}

		FQuat Rotation = Relative.GetRotation();
		Rotation.Normalize();
		Relative.SetRotation(Rotation);
		Frame.BoneLocalTransforms[BoneIndex] = Relative;

		if (bDebug && BoneIndex < 5)
		{
			const FVector Translation = Relative.GetTranslation();
			const FVector Scale = Relative.GetScale3D();
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("[%d] %s Parent=%d Pos(%.2f,%.2f,%.2f) Scale(%.2f,%.2f,%.2f)"),
				BoneIndex,
				*BoneNames[BoneIndex].ToString(),
				ParentIndex,
				Translation.X, Translation.Y, Translation.Z,
				Scale.X, Scale.Y, Scale.Z);
		}
	}

	CaptureCurves(SkelComp);

	TArray<FName> FilteredCurveNames;
	TArray<float> FilteredCurveValues;
	BuildFilteredCurves(FilteredCurveNames, FilteredCurveValues);

	Frame.CurveNames = MoveTemp(FilteredCurveNames);
	Frame.CurveValues = MoveTemp(FilteredCurveValues);

	OnPoseFrameReady.Broadcast(Subject, Frame);
}

void UO3DSenderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ActiveSender.IsValid())
	{
		ActiveSender->Tick(DeltaTime);
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
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoCreateTransport)
	};

	const bool bInGameWorld = (GetWorld() && GetWorld()->IsGameWorld());
	const bool bWasCapturing = bIsCapturing;

	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName))
	{
		EnsureValidTransportName();
		ClearTransportOptions();
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
