// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "IBroadcastTransport.h"
#include "O3DSWebRtcBackend.h" // shared WebRTC backend enum (BlueprintType)
#include "O3DSBroadcastTransportAdapter.generated.h"

class UO3DSBroadcastComponent;

UENUM()
enum class EO3DSTransportKind : uint8
{
    Disabled,
    TCP UMETA(DisplayName="TCP"),
    TCPServer UMETA(DisplayName="TCP Server"), // broadcaster listens and sends O3DS header framed payloads
    UDP,
    NNG,
    // Explicit WebRTC roles for symmetry with receiver
    WebRTCClient UMETA(DisplayName="WebRTC Client"),
    WebRTCServer UMETA(DisplayName="WebRTC Server")
};

// Transport family for new UX (preferred over EO3DSTransportKind)
UENUM(BlueprintType)
enum class EO3DSTransportFamily : uint8
{
    NNG UMETA(DisplayName="NNG"),
    TCP UMETA(DisplayName="TCP"),
    UDP UMETA(DisplayName="UDP"),
    WebRTC UMETA(DisplayName="WebRTC")
};

// NNG-specific modes
UENUM(BlueprintType)
enum class EO3DSNngMode : uint8
{
    Publisher UMETA(DisplayName="Publisher"),
    PairClient UMETA(DisplayName="Pair Client"),
    PairServer UMETA(DisplayName="Pair Server"),
    Push UMETA(DisplayName="Push")
};

// TCP-specific modes
UENUM(BlueprintType)
enum class EO3DSTcpMode : uint8
{
    Client UMETA(DisplayName="Client"),
    Server UMETA(DisplayName="Server")
};

// WebRTC-specific modes
UENUM(BlueprintType)
enum class EO3DSWebRtcMode : uint8
{
    Client UMETA(DisplayName="Client"),
    Server UMETA(DisplayName="Server")
};

// WebRTC backend selection now comes from shared header (EO3DSWebRtcBackend)

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSBroadcastTransportAdapter : public UActorComponent
{
    GENERATED_BODY()
public:
    UO3DSBroadcastTransportAdapter();

    // Enable/disable this adapter instance
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    bool bEnable = false;

    // Transport selection and endpoint (legacy; hidden)
    UPROPERTY(meta=(DisplayName="Transport (Deprecated)", DeprecatedProperty, DeprecationMessage="Use Transport Family + Mode instead", EditCondition="false", EditConditionHides), EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    EO3DSTransportKind Transport = EO3DSTransportKind::Disabled;

    // New Transport Family UX (preferred)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    EO3DSTransportFamily TransportFamily = EO3DSTransportFamily::TCP;

    // Mode selection per family (conditional visibility)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::NNG", EditConditionHides))
    EO3DSNngMode NngMode = EO3DSNngMode::Publisher;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::TCP", EditConditionHides))
    EO3DSTcpMode TcpMode = EO3DSTcpMode::Client;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::WebRTC", EditConditionHides))
    EO3DSWebRtcMode WebRtcMode = EO3DSWebRtcMode::Client;

    // WebRTC backend selection (libdatachannel P2P or LiveKit SFU)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::WebRTC", EditConditionHides))
    EO3DSWebRtcBackend WebRtcBackend = EO3DSWebRtcBackend::LibDataChannel;

    // LiveKit-specific configuration (only shown when WebRTC + LiveKit backend)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|LiveKit", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::WebRTC && WebRtcBackend == EO3DSWebRtcBackend::LiveKit", EditConditionHides))
    FString LiveKitServerUrl = TEXT("wss://livekit.example.com");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|LiveKit", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::WebRTC && WebRtcBackend == EO3DSWebRtcBackend::LiveKit", EditConditionHides))
    FString LiveKitRoom = TEXT("room1");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport|LiveKit", meta=(EditCondition="TransportFamily == EO3DSTransportFamily::WebRTC && WebRtcBackend == EO3DSWebRtcBackend::LiveKit", EditConditionHides))
    FString LiveKitToken;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    FString Url = TEXT("tcp://127.0.0.1:9000");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    FString Key;

    // Queue size for backpressure (bytes)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    int32 MaxQueuedBytes = 8 * 1024 * 1024; // 8 MB

    // Optional: reference to the broadcaster component. If not set, auto-find.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    TWeakObjectPtr<UO3DSBroadcastComponent> BroadcastComponent;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    void EnsureBound();
    void Unbind();

    void OnSerializedFrame(const FString& Subject, const TArray<uint8>& Buffer, double Timestamp);

    // Resolve concrete transport from enum
    TUniquePtr<IBroadcastTransport> CreateTransport() const;

    struct FItem { TArray<uint8> Data; double Ts = 0.0; };

    TQueue<FItem, EQueueMode::Mpsc> Queue;

    TUniquePtr<IBroadcastTransport> TransportImpl;
    FDelegateHandle SerializedHandle;

    // Counters
    TAtomic<uint64> QueuedBytes{0};
    TAtomic<uint64> EnqueuedFrames{0};
    TAtomic<uint64> DroppedFrames{0};

    // CVars
    // Note: Runtime CVars for this adapter are defined at file scope in the .cpp to avoid
    // template/macro conflicts in some non-editor builds. See O3DSBroadcastTransportAdapter.cpp.

    // Stats dumping support
    void DumpStatsInstance() const;
    static void DumpAllStats();
};
