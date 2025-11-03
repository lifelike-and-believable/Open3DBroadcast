#include "Open3DStreamSource.h"
#include "LiveLinkRoleTrait.h"

#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Common/UdpSocketBuilder.h"
#include "Internationalization/Regex.h"

#include "o3ds_generated.h"
#include "O3DSHelpers.h"
#include "UnrealModel.h"
#include "o3ds/model.h"
#include "WebRTC/Open3DSWebRtcReceiver.h"

//#include "get_time.h"
using namespace O3DS::Data;

// Receiver-side diagnostics
static TAutoConsoleVariable<int32> CVarO3DSReceiverDebugParse(
 TEXT("o3ds.Receiver.DebugParse"),
1,
 TEXT("Enable debug logs when parsing incoming O3DS packets (0/1)."),
 ECVF_Default);


#define LOCTEXT_NAMESPACE "Open3DStream"

// Use shared hashing helpers
static uint64 HashArray(const TArray<FName>& Names, const TArray<int32>* Parents = nullptr)
{
	return Parents ? O3DSHelpers::HashNamesAndParents(Names, *Parents)
				   : O3DSHelpers::HashNames(Names);
}

static uint64 HashCurveNames(const TArray<FName>& Names)
{
	return O3DSHelpers::HashNames(Names);
}

FOpen3DStreamSource::FOpen3DStreamSource()
:FOpen3DStreamSource(GetDefault<UOpen3DStreamSettingsObject>()->Settings)
{
}

FOpen3DStreamSource::FOpen3DStreamSource(const FOpen3DStreamSettings& Settings)
 : SourceType(LOCTEXT("ConnctionType", "Open3D Stream"))
 , SourceMachineName(LOCTEXT("SourceMachineName", "-"))
 , SourceStatus(LOCTEXT("ConnctionStatus", "Inactive"))
 , Settings(nullptr)
 , Client(nullptr)
 , ArrivalTimeOffset(0.0)
 , Frame(0)
 , LogFlag(false)
 , bIsValid(true)
 , mAddr(ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr())
 , SourceSettings(Settings) // Store settings for later use
{
 Url = Settings.Url;
 Protocol = Settings.Protocol;
 Key = Settings.Key;
 TimeOffset = Settings.TimeOffset;

 // If WebRTC, ensure room exists as query param in URL for transport that expects it
 if (Protocol.ToString().Contains(TEXT("WebRTC")))
 {
 FString U = Url.ToString();
 const FString Room = Settings.WebRtcRoom;
 if (!Room.IsEmpty() && !U.Contains(TEXT("room=")))
 {
 U += U.Contains(TEXT("?")) ? FString::Printf(TEXT("&room=%s"), *Room) : FString::Printf(TEXT("?room=%s"), *Room);
 Url = FText::FromString(U);
 }
 }

 server.OnData.BindRaw(this, &FOpen3DStreamSource::OnPackage);
 server.OnState.BindRaw(this, &FOpen3DStreamSource::OnStatus);

 // One-time log to confirm that the receiver is wiring the OnData callback
 static bool bLoggedBind = false;
 if (!bLoggedBind)
 {
 bLoggedBind = true;
 UE_LOG(LogTemp, Log, TEXT("O3DS RX: LiveLink OnData bound in Open3DStreamSource"));
 }
}

FOpen3DStreamSource::~FOpen3DStreamSource()
{}

void FOpen3DStreamSource::InitializeSettings(ULiveLinkSourceSettings* InSettings)
{}

TSubclassOf < ULiveLinkSourceSettings > FOpen3DStreamSource::GetSettingsClass() const
{
 return UOpen3DStreamSourceSettings::StaticClass();
}

void FOpen3DStreamSource::OnStatus(FText msg, bool IsError)
{
 FString smsg = msg.ToString();
 if (IsError) {
 UE_LOG(LogTemp, Warning, TEXT("O3DS: %s"), *smsg);
 }
 else {
 UE_LOG(LogTemp, Log, TEXT("O3DS: %s"), *smsg);
 }
 SourceStatus = msg;
 LogFlag = false;
}

void FOpen3DStreamSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
 // "The source has been added to the Client and a Guid has been associated"
 Client = InClient;
 SourceGuid = InSourceGuid;
 bIsValid = true;

 // Treat Loopback as an in-memory source that skips network start
 if (Protocol.ToString().Contains(TEXT("Loopback")) || Protocol.ToString().Contains(TEXT("Loopback (In-Editor Test)")) || Protocol.ToString().Contains(TEXT("InMemory")))
 {
 SourceStatus = LOCTEXT("LoopbackActive", "Loopback active");
 UpdateConnectionLastActive();
 return;
 }

 const FRegexPattern pattern(TEXT("^[0-9:.]+$"));
 FRegexMatcher matcher(pattern, Url.ToString());

 if (matcher.FindNext())
 {
 Url = FText::Format(LOCTEXT("FormattedUrl", "tcp://{0}"), Url);
 }

 // Normalize tcp URL typos (e.g., tcp://0.0.0.0.9000)
 {
	 FString U = Url.ToString();
	 const FString Normalized = O3DSHelpers::NormalizeTcpUrlHostPort(U);
	 if (!Normalized.Equals(U))
	 {
		 UE_LOG(LogTemp, Log, TEXT("O3DS RX: Normalized URL '%s' -> '%s'"), *U, *Normalized);
		 Url = FText::FromString(Normalized);
	 }
 }

 // Branch: if WebRTC, use the receiver adapter; otherwise use legacy server.start()
 if (Protocol.ToString().Contains(TEXT("WebRTC")))
 {
	 WebRtcReceiver = MakeShared<FOpen3DSWebRtcReceiver>();
	 WebRtcReceiver->SetOnDataCallback([this](const TArray<uint8>& Bytes) { OnPackage(Bytes); });
	 WebRtcReceiver->SetOnStateCallback([this](const FString& State, bool bIsError) { OnStatus(FText::FromString(State), bIsError); });

	 if (!WebRtcReceiver->Start(SourceSettings))
	 {
		 UE_LOG(LogTemp, Error, TEXT("O3DS RX: WebRTC receiver failed to start"));
		 bIsValid = false;
	 }
 }
 else
 {
	 if (!server.start(Url, Protocol, &SourceSettings))
	 {
		 bIsValid = false;
	 }
 }
 UpdateConnectionLastActive();
}

void FOpen3DStreamSource::Tick(float DeltaTime)
{
	TimeSinceLastCheck += DeltaTime;
	if (TimeSinceLastCheck >= CheckInterval)
	{
		RemoveInactiveSubjects();
		TimeSinceLastCheck =0;
	}

	// Tick WebRTC receiver if active
	if (WebRtcReceiver)
	{
		WebRtcReceiver->Tick(DeltaTime);
	}
	else
	{
		this->server.tick();
	}
}bool FOpen3DStreamSource::IsTickable() const
{
 return true; // this->server.mTcp != nullptr;
}

// Remove ad-hoc basis hacks; use TRS directly
void operator >>(const O3DS::Matrixd& src, FMatrix& dst)
{
 for (int u =0; u <4; ++u)
 {
 for (int v =0; v <4; ++v)
 {
 dst.M[u][v] = src.m[u][v];
 }
 }
}

void FOpen3DStreamSource::OnPackage(const TArray<uint8>& data)
{
 if (!bIsValid)
 return;

 if(!LogFlag)
 {
 SourceStatus = FText::Format(LOCTEXT("ReceivingString", "Receiving {0}"), Protocol);
 LogFlag = true;
 }

 if (CVarO3DSReceiverDebugParse->GetInt() !=0)
 {
 const int32 DumpN = FMath::Min(64, data.Num());
 FString Hex; Hex.Reserve(DumpN *3);
 for (int32 i =0; i < DumpN; ++i) { Hex += FString::Printf(TEXT("%02X "), data[i]); }
 static const uint8 Magic[14] = {0x00,0xFF,0x03,0xFE,'O','3','D','S','-','S','T','A','R','T'};
 const bool bTcpMagic = (data.Num() >=14) && (FMemory::Memcmp(data.GetData(), Magic,14) ==0);
 UE_LOG(LogTemp, Verbose, TEXT("O3DS Receiver: packet bytes=%d tcp_magic=%s first_%d=%s"), data.Num(), bTcpMagic?TEXT("true"):TEXT("false"), DumpN, *Hex);
 }

 // Parse O3DS buffer (non-TCP transports provide raw payload without TCP magic)
 if (!mSubjects.Parse((const char*)data.GetData(), data.Num(),0))
 {
 OnStatus(LOCTEXT("DataError", "Data Error"), true);
 if (CVarO3DSReceiverDebugParse->GetInt() !=0)
 {
 UE_LOG(LogTemp, Warning, TEXT("O3DS Receiver: Parse failed (bytes=%d)"), data.Num());
 }
 return;
 }

 if (CVarO3DSReceiverDebugParse->GetInt() !=0)
 {
 int32 Count =0;
 for (auto* S : mSubjects) { (void)S; ++Count; }
 UE_LOG(LogTemp, Verbose, TEXT("O3DS Receiver: Parse OK subjects=%d time=%.6f"), Count, mSubjects.mTime);
 }

 TArray<FName> BoneNames;
 TArray<int32> BoneParents;

 for (O3DS::Subject* subject : mSubjects)
 {
 FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
 FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
 FLiveLinkBaseFrameData& BaseFrameData = static_cast<FLiveLinkBaseFrameData&>(FrameData);

 FrameData.WorldTime = FPlatformTime::Seconds();
 FFrameRate FrameRate(60,1);
 FFrameTime FrameTime = FFrameTime(FrameRate.AsFrameTime(mSubjects.mTime));
 FrameData.FrameId = Frame++;
 FrameData.MetaData.SceneTime = FQualifiedFrameTime(FrameTime, FrameRate);

 BoneNames.Empty();
 BoneParents.Empty();

 FLiveLinkSubjectName SubjectName(subject->mName.c_str());
 const FLiveLinkSubjectKey SubjectKey(SourceGuid, SubjectName);

 size_t transformCount = subject->mTransforms.size();
 auto &transforms = subject->mTransforms;

 BoneNames.Reserve(transformCount);
 BoneParents.Reserve(transformCount);

 bool nan = false;

 for (auto& transform : transforms)
 {
 // Copy to mutable locals (Vector[ ] is non-const in O3DS)
 O3DS::Vector3d T = transform->translation.value;
 O3DS::Vector4d R = transform->rotation.value;
 O3DS::Vector3d S = transform->scale.value;

 FQuat q((float)R[0], (float)R[1], (float)R[2], (float)R[3]);
 q.Normalize();
 FVector t((float)T[0], (float)T[1], (float)T[2]);
 FVector s((float)S[0], (float)S[1], (float)S[2]);

 if (!FMath::IsFinite(t.X) || !FMath::IsFinite(t.Y) || !FMath::IsFinite(t.Z) ||
 !FMath::IsFinite(q.X) || !FMath::IsFinite(q.Y) || !FMath::IsFinite(q.Z) || !FMath::IsFinite(q.W) ||
 !FMath::IsFinite(s.X) || !FMath::IsFinite(s.Y) || !FMath::IsFinite(s.Z))
 {
 nan = true; break;
 }

 FTransform fTransform(q, t, s);

 std::string name = transform->mName.c_str();
 size_t pos = name.rfind(':');
 if (pos != std::string::npos)
 name = name.erase(0, pos+1);

 BoneNames.Emplace(name.c_str());
 BoneParents.Emplace(transform->mParentId);
 FrameData.Transforms.Add(fTransform);
 }

 if (nan)
 {
 continue;
 }

 for (size_t ci =0; ci < subject->mCurveNames.size(); ++ci)
 {
 float value = subject->mCurveValues.size() > ci ? subject->mCurveValues[ci] :0.0f;
 BaseFrameData.PropertyValues.Add(value);
 }

 // Compute hashes to detect static-data changes
 const uint64 NewSkelHash = HashArray(BoneNames, &BoneParents);
 TArray<FName> CurveNames;
 CurveNames.Reserve(subject->mCurveNames.size());
 for (const std::string& CN : subject->mCurveNames)
 {
 CurveNames.Add(FName(CN.c_str()));
 }
 const uint64 NewCurveHash = HashCurveNames(CurveNames);

 const uint64* ExistingSkelHash = SubjectSkeletonHash.Find(SubjectName.Name);
 const uint64* ExistingCurveHash = SubjectCurveSetHash.Find(SubjectName.Name);
 const bool bNeedStaticUpdate = (ExistingSkelHash == nullptr || *ExistingSkelHash != NewSkelHash) ||
 (ExistingCurveHash == nullptr || *ExistingCurveHash != NewCurveHash);

 if (InitializedSubjects.Find(SubjectName) == INDEX_NONE || bNeedStaticUpdate)
 {
 if (BoneNames.Num() ==0)
 continue;

 // (Re)create subject and push static data when hashes changed
 FLiveLinkSubjectPreset Preset;
 Preset.Key = SubjectKey;
 Preset.Role = ULiveLinkAnimationRole::StaticClass();
 Preset.Settings = nullptr;
 Preset.bEnabled = true;
 Client->CreateSubject(Preset);

 FLiveLinkStaticDataStruct LiveLinkSkeletonStaticData;
 LiveLinkSkeletonStaticData.InitializeWith(FLiveLinkSkeletonStaticData::StaticStruct(), nullptr);
 FLiveLinkSkeletonStaticData* SkeletonDataPtr = LiveLinkSkeletonStaticData.Cast<FLiveLinkSkeletonStaticData>();

 SkeletonDataPtr->SetBoneNames(BoneNames);
 SkeletonDataPtr->SetBoneParents(BoneParents);

 TArray<FName> PropertyNames;
 PropertyNames.Reserve(CurveNames.Num());
 for (const FName& N : CurveNames)
 {
 PropertyNames.Add(N);
 }
 SkeletonDataPtr->PropertyNames = PropertyNames;

 Client->RemoveSubject_AnyThread(SubjectKey);
 Client->PushSubjectStaticData_AnyThread(SubjectKey, ULiveLinkAnimationRole::StaticClass(), MoveTemp(LiveLinkSkeletonStaticData));

 if (InitializedSubjects.Find(SubjectName) == INDEX_NONE)
 {
 InitializedSubjects.Add(SubjectName);
 UE_LOG(LogTemp, Log, TEXT("O3DS Receiver: Created subject '%s'"), *SubjectName.Name.ToString());
 }
 else
 {
 UE_LOG(LogTemp, Log, TEXT("O3DS Receiver: Static data updated for subject '%s' (bones/curves changed)"), *SubjectName.Name.ToString());
 }

 SubjectSkeletonHash.FindOrAdd(SubjectName.Name) = NewSkelHash;
 SubjectCurveSetHash.FindOrAdd(SubjectName.Name) = NewCurveHash;
 }

 Client->PushSubjectFrameData_AnyThread(SubjectKey, MoveTemp(FrameDataStruct));
 SubjectLastUpdateTime.FindOrAdd(SubjectName) = FPlatformTime::Seconds();
 }
}

void FOpen3DStreamSource::RemoveInactiveSubjects()
{
 const double CurrentTime = FPlatformTime::Seconds();

 for (auto It = SubjectLastUpdateTime.CreateIterator(); It; ++It)
 {
 double LastUpdateTime = It.Value();
 if (CurrentTime - LastUpdateTime > InactivityThreshold)
 {
 FName SubjectName = It.Key();
 SubjectLastUpdateTime.Remove(SubjectName);
 const FLiveLinkSubjectKey SubjectKey(SourceGuid, SubjectName);
 Client->RemoveSubject_AnyThread(SubjectKey);
 InitializedSubjects.Remove(SubjectName);
 SubjectSkeletonHash.Remove(SubjectName);
 SubjectCurveSetHash.Remove(SubjectName);
 }
 }
}

bool FOpen3DStreamSource::RequestSourceShutdown()
{
 bIsValid = false;

 // Stop WebRTC receiver if active
 if (WebRtcReceiver)
 {
	 WebRtcReceiver->Stop();
	 WebRtcReceiver.Reset();
 }

 this->server.OnData.Unbind();
 this->server.stop();
 Client = nullptr;
 SourceGuid.Invalidate();

 return true;
}

void FOpen3DStreamSource::Update()
{
 // Called during the game thread, return quickly.
 //if(server.Recv())
 // OnPackage((uint8*)(server.buffer.data()), server.buffer.size());
}

FORCEINLINE void FOpen3DStreamSource::UpdateConnectionLastActive()
{
 FScopeLock ConnectionTimeLock(&ConnectionLastActiveSection);
 ConnectionLastActive = FPlatformTime::Seconds();
}

#undef LOCTEXT_NAMESPACE
