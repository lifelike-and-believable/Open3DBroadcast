// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSBroadcastLoopbackAdapter.generated.h"

class UO3DSBroadcastComponent;
class FOpen3DStreamSource;
class ULiveLinkSubsystem;

/**
 * Dev-only in-memory loopback from broadcast serialized bytes to the local LiveLink receiver.
 * Enabled via CVar: o3ds.Broadcast.Loopback=1 (editor-only).
 */
UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSBroadcastLoopbackAdapter : public UActorComponent
{
    GENERATED_BODY()
public:
    UO3DSBroadcastLoopbackAdapter();

    // Ensure hook also occurs when component is created dynamically after BeginPlay
    virtual void OnRegister() override;

#if WITH_EDITOR
    // Expose CVar check so other modules can query
    static int32 GetLoopbackEnabled();
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void EnsureHooked();
    void Unhook();
    void EnsureLiveLinkSource();

    void OnSerializedFrameReceived(const FString& Subject, const TArray<uint8>& Buffer, double Timestamp);

    TWeakObjectPtr<UO3DSBroadcastComponent> BroadcastComponent;

    // LiveLink
    TSharedPtr<FOpen3DStreamSource> Source;
    FGuid SourceGuid;

    FDelegateHandle SerializedFrameHandle;
};
