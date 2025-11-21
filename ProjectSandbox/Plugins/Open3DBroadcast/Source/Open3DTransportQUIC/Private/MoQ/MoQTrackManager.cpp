// Copyright (c) Open3DStream Contributors

#include "MoQ/MoQTrackManager.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace O3DMoQ
{
	namespace
	{
		FString NormalizeKey(const FString& Value)
		{
			return Value.ToLower();
		}
	}

	FMoQTrackManager::FMoQTrackManager() = default;

	FMoQTrackId FMoQTrackManager::PublishTrack(const FString& TrackName, const FMoQTrackProperties& Properties, FMoQAnnounceMessage& OutAnnounce, FString& OutError)
	{
		if (!ValidateTrackName(TrackName, OutError))
		{
			return 0;
		}

		FMoQTrackProperties Sanitized = Properties;
		Sanitized.ClampPriority();

		const FString NormalizedName = NormalizeKey(TrackName);
		const double NowSeconds = FPlatformTime::Seconds();

		FScopeLock Lock(&Mutex);

		FMoQTrackDescriptor* Descriptor = nullptr;
		if (const FMoQTrackId* ExistingId = TracksByName.Find(NormalizedName))
		{
			Descriptor = TracksById.Find(*ExistingId);
		}

		if (Descriptor)
		{
			Descriptor->Properties = Sanitized;
			Descriptor->LastUpdatedSeconds = NowSeconds;
		}
		else
		{
			FMoQTrackDescriptor NewDescriptor;
			NewDescriptor.TrackId = ReserveTrackId();
			NewDescriptor.TrackName = TrackName;
			NewDescriptor.Properties = Sanitized;
			NewDescriptor.LastUpdatedSeconds = NowSeconds;

			TracksById.Add(NewDescriptor.TrackId, NewDescriptor);
			TracksByName.Add(NormalizedName, NewDescriptor.TrackId);
			TrackIdToNormalizedName.Add(NewDescriptor.TrackId, NormalizedName);
			Descriptor = TracksById.Find(NewDescriptor.TrackId);
		}

		if (!Descriptor)
		{
			OutError = TEXT("Failed to cache track descriptor.");
			return 0;
		}

		OutAnnounce.Version = Constants::ProtocolVersion;
		OutAnnounce.TrackId = Descriptor->TrackId;
		OutAnnounce.TrackName = Descriptor->TrackName;
		OutAnnounce.Properties = Descriptor->Properties;
		OutError.Reset();
		return Descriptor->TrackId;
	}

	bool FMoQTrackManager::UnpublishTrack(FMoQTrackId TrackId, FMoQUnannounceMessage& OutMessage)
	{
		FScopeLock Lock(&Mutex);
		FMoQTrackDescriptor Descriptor;
		if (!TracksById.RemoveAndCopyValue(TrackId, Descriptor))
		{
			return false;
		}

		if (const FString* Normalized = TrackIdToNormalizedName.Find(TrackId))
		{
			TracksByName.Remove(*Normalized);
			TrackIdToNormalizedName.Remove(TrackId);
		}

		OutMessage.Version = Constants::ProtocolVersion;
		OutMessage.TrackId = TrackId;
		return true;
	}

	bool FMoQTrackManager::TryGetTrackById(FMoQTrackId TrackId, FMoQTrackDescriptor& OutDescriptor) const
	{
		FScopeLock Lock(&Mutex);
		if (const FMoQTrackDescriptor* Descriptor = TracksById.Find(TrackId))
		{
			OutDescriptor = *Descriptor;
			return true;
		}
		return false;
	}

	bool FMoQTrackManager::TryGetTrackByName(const FString& TrackName, FMoQTrackDescriptor& OutDescriptor) const
	{
		const FString Normalized = NormalizeKey(TrackName);
		FScopeLock Lock(&Mutex);
		if (const FMoQTrackId* TrackId = TracksByName.Find(Normalized))
		{
			if (const FMoQTrackDescriptor* Descriptor = TracksById.Find(*TrackId))
			{
				OutDescriptor = *Descriptor;
				return true;
			}
		}
		return false;
	}

	TArray<FMoQTrackDescriptor> FMoQTrackManager::SnapshotPublishedTracks() const
	{
		FScopeLock Lock(&Mutex);
		TArray<FMoQTrackDescriptor> Snapshot;
		TracksById.GenerateValueArray(Snapshot);
		return Snapshot;
	}

	FMoQSubscriptionId FMoQTrackManager::SubscribeTrack(const FString& Pattern, const FMoQTrackProperties& RequestedProperties, FMoQSubscribeMessage& OutMessage, FString& OutError)
	{
		if (!ValidateTrackPattern(Pattern, OutError))
		{
			return 0;
		}

		FMoQTrackProperties Sanitized = RequestedProperties;
		Sanitized.ClampPriority();

		FScopeLock Lock(&Mutex);
		const FMoQSubscriptionId SubscriptionId = ReserveSubscriptionId();

		FMoQSubscriptionStateInfo& Entry = Subscriptions.FindOrAdd(SubscriptionId);
		Entry.SubscriptionId = SubscriptionId;
		Entry.Pattern = Pattern;
		Entry.NormalizedPattern = NormalizeKey(Pattern);
		Entry.RequestedProperties = Sanitized;
		Entry.State = EMoQSubscriptionState::Pending;
		Entry.LastError.Reset();
		Entry.BoundTrack = FMoQTrackDescriptor();

		OutMessage.Version = Constants::ProtocolVersion;
		OutMessage.SubscriptionId = SubscriptionId;
		OutMessage.TrackPattern = Pattern;
		OutMessage.RequestedProperties = Sanitized;
		OutError.Reset();
		return SubscriptionId;
	}

	bool FMoQTrackManager::UnsubscribeTrack(FMoQSubscriptionId SubscriptionId, FMoQUnsubscribeMessage& OutMessage, FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(SubscriptionId))
		{
			Entry->State = EMoQSubscriptionState::Closed;
			Entry->LastError = TEXT("Unsubscribed");

			OutMessage.Version = Constants::ProtocolVersion;
			OutMessage.SubscriptionId = SubscriptionId;
			OutMessage.TrackPattern = Entry->Pattern;
			OutError.Reset();
			return true;
		}

		OutError = TEXT("Unknown subscription identifier.");
		return false;
	}

	bool FMoQTrackManager::ApplySubscribeOk(const FMoQSubscribeOkMessage& Message, FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(Message.SubscriptionId))
		{
			if (!DoesPatternMatch(Entry->Pattern, Message.TrackName))
			{
				OutError = TEXT("SubscribeOk track name does not match requested pattern.");
				return false;
			}

			Entry->State = EMoQSubscriptionState::Active;
			Entry->LastError.Reset();
			Entry->BoundTrack.TrackId = Message.TrackId;
			Entry->BoundTrack.TrackName = Message.TrackName;
			Entry->BoundTrack.Properties = Message.ResolvedProperties;
			Entry->BoundTrack.Properties.ClampPriority();
			Entry->BoundTrack.LastUpdatedSeconds = FPlatformTime::Seconds();
			OutError.Reset();
			return true;
		}

		OutError = TEXT("Unknown subscription identifier for SubscribeOk.");
		return false;
	}

	bool FMoQTrackManager::ApplySubscribeError(const FMoQSubscribeErrorMessage& Message)
	{
		FScopeLock Lock(&Mutex);
		if (FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(Message.SubscriptionId))
		{
			Entry->State = EMoQSubscriptionState::Error;
			Entry->LastError = Message.ErrorReason;
			Entry->BoundTrack = FMoQTrackDescriptor();
			return true;
		}
		return false;
	}

	bool FMoQTrackManager::CloseSubscription(FMoQSubscriptionId SubscriptionId, const FString& Reason)
	{
		FScopeLock Lock(&Mutex);
		if (FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(SubscriptionId))
		{
			Entry->State = EMoQSubscriptionState::Closed;
			Entry->LastError = Reason;
			return true;
		}
		return false;
	}

	bool FMoQTrackManager::TryGetSubscription(FMoQSubscriptionId SubscriptionId, FMoQSubscriptionStateInfo& OutInfo) const
	{
		FScopeLock Lock(&Mutex);
		if (const FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(SubscriptionId))
		{
			OutInfo = *Entry;
			return true;
		}
		return false;
	}

	TArray<FMoQSubscriptionStateInfo> FMoQTrackManager::SnapshotSubscriptions() const
	{
		FScopeLock Lock(&Mutex);
		TArray<FMoQSubscriptionStateInfo> Snapshot;
		Subscriptions.GenerateValueArray(Snapshot);
		return Snapshot;
	}

	EMoQSubscriptionState FMoQTrackManager::GetSubscriptionState(FMoQSubscriptionId SubscriptionId) const
	{
		FScopeLock Lock(&Mutex);
		if (const FMoQSubscriptionStateInfo* Entry = Subscriptions.Find(SubscriptionId))
		{
			return Entry->State;
		}
		return EMoQSubscriptionState::None;
	}

	bool FMoQTrackManager::DoesPatternMatch(const FString& Pattern, const FString& TrackName)
	{
		if (Pattern.IsEmpty())
		{
			return false;
		}

		if (Pattern == TEXT("*"))
		{
			return true;
		}

		const FString NormalizedPattern = NormalizeKey(Pattern);
		const FString NormalizedName = NormalizeKey(TrackName);

		int32 PatternIndex = 0;
		int32 NameIndex = 0;
		int32 StarIndex = INDEX_NONE;
		int32 MatchIndex = 0;

		while (NameIndex < NormalizedName.Len())
		{
			if (PatternIndex < NormalizedPattern.Len() &&
				(NormalizedPattern[PatternIndex] == NormalizedName[NameIndex] || NormalizedPattern[PatternIndex] == TEXT('*')))
			{
				if (NormalizedPattern[PatternIndex] == TEXT('*'))
				{
					StarIndex = PatternIndex++;
					MatchIndex = NameIndex;
				}
				else
				{
					++PatternIndex;
					++NameIndex;
				}
			}
			else if (StarIndex != INDEX_NONE)
			{
				PatternIndex = StarIndex + 1;
				NameIndex = ++MatchIndex;
			}
			else
			{
				return false;
			}
		}

		while (PatternIndex < NormalizedPattern.Len() && NormalizedPattern[PatternIndex] == TEXT('*'))
		{
			++PatternIndex;
		}

		return PatternIndex == NormalizedPattern.Len();
	}

	FMoQTrackId FMoQTrackManager::ReserveTrackId()
	{
		FMoQTrackId Result = NextTrackId++;
		if (NextTrackId == 0)
		{
			NextTrackId = 1; // Wrap safety (reserve 0 as invalid)
		}
		return Result;
	}

	FMoQSubscriptionId FMoQTrackManager::ReserveSubscriptionId()
	{
		FMoQSubscriptionId Result = NextSubscriptionId++;
		if (NextSubscriptionId == 0)
		{
			NextSubscriptionId = 1;
		}
		return Result;
	}
}
