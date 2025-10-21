#include "Open3DStreamSource.h"
#include "LiveLinkRoleTrait.h"

#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Common/UdpSocketBuilder.h"
#include "Internationalization/Regex.h"

#include "o3ds_generated.h"
#include "UnrealModel.h"
#include "o3ds/model.h"

//#include "get_time.h"
using namespace O3DS::Data;


// E:\Unreal\UE_4.25\Engine\Plugins\Animation\LiveLink\Source\LiveLink\Private\LiveLinkMessageBusSource.cpp
// E:\Unreal\UE_4.25\Engine\Source\Runtime\LiveLinkInterface\Public\LiveLinkTypes.h
// E:\Unreal\UE_4.25\Engine\Plugins\Runtime\AR\Apple\AppleARKit\Source\AppleARKitPoseTrackingLiveLink\Private\AppleARKitPoseTrackingLiveLinkSource.cpp


#define LOCTEXT_NAMESPACE "Open3DStream"

static uint64 HashArray(const TArray<FName>& Names, const TArray<int32>* Parents = nullptr)
{
    uint64 H = 1469598103934665603ull; // FNV-1a 64-bit
    auto Mix = [&H](const void* Data, SIZE_T Bytes)
    {
        const uint8* P = static_cast<const uint8*>(Data);
        for (SIZE_T i = 0; i < Bytes; ++i)
        {
            H ^= P[i];
            H *= 1099511628211ull;
        }
    };
    for (const FName& N : Names)
    {
        const FString S = N.ToString();
        Mix(*S, S.Len() * sizeof(TCHAR));
    }
    if (Parents && Parents->Num() > 0)
    {
        Mix(Parents->GetData(), Parents->Num() * sizeof(int32));
    }
    return H;
}

static uint64 HashCurveNames(const TArray<FName>& Names)
{
    return HashArray(Names, nullptr);
}

FOpen3DStreamSource::FOpen3DStreamSource()
:FOpen3DStreamSource(GetDefault<UOpen3DStreamSettingsObject>()->Settings)
{
}

FOpen3DStreamSource::FOpen3DStreamSource(const FOpen3DStreamSettings& Settings)
    : SourceType(LOCTEXT("ConnctionType", "Open 3D Stream"))
    , SourceMachineName(LOCTEXT("SourceMachineName", "-"))
    , SourceStatus(LOCTEXT("ConnctionStatus", "Inactive"))
    , Settings(nullptr)
    , Client(nullptr)
    , ArrivalTimeOffset(0.0)
    , Frame(0)
    , LogFlag(false)
    , bIsValid(true)
    , mAddr(ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr())
{
    Url        = Settings.Url;
    Protocol   = Settings.Protocol;
    Key        = Settings.Key;
    TimeOffset = Settings.TimeOffset;

    server.OnData.BindRaw(this, &FOpen3DStreamSource::OnPackage);
    server.OnState.BindRaw(this, &FOpen3DStreamSource::OnStatus);
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

    if (!server.start(Url, Protocol))
    {
        bIsValid = false;
    }
    UpdateConnectionLastActive();
}

void FOpen3DStreamSource::Tick(float DeltaTime)
{
    TimeSinceLastCheck += DeltaTime;
    if (TimeSinceLastCheck >= CheckInterval)
    {
        RemoveInactiveSubjects();
        TimeSinceLastCheck = 0;
    }
    this->server.tick();
}

bool FOpen3DStreamSource::IsTickable() const
{
    return true; // this->server.mTcp != nullptr;
}

// Remove ad-hoc basis hacks; use TRS directly
void operator >>(const O3DS::Matrixd& src, FMatrix& dst)
{
    for (int u = 0; u < 4; ++u)
    {
        for (int v = 0; v < 4; ++v)
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

    //O3DS::Unreal::UnBuilder builder;
    if (!mSubjects.Parse((const char*)data.GetData(), data.Num(), 0))
    {
        OnStatus(LOCTEXT("DataError", "Data Error"), true);
        return;
    }

    TArray<FName>      BoneNames;
    TArray<int32>      BoneParents;

    for (O3DS::Subject* subject : mSubjects)
    {
        FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
        FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
        FLiveLinkBaseFrameData& BaseFrameData = static_cast<FLiveLinkBaseFrameData&>(FrameData);

        FrameData.WorldTime = FPlatformTime::Seconds();
        FFrameRate FrameRate(60, 1);
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

        for (size_t ci = 0; ci < subject->mCurveNames.size(); ++ci)
        {
            float value = subject->mCurveValues.size() > ci ? subject->mCurveValues[ci] : 0.0f;
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
            if (BoneNames.Num() == 0)
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
                UE_LOG(LogTemp, Log, TEXT("O3DS LiveLink: Created subject '%s'"), *SubjectName.Name.ToString());
            }
            else
            {
                UE_LOG(LogTemp, Log, TEXT("O3DS LiveLink: Static data updated for subject '%s' (bones/curves changed)"), *SubjectName.Name.ToString());
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
    //  OnPackage((uint8*)(server.buffer.data()), server.buffer.size());
}

FORCEINLINE void FOpen3DStreamSource::UpdateConnectionLastActive()
{
    FScopeLock ConnectionTimeLock(&ConnectionLastActiveSection);
    ConnectionLastActive = FPlatformTime::Seconds();
}

#undef LOCTEXT_NAMESPACE
