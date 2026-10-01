// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DControlBus.h"

#include "Misc/Crc.h"

namespace
{
	/**
	 * TMap<FString, ...> compares keys case-insensitively by default. Control keys, targets and
	 * source ids are case-sensitive ("Light.Intensity" and "light.intensity" are two keys), so
	 * every string-keyed map here uses these key functions instead.
	 */
	template <typename ValueType>
	struct TCaseSensitiveStringKeyFuncs : TDefaultMapKeyFuncs<FString, ValueType, false>
	{
		using Super = TDefaultMapKeyFuncs<FString, ValueType, false>;
		static bool Matches(typename Super::KeyInitType A, typename Super::KeyInitType B)
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		static uint32 GetKeyHash(typename Super::KeyInitType Key)
		{
			return FCrc::StrCrc32(*Key);
		}
	};

	template <typename ValueType>
	using TCaseSensitiveStringMap = TMap<FString, ValueType, FDefaultSetAllocator, TCaseSensitiveStringKeyFuncs<ValueType>>;

	/** Events remembered per (source, epoch) for duplicate suppression; matches the core receiver's default window. */
	constexpr int32 EventHistoryPerSource = 512;

	struct FCachedValue
	{
		FO3DControlValue Value;
		int64 Epoch = 0;
		int64 Version = 0;
	};

	struct FSourceState
	{
		/** Keyed by SlotKey(Key, Target); the (Key, Target) pair is stored alongside for GetValues. */
		TCaseSensitiveStringMap<TPair<TPair<FString, FString>, FCachedValue>> Values;
		int64 EventEpoch = 0;
		TSet<int64> SeenEvents;
		TArray<int64> SeenOrder;
	};

	FO3DOnControlChange GOnChange;
	TCaseSensitiveStringMap<FSourceState> GSources;
	TOptional<bool> GReceiveOverride;

	FString SlotKey(const FString& Key, const FString& Target)
	{
		// Length-prefixed, so ("a", "bc") and ("ab", "c") never collide whatever the strings hold.
		return FString::Printf(TEXT("%d:%s|%s"), Key.Len(), *Key, *Target);
	}

	bool Newer(int64 EpochA, int64 VersionA, int64 EpochB, int64 VersionB)
	{
		return EpochA > EpochB || (EpochA == EpochB && VersionA > VersionB);
	}
}

FO3DOnControlChange& FO3DControlBus::OnChange()
{
	check(IsInGameThread());
	return GOnChange;
}

bool FO3DControlBus::Publish(const FO3DControlChange& Change)
{
	check(IsInGameThread());
	const FO3DControlMeta& Meta = Change.Meta;
	FSourceState& Source = GSources.FindOrAdd(Meta.SourceId);

	switch (Change.Kind)
	{
	case FO3DControlChange::EKind::Event:
	{
		if (Meta.Epoch != Source.EventEpoch)
		{
			if (Meta.Epoch < Source.EventEpoch)
			{
				return false; // an older session of this source
			}
			Source.EventEpoch = Meta.Epoch;
			Source.SeenEvents.Reset();
			Source.SeenOrder.Reset();
		}
		if (Source.SeenEvents.Contains(Meta.EventId))
		{
			return false;
		}
		Source.SeenEvents.Add(Meta.EventId);
		Source.SeenOrder.Add(Meta.EventId);
		if (Source.SeenOrder.Num() > EventHistoryPerSource)
		{
			Source.SeenEvents.Remove(Source.SeenOrder[0]);
			Source.SeenOrder.RemoveAt(0, 1, EAllowShrinking::No);
		}
		break;
	}
	case FO3DControlChange::EKind::ValueChanged:
	{
		const FString Key = SlotKey(Change.Name, Meta.TargetSubject);
		if (auto* Existing = Source.Values.Find(Key))
		{
			FCachedValue& Cached = Existing->Value;
			if (!Newer(Meta.Epoch, Meta.Version, Cached.Epoch, Cached.Version))
			{
				return false;
			}
			Cached.Epoch = Meta.Epoch;
			Cached.Version = Meta.Version;
			Cached.Value = Change.Value;
		}
		else
		{
			FCachedValue Cached;
			Cached.Value = Change.Value;
			Cached.Epoch = Meta.Epoch;
			Cached.Version = Meta.Version;
			Source.Values.Add(Key, MakeTuple(MakeTuple(Change.Name, Meta.TargetSubject), MoveTemp(Cached)));
		}
		break;
	}
	case FO3DControlChange::EKind::ValueCleared:
	{
		const FString Key = SlotKey(Change.Name, Meta.TargetSubject);
		auto* Existing = Source.Values.Find(Key);
		if (Existing == nullptr)
		{
			return false; // nothing to clear (or another source's receiver cleared it already)
		}
		if (Meta.Version != 0 && !Newer(Meta.Epoch, Meta.Version, Existing->Value.Epoch, Existing->Value.Version))
		{
			return false;
		}
		Source.Values.Remove(Key);
		break;
	}
	}

	GOnChange.Broadcast(Change);
	return true;
}

const FO3DControlValue* FO3DControlBus::FindValue(const FString& SourceId, const FString& Key, const FString& Target)
{
	check(IsInGameThread());
	const FSourceState* Source = GSources.Find(SourceId);
	if (Source == nullptr)
	{
		return nullptr;
	}
	const auto* Entry = Source->Values.Find(SlotKey(Key, Target));
	return Entry ? &Entry->Value.Value : nullptr;
}

TArray<TTuple<FString, FString, FO3DControlValue>> FO3DControlBus::GetValues(const FString& SourceId)
{
	check(IsInGameThread());
	TArray<TTuple<FString, FString, FO3DControlValue>> Result;
	if (const FSourceState* Source = GSources.Find(SourceId))
	{
		for (const auto& Pair : Source->Values)
		{
			Result.Emplace(Pair.Value.Key.Key, Pair.Value.Key.Value, Pair.Value.Value.Value);
		}
	}
	Result.Sort([](const TTuple<FString, FString, FO3DControlValue>& A, const TTuple<FString, FString, FO3DControlValue>& B)
	{
		const int32 ByKey = A.Get<0>().Compare(B.Get<0>(), ESearchCase::CaseSensitive);
		return ByKey != 0 ? ByKey < 0 : A.Get<1>().Compare(B.Get<1>(), ESearchCase::CaseSensitive) < 0;
	});
	return Result;
}

TArray<FString> FO3DControlBus::GetSources()
{
	check(IsInGameThread());
	// Not GetKeys(): it de-duplicates through a default TSet<FString>, which is case-insensitive
	// and would merge source ids that differ only in case.
	TArray<FString> Result;
	Result.Reserve(GSources.Num());
	for (const auto& Pair : GSources)
	{
		Result.Add(Pair.Key);
	}
	Result.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return Result;
}

void FO3DControlBus::ForgetSource(const FString& SourceId)
{
	check(IsInGameThread());
	GSources.Remove(SourceId);
}

void FO3DControlBus::SetReceiveOverride(TOptional<bool> bEnabled)
{
	check(IsInGameThread());
	GReceiveOverride = bEnabled;
}

TOptional<bool> FO3DControlBus::GetReceiveOverride()
{
	check(IsInGameThread());
	return GReceiveOverride;
}

void FO3DControlBus::ResetForTesting()
{
	check(IsInGameThread());
	GOnChange.Clear();
	GSources.Reset();
	GReceiveOverride.Reset();
}
