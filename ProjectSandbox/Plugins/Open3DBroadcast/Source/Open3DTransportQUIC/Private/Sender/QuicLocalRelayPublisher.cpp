// Copyright (c) Open3DStream Contributors

#include "Sender/QuicLocalRelayPublisher.h"

#include "Open3DTransportQUICLog.h"
#include "MoQ/MoQTrackManager.h"

namespace O3DQuic
{
	FLocalQuicPublisherRelay::FLocalQuicPublisherRelay() = default;

	FLocalQuicPublisherRelay::~FLocalQuicPublisherRelay()
	{
		Shutdown();
	}

	bool FLocalQuicPublisherRelay::Initialize(const FQuicSenderOptions& Options, FString& OutError)
	{
		OutError.Reset();
		ActiveOptions = Options;

		if (!Connection.InitializeServer(Options, this, OutError))
		{
			return false;
		}

		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("Local QUIC relay listening on %s:%u"),
			*Options.Endpoint.Host,
			Options.Endpoint.Port);

		return true;
	}

	void FLocalQuicPublisherRelay::Shutdown()
	{
		Connection.Shutdown();

		FScopeLock Lock(&SubscriberMutex);
		Subscribers.Reset();
	}

	void FLocalQuicPublisherRelay::UpdateTrackMetadata(O3DMoQ::FMoQTrackId TrackId, const TArray<uint8>& AnnouncePayload, const TArray<uint8>& UnannouncePayload)
	{
		CurrentTrackId = TrackId;
		CurrentAnnouncePayload = AnnouncePayload;
		CurrentUnannouncePayload = UnannouncePayload;
	}

	bool FLocalQuicPublisherRelay::SendAnnounce(const TArray<uint8>& Payload)
	{
		if (Payload.Num() == 0)
		{
			return false;
		}

		UpdateTrackMetadata(CurrentTrackId, Payload, CurrentUnannouncePayload);

		const TArray<uint32> Targets = SnapshotPrimarySubscribers();
		bool bAnySuccess = false;
		for (uint32 SubscriberId : Targets)
		{
			bAnySuccess |= SendControlPayload(SubscriberId, Payload);
		}
		return bAnySuccess;
	}

	bool FLocalQuicPublisherRelay::SendUnannounce(const TArray<uint8>& Payload)
	{
		if (Payload.Num() == 0)
		{
			return false;
		}

		CurrentUnannouncePayload = Payload;

		const TArray<uint32> Targets = SnapshotPrimarySubscribers();
		bool bAnySuccess = false;
		for (uint32 SubscriberId : Targets)
		{
			bAnySuccess |= SendControlPayload(SubscriberId, Payload);
		}
		return bAnySuccess;
	}

	int32 FLocalQuicPublisherRelay::FanoutObject(const TArray<uint8>& Payload, FString& OutError)
	{
		OutError.Reset();

		const TArray<uint32> Targets = SnapshotPrimarySubscribers();
		if (Targets.IsEmpty())
		{
			return 0;
		}

		int32 Delivered = 0;
		for (uint32 SubscriberId : Targets)
		{
			FString Error;
			if (Connection.SendObjectMessage(SubscriberId, Payload.GetData(), Payload.Num(), ActiveOptions.TrackProperties.Reliability, Error))
			{
				Delivered++;
			}
			else
			{
				UE_LOG(LogOpen3DTransportQUIC, Verbose, TEXT("Failed to deliver QUIC frame to subscriber %u: %s"), SubscriberId, *Error);
			}
		}

		if (Delivered == 0)
		{
			OutError = TEXT("No subscribers accepted object payload.");
		}

		return Delivered;
	}

	bool FLocalQuicPublisherRelay::HasActiveSubscribers() const
	{
		return !SnapshotPrimarySubscribers().IsEmpty();
	}

	void FLocalQuicPublisherRelay::OnQuicSubscriberConnected(uint32 SubscriberId)
	{
		{
			FScopeLock Lock(&SubscriberMutex);
			FSubscriberState& State = Subscribers.FindOrAdd(SubscriberId);
			State.bConnected = true;
			State.bPrimarySubscribed = false;
			State.ActiveSubscriptions.Reset();
		}

		if (CurrentAnnouncePayload.Num() > 0)
		{
			SendControlPayload(SubscriberId, CurrentAnnouncePayload);
		}

		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("QUIC subscriber %u connected to local relay."), SubscriberId);
	}

	void FLocalQuicPublisherRelay::OnQuicSubscriberDisconnected(uint32 SubscriberId, const FString& Reason)
	{
		{
			FScopeLock Lock(&SubscriberMutex);
			Subscribers.Remove(SubscriberId);
		}

		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("QUIC subscriber %u disconnected from local relay: %s"), SubscriberId, *Reason);
	}

	void FLocalQuicPublisherRelay::OnQuicSubscriberControlMessage(uint32 SubscriberId, const TArray<uint8>& Payload)
	{
		if (Payload.Num() < 3)
		{
			UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Received truncated control payload from subscriber %u"), SubscriberId);
			return;
		}

		const uint8 TypeByte = Payload[2];
		const O3DMoQ::EMoQControlMessageType ControlType = static_cast<O3DMoQ::EMoQControlMessageType>(TypeByte);
		switch (ControlType)
		{
		case O3DMoQ::EMoQControlMessageType::Subscribe:
			HandleSubscribeMessage(SubscriberId, Payload);
			break;
		case O3DMoQ::EMoQControlMessageType::Unsubscribe:
			HandleUnsubscribeMessage(SubscriberId, Payload);
			break;
		default:
			UE_LOG(LogOpen3DTransportQUIC, VeryVerbose, TEXT("Ignoring unsupported MoQ control message type %u"), TypeByte);
			break;
		}
	}

	bool FLocalQuicPublisherRelay::HandleSubscribeMessage(uint32 SubscriberId, const TArray<uint8>& Payload)
	{
		O3DMoQ::FMoQSubscribeMessage Message;
		FString Error;
		if (!O3DMoQ::FMoQSubscribeMessage::Deserialize(Payload, Message, Error))
		{
			UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Failed to parse MoQ Subscribe message: %s"), *Error);
			return false;
		}

		const bool bMatch = O3DMoQ::FMoQTrackManager::DoesPatternMatch(Message.TrackPattern, ActiveOptions.TrackName);
		if (!bMatch)
		{
			O3DMoQ::FMoQSubscribeErrorMessage ErrorMessage;
			ErrorMessage.SubscriptionId = Message.SubscriptionId;
			ErrorMessage.ErrorCode = 404;
			ErrorMessage.ErrorReason = TEXT("Track pattern did not match available QUIC track.");
			TArray<uint8> Buffer;
			if (ErrorMessage.Serialize(Buffer, Error))
			{
				SendControlPayload(SubscriberId, Buffer);
			}
			return false;
		}

		SetSubscriberSubscription(SubscriberId, Message.SubscriptionId, true);

		O3DMoQ::FMoQSubscribeOkMessage OkMessage;
		OkMessage.SubscriptionId = Message.SubscriptionId;
		OkMessage.TrackId = CurrentTrackId;
		OkMessage.TrackName = ActiveOptions.TrackName;
		OkMessage.ResolvedProperties = ActiveOptions.TrackProperties;
		TArray<uint8> Buffer;
		if (OkMessage.Serialize(Buffer, Error))
		{
			SendControlPayload(SubscriberId, Buffer);
		}

		if (CurrentAnnouncePayload.Num() > 0)
		{
			SendControlPayload(SubscriberId, CurrentAnnouncePayload);
		}

		return true;
	}

	bool FLocalQuicPublisherRelay::HandleUnsubscribeMessage(uint32 SubscriberId, const TArray<uint8>& Payload)
	{
		O3DMoQ::FMoQUnsubscribeMessage Message;
		FString Error;
		if (!O3DMoQ::FMoQUnsubscribeMessage::Deserialize(Payload, Message, Error))
		{
			UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Failed to parse MoQ Unsubscribe message: %s"), *Error);
			return false;
		}

		SetSubscriberSubscription(SubscriberId, Message.SubscriptionId, false);
		return true;
	}

	bool FLocalQuicPublisherRelay::SendControlPayload(uint32 SubscriberId, const TArray<uint8>& Payload)
	{
		if (Payload.Num() == 0)
		{
			return false;
		}

		FString Error;
		if (!Connection.SendControlMessage(SubscriberId, Payload.GetData(), Payload.Num(), Error))
		{
			UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Failed to send QUIC control payload to subscriber %u: %s"), SubscriberId, *Error);
			return false;
		}
		return true;
	}

	void FLocalQuicPublisherRelay::SetSubscriberSubscription(uint32 SubscriberId, O3DMoQ::FMoQSubscriptionId SubscriptionId, bool bSubscribed)
	{
		FScopeLock Lock(&SubscriberMutex);
		FSubscriberState& State = Subscribers.FindOrAdd(SubscriberId);
		State.bConnected = true;
		if (bSubscribed)
		{
			State.ActiveSubscriptions.Add(SubscriptionId);
			State.bPrimarySubscribed = true;
		}
		else
		{
			State.ActiveSubscriptions.Remove(SubscriptionId);
			State.bPrimarySubscribed = State.ActiveSubscriptions.Num() > 0;
		}
	}

	TArray<uint32> FLocalQuicPublisherRelay::SnapshotPrimarySubscribers() const
	{
		FScopeLock Lock(&SubscriberMutex);
		TArray<uint32> Result;
		for (const TPair<uint32, FSubscriberState>& Pair : Subscribers)
		{
			if (Pair.Value.bConnected && Pair.Value.bPrimarySubscribed)
			{
				Result.Add(Pair.Key);
			}
		}
		return Result;
	}
}
