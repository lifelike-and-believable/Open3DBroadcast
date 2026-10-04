// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DControlBus.h"
#include "O3DControlTypes.h"
#include "Templates/SharedPointer.h"
#include "O3DRemoteControlComponent.generated.h"

class FO3DRuntimeContext;

struct FO3DControlChange;

/** One current value, as UO3DRemoteControlComponent::GetAllControlValues returns it. */
USTRUCT(BlueprintType)
struct OPEN3DRECEIVER_API FO3DControlEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString Key;

	/** The subject the value is aimed at; empty for the whole stream. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString TargetSubject;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FO3DControlValue Value;

	/** The sender it came from. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString SourceId;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FO3DOnRemoteControlEvent, const FString&, EventName, const FO3DControlValue&, Value, const FO3DControlMeta&, Meta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FO3DOnRemoteControlValueChanged, const FString&, Key, const FO3DControlValue&, Value, const FO3DControlMeta&, Meta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FO3DOnRemoteControlValueCleared, const FString&, Key, const FO3DControlMeta&, Meta);

/**
 * Receives control events and values from Open3DBroadcast senders (docs/adr/0011-control-channel.md,
 * item 8): cues to fire VFX, lights or audio, and environment or character parameters.
 *
 * Add it to any actor, set the filters, and bind the events. Control must be accepted for the
 * receiver source (Project Settings > Plugins > Open3DBroadcast Control, or
 * UO3DControlLibrary::SetControlReceiveEnabled at runtime).
 *
 * Names, keys and targets are case-sensitive. Nothing here sets properties or runs commands by
 * itself: the project decides what each event and key means.
 */
UCLASS(ClassGroup = (Open3DBroadcast), meta = (BlueprintSpawnableComponent))
class OPEN3DRECEIVER_API UO3DRemoteControlComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UO3DRemoteControlComponent();

	/**
	 * The runtime context whose control changes this component hears
	 * (docs/adr/0012-runtime-services-and-global-state.md): receiver sources with the same Context
	 * Name. Empty: the default context. Case-insensitive. Read when play begins; the value queries
	 * use the same context.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FName ContextName;

	/** Only changes received on this transport stream (the receiver source's stream id). Empty accepts every stream. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control|Filter")
	FString StreamIdFilter;

	/** Only changes from senders with this name (the sender component's owner). Empty accepts every sender. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control|Filter")
	FString SourceNameFilter;

	/** Only changes aimed at this subject (a character's LiveLink subject name). Empty accepts every target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control|Filter")
	FString TargetSubjectFilter;

	/** With a target filter set, also accept changes aimed at the whole stream (no target). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control|Filter")
	bool bIncludeUntargeted = true;

	/** Only event names and keys starting with this, for example "vfx." or "env.". Empty accepts every name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control|Filter")
	FString NamePrefixFilter;

	/** A sender fired an event. */
	UPROPERTY(BlueprintAssignable, Category = "Open3DBroadcast|Control")
	FO3DOnRemoteControlEvent OnControlEvent;

	/** A sender set a value, or the value changed. */
	UPROPERTY(BlueprintAssignable, Category = "Open3DBroadcast|Control")
	FO3DOnRemoteControlValueChanged OnControlValueChanged;

	/** A sender cleared a value, or the sender went away. */
	UPROPERTY(BlueprintAssignable, Category = "Open3DBroadcast|Control")
	FO3DOnRemoteControlValueCleared OnControlValueCleared;

	/**
	 * The current value of Key (aimed at TargetSubject, or at the whole stream when empty) from
	 * the first sender that passes the source filter. False when no such value exists.
	 */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	bool GetControlValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const;

	/** Every current value from senders that pass the filters. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	TArray<FO3DControlEntry> GetAllControlValues() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void Bind();
	void Unbind();
	void HandleChange(const FO3DControlChange& Change);
	bool PassesFilters(const FString& Name, const FString& Target, const FO3DControlMeta& Meta) const;
	bool PassesSourceFilter(const FString& SourceId) const;

	/** The bus this component listens to and queries: BoundContext's, or ContextName's before play. */
	FO3DControlBus::FInstance& GetBus() const;

	FDelegateHandle BusHandle;
	/** The context BusHandle is bound in, so Unbind removes it from the same bus. */
	TSharedPtr<FO3DRuntimeContext, ESPMode::ThreadSafe> BoundContext;
	/** Source id -> display name, learned from changes, for SourceNameFilter in the value queries. */
	TMap<FString, FString> SourceNames;

	friend struct FO3DRemoteControlComponentTestAccessor;
};
