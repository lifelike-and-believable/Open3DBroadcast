// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DRemoteControlComponent.h"

#include "O3DControlBus.h"

UO3DRemoteControlComponent::UO3DRemoteControlComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DRemoteControlComponent::BeginPlay()
{
	Super::BeginPlay();
	Bind();
}

void UO3DRemoteControlComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Unbind();
	Super::EndPlay(EndPlayReason);
}

void UO3DRemoteControlComponent::Bind()
{
	if (!BusHandle.IsValid())
	{
		BusHandle = FO3DControlBus::OnChange().AddUObject(this, &UO3DRemoteControlComponent::HandleChange);
	}
}

void UO3DRemoteControlComponent::Unbind()
{
	if (BusHandle.IsValid())
	{
		FO3DControlBus::OnChange().Remove(BusHandle);
		BusHandle.Reset();
	}
}

bool UO3DRemoteControlComponent::PassesSourceFilter(const FString& SourceId) const
{
	if (SourceNameFilter.IsEmpty())
	{
		return true;
	}
	const FString* Name = SourceNames.Find(SourceId);
	return Name != nullptr && Name->Equals(SourceNameFilter, ESearchCase::CaseSensitive);
}

bool UO3DRemoteControlComponent::PassesFilters(const FString& Name, const FString& Target, const FO3DControlMeta& Meta) const
{
	if (!StreamIdFilter.IsEmpty() && !Meta.StreamId.Equals(StreamIdFilter, ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (!SourceNameFilter.IsEmpty() && !Meta.SourceName.Equals(SourceNameFilter, ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (!TargetSubjectFilter.IsEmpty())
	{
		const bool bAimedHere = Target.Equals(TargetSubjectFilter, ESearchCase::CaseSensitive);
		if (!bAimedHere && !(bIncludeUntargeted && Target.IsEmpty()))
		{
			return false;
		}
	}
	return NamePrefixFilter.IsEmpty() || Name.StartsWith(NamePrefixFilter, ESearchCase::CaseSensitive);
}

void UO3DRemoteControlComponent::HandleChange(const FO3DControlChange& Change)
{
	SourceNames.Add(Change.Meta.SourceId, Change.Meta.SourceName);
	if (!PassesFilters(Change.Name, Change.Meta.TargetSubject, Change.Meta))
	{
		return;
	}
	switch (Change.Kind)
	{
	case FO3DControlChange::EKind::Event:
		OnControlEvent.Broadcast(Change.Name, Change.Value, Change.Meta);
		break;
	case FO3DControlChange::EKind::ValueChanged:
		OnControlValueChanged.Broadcast(Change.Name, Change.Value, Change.Meta);
		break;
	case FO3DControlChange::EKind::ValueCleared:
		OnControlValueCleared.Broadcast(Change.Name, Change.Meta);
		break;
	}
}

bool UO3DRemoteControlComponent::GetControlValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const
{
	for (const FString& SourceId : FO3DControlBus::GetSources())
	{
		if (!PassesSourceFilter(SourceId))
		{
			continue;
		}
		if (const FO3DControlValue* Value = FO3DControlBus::FindValue(SourceId, Key, TargetSubject))
		{
			OutValue = *Value;
			return true;
		}
	}
	OutValue = FO3DControlValue();
	return false;
}

TArray<FO3DControlEntry> UO3DRemoteControlComponent::GetAllControlValues() const
{
	TArray<FO3DControlEntry> Entries;
	for (const FString& SourceId : FO3DControlBus::GetSources())
	{
		if (!PassesSourceFilter(SourceId))
		{
			continue;
		}
		for (const TTuple<FString, FString, FO3DControlValue>& Value : FO3DControlBus::GetValues(SourceId))
		{
			// The stream filter is not applied here: the bus caches values per sender, not per stream.
			if (!NamePrefixFilter.IsEmpty() && !Value.Get<0>().StartsWith(NamePrefixFilter, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!TargetSubjectFilter.IsEmpty())
			{
				const bool bAimedHere = Value.Get<1>().Equals(TargetSubjectFilter, ESearchCase::CaseSensitive);
				if (!bAimedHere && !(bIncludeUntargeted && Value.Get<1>().IsEmpty()))
				{
					continue;
				}
			}
			FO3DControlEntry Entry;
			Entry.Key = Value.Get<0>();
			Entry.TargetSubject = Value.Get<1>();
			Entry.Value = Value.Get<2>();
			Entry.SourceId = SourceId;
			Entries.Add(MoveTemp(Entry));
		}
	}
	return Entries;
}
