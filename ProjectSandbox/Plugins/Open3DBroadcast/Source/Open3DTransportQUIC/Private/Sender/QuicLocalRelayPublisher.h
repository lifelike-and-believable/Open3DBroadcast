// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "HAL/CriticalSection.h"

#include "MoQ/MoQProtocol.h"
#include "Shared/QuicConnection.h"
#include "Shared/QuicHelpers.h"

namespace O3DQuic
{
	class OPEN3DTRANSPORTQUIC_API IQuicPublisherRelay
	{
	public:
		virtual ~IQuicPublisherRelay() = default;

		virtual bool Initialize(const FQuicSenderOptions& Options, FString& OutError) = 0;
		virtual void Shutdown() = 0;

		virtual void UpdateTrackMetadata(O3DMoQ::FMoQTrackId TrackId, const TArray<uint8>& AnnouncePayload, const TArray<uint8>& UnannouncePayload) = 0;
		virtual bool SendAnnounce(const TArray<uint8>& Payload) = 0;
		virtual bool SendUnannounce(const TArray<uint8>& Payload) = 0;

		/** Returns number of subscribers that received the payload (0 if none). */
		virtual int32 FanoutObject(const TArray<uint8>& Payload, FString& OutError) = 0;
		virtual bool HasActiveSubscribers() const = 0;
	};

	/**
	 * Local in-process relay that mirrors the behavior of a standalone MoQ relay while
	 * still running inside the Unreal process. This allows the sender to publish using
	 * the same API regardless of whether a remote relay is used.
	 */
	class OPEN3DTRANSPORTQUIC_API FLocalQuicPublisherRelay final : public IQuicPublisherRelay, public IQuicConnectionListener
	{
	public:
		FLocalQuicPublisherRelay();
		virtual ~FLocalQuicPublisherRelay() override;

		virtual bool Initialize(const FQuicSenderOptions& Options, FString& OutError) override;
		virtual void Shutdown() override;

		virtual void UpdateTrackMetadata(O3DMoQ::FMoQTrackId TrackId, const TArray<uint8>& AnnouncePayload, const TArray<uint8>& UnannouncePayload) override;
		virtual bool SendAnnounce(const TArray<uint8>& Payload) override;
		virtual bool SendUnannounce(const TArray<uint8>& Payload) override;

		virtual int32 FanoutObject(const TArray<uint8>& Payload, FString& OutError) override;
		virtual bool HasActiveSubscribers() const override;

		// IQuicConnectionListener
		virtual void OnQuicSubscriberConnected(uint32 SubscriberId) override;
		virtual void OnQuicSubscriberDisconnected(uint32 SubscriberId, const FString& Reason) override;
		virtual void OnQuicSubscriberControlMessage(uint32 SubscriberId, const TArray<uint8>& Payload) override;

	private:
		bool HandleSubscribeMessage(uint32 SubscriberId, const TArray<uint8>& Payload);
		bool HandleUnsubscribeMessage(uint32 SubscriberId, const TArray<uint8>& Payload);
		bool SendControlPayload(uint32 SubscriberId, const TArray<uint8>& Payload);
		void SetSubscriberSubscription(uint32 SubscriberId, O3DMoQ::FMoQSubscriptionId SubscriptionId, bool bSubscribed);
		TArray<uint32> SnapshotPrimarySubscribers() const;

	private:
		struct FSubscriberState
		{
			bool bConnected = false;
			bool bPrimarySubscribed = false;
			TSet<O3DMoQ::FMoQSubscriptionId> ActiveSubscriptions;
		};

	private:
		FQuicConnection Connection;
		FQuicSenderOptions ActiveOptions;
		mutable FCriticalSection SubscriberMutex;
		TMap<uint32, FSubscriberState> Subscribers;

		O3DMoQ::FMoQTrackId CurrentTrackId = 0;
		TArray<uint8> CurrentAnnouncePayload;
		TArray<uint8> CurrentUnannouncePayload;
	};
}
