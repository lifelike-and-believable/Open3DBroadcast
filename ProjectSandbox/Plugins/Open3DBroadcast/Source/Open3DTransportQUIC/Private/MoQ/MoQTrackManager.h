// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "MoQ/MoQProtocol.h"

namespace O3DMoQ
{
	struct OPEN3DTRANSPORTQUIC_API FMoQTrackDescriptor
	{
		FMoQTrackId TrackId = 0;
		FString TrackName;
		FMoQTrackProperties Properties;
		double LastUpdatedSeconds = 0.0;
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQSubscriptionStateInfo
	{
		FMoQSubscriptionId SubscriptionId = 0;
		FString Pattern;
		FString NormalizedPattern;
		FMoQTrackProperties RequestedProperties;
		EMoQSubscriptionState State = EMoQSubscriptionState::Pending;
		FString LastError;
		FMoQTrackDescriptor BoundTrack;
	};

	class OPEN3DTRANSPORTQUIC_API FMoQTrackManager
	{
	public:
		FMoQTrackManager();

		FMoQTrackId PublishTrack(const FString& TrackName, const FMoQTrackProperties& Properties, FMoQAnnounceMessage& OutAnnounce, FString& OutError);
		bool UnpublishTrack(FMoQTrackId TrackId, FMoQUnannounceMessage& OutMessage);

		bool TryGetTrackById(FMoQTrackId TrackId, FMoQTrackDescriptor& OutDescriptor) const;
		bool TryGetTrackByName(const FString& TrackName, FMoQTrackDescriptor& OutDescriptor) const;
		TArray<FMoQTrackDescriptor> SnapshotPublishedTracks() const;

		FMoQSubscriptionId SubscribeTrack(const FString& Pattern, const FMoQTrackProperties& RequestedProperties, FMoQSubscribeMessage& OutMessage, FString& OutError);
		bool UnsubscribeTrack(FMoQSubscriptionId SubscriptionId, FMoQUnsubscribeMessage& OutMessage, FString& OutError);

		bool ApplySubscribeOk(const FMoQSubscribeOkMessage& Message, FString& OutError);
		bool ApplySubscribeError(const FMoQSubscribeErrorMessage& Message);
		bool CloseSubscription(FMoQSubscriptionId SubscriptionId, const FString& Reason);

		bool TryGetSubscription(FMoQSubscriptionId SubscriptionId, FMoQSubscriptionStateInfo& OutInfo) const;
		TArray<FMoQSubscriptionStateInfo> SnapshotSubscriptions() const;
		EMoQSubscriptionState GetSubscriptionState(FMoQSubscriptionId SubscriptionId) const;

		static bool DoesPatternMatch(const FString& Pattern, const FString& TrackName);

	private:
		FMoQTrackId ReserveTrackId();
		FMoQSubscriptionId ReserveSubscriptionId();

	private:
		mutable FCriticalSection Mutex;
		TMap<FMoQTrackId, FMoQTrackDescriptor> TracksById;
		TMap<FString, FMoQTrackId> TracksByName;
		TMap<FMoQTrackId, FString> TrackIdToNormalizedName;
		TMap<FMoQSubscriptionId, FMoQSubscriptionStateInfo> Subscriptions;
		FMoQTrackId NextTrackId = 1;
		FMoQSubscriptionId NextSubscriptionId = 1;
	};
}
