// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "IBroadcastTransport.h"
#include "Containers/Queue.h" // TQueue, EQueueMode
#include "HAL/IConsoleManager.h" // TAutoConsoleVariable
#include "O3DSBroadcastTransportAdapter.generated.h"

class UO3DSBroadcastComponent;

UENUM()
enum class EO3DSTransportKind : uint8
{
    Disabled,
    TCP UMETA(DisplayName="TCP"),
    TCPServer UMETA(DisplayName="TCP Server"), // new: broadcaster listens and sends O3DS header framed payloads
    UDP,
    NNG,
    WebRTC
};

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSBroadcastTransportAdapter : public UActorComponent
{
    GENERATED_BODY()
public:
    UO3DSBroadcastTransportAdapter();

    // Transport selection and endpoint
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast|Transport")
    EO3DSTransportKind Transport = EO3DSTransportKind::Disabled;

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
