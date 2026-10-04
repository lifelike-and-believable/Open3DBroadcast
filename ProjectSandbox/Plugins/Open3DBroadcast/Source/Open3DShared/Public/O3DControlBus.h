// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Misc/Optional.h"
#include "O3DControlTypes.h"
#include "Templates/UniquePtr.h"

/** One control change, as the bus publishes it. */
struct OPEN3DSHARED_API FO3DControlChange
{
	enum class EKind : uint8
	{
		ValueChanged,
		ValueCleared,
		Event
	};

	EKind Kind = EKind::ValueChanged;
	/** The key (values) or the event name (events). Case-sensitive. */
	FString Name;
	/** Empty for ValueCleared. */
	FO3DControlValue Value;
	FO3DControlMeta Meta;
};

/** ChangeRef is valid only for the duration of the broadcast; listeners copy what they keep. */
DECLARE_MULTICAST_DELEGATE_OneParam(FO3DOnControlChange, const FO3DControlChange& /*ChangeRef*/);

/**
 * Hand-off from receiver sources to gameplay (docs/adr/0011-control-channel.md, item 8), the
 * control counterpart of FO3DAudioBus. Receiver sources publish the changes their
 * ControlReceiver produced; components such as UO3DRemoteControlComponent listen and filter.
 *
 * Each FO3DRuntimeContext owns one bus (FInstance,
 * docs/adr/0012-runtime-services-and-global-state.md). The statics below use the default
 * context's bus (FO3DRuntimeContext::Default()), so code written against them behaves as before.
 * The receive override is the exception: it is process-wide, not per context, because it
 * overrides a project setting for the whole process.
 *
 * Threading (as FO3DAudioBus, SHR-10): game thread only. Every function checks it. Publishers
 * on other threads marshal to the game thread first.
 *
 * Duplicate delivery: two receiver sources can hear the same sender (UDP multicast, one MoQ
 * track, a duplicated LiveLink source). Each runs its own ControlReceiver, so both would publish
 * every change. The bus therefore drops an event whose (SourceId, Epoch, EventId) it has already
 * published, and applies a value change or clear only when its (Epoch, Version) is newer than
 * what it holds for that (SourceId, Key, Target). A clear with Version 0 (a receiver dropping a
 * source that went quiet) always applies.
 *
 * Keys, event names, targets and source ids are case-sensitive FStrings.
 */
class OPEN3DSHARED_API FO3DControlBus
{
public:
	/** One control bus. Obtained from FO3DRuntimeContext::GetControlBus(); not copyable. */
	class OPEN3DSHARED_API FInstance
	{
	public:
		FInstance();
		~FInstance();
		FInstance(const FInstance&) = delete;
		FInstance& operator=(const FInstance&) = delete;

		/** Fired for every change that passes the duplicate checks. Game thread only. */
		FO3DOnControlChange& OnChange();

		/**
		 * Applies the duplicate checks, updates the value cache, and broadcasts. Returns false when
		 * the change was dropped as a duplicate or stale. Game thread only.
		 */
		bool Publish(const FO3DControlChange& Change);

		/** The current value of (SourceId, Key, Target), or nullptr. Valid until the next Publish or Forget. Game thread only. */
		const FO3DControlValue* FindValue(const FString& SourceId, const FString& Key, const FString& Target) const;

		/** Every current value from one source as (Key, Target, Value), sorted by key then target. Game thread only. */
		TArray<TTuple<FString, FString, FO3DControlValue>> GetValues(const FString& SourceId) const;

		/** Sources that currently have values or recent events. Game thread only. */
		TArray<FString> GetSources() const;

		/** Forgets a source's values and event history without broadcasting. Game thread only. */
		void ForgetSource(const FString& SourceId);

		/** Clears every listener, value and event history. Tests only. Game thread only. */
		void ResetForTesting();

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	/** The default context's OnChange(). Game thread only. */
	static FO3DOnControlChange& OnChange();

	/** The default context's Publish(). Game thread only. */
	static bool Publish(const FO3DControlChange& Change);

	/** The default context's FindValue(). Game thread only. */
	static const FO3DControlValue* FindValue(const FString& SourceId, const FString& Key, const FString& Target);

	/** The default context's GetValues(). Game thread only. */
	static TArray<TTuple<FString, FString, FO3DControlValue>> GetValues(const FString& SourceId);

	/** The default context's GetSources(). Game thread only. */
	static TArray<FString> GetSources();

	/** The default context's ForgetSource(). Game thread only. */
	static void ForgetSource(const FString& SourceId);

	/**
	 * Runtime override for receiving control in this process (ADR 0011 item 8, "Enabling
	 * control on a client"): set means it overrides the project setting; unset means the project
	 * setting applies. A per-source setting other than ProjectDefault still wins over both.
	 * Process-wide, shared by every runtime context. Game thread only.
	 */
	static void SetReceiveOverride(TOptional<bool> bEnabled);
	static TOptional<bool> GetReceiveOverride();

	/** Clears the default context's listeners, values and event history, and the override. Tests only. Game thread only. */
	static void ResetForTesting();
};
