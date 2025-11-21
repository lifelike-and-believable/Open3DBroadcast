// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

namespace O3DMoQ
{
	namespace Constants
	{
		static constexpr uint16 ProtocolVersion = 1;
		static constexpr int32 MaxTrackNameBytes = 512;
		static constexpr int32 MaxTrackPatternBytes = 512;
		static constexpr int32 MaxControlMetadataPairs = 16;
		static constexpr int32 MaxControlPayloadBytes = 64 * 1024;
		static constexpr int32 MaxObjectPayloadBytes = 4 * 1024 * 1024;
	}

	enum class EMoQControlMessageType : uint8
	{
		Announce = 1,
		Unannounce = 2,
		Subscribe = 3,
		Unsubscribe = 4,
		SubscribeOk = 5,
		SubscribeError = 6,
		Heartbeat = 7
	};

	enum class EMoQReliabilityMode : uint8
	{
		ReliableOrdered = 0,
		UnreliableSequenced = 1
	};

	enum class EMoQSubscriptionState : uint8
	{
		None = 0,
		Pending,
		Active,
		Error,
		Closed
	};

	using FMoQTrackId = uint32;
	using FMoQSubscriptionId = uint32;

	struct OPEN3DTRANSPORTQUIC_API FMoQTrackProperties
	{
		uint8 Priority = 128;
		EMoQReliabilityMode Reliability = EMoQReliabilityMode::ReliableOrdered;
		bool bDatagramsAllowed = true;
		bool bIsAudio = false;

		void ClampPriority();
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQAnnounceMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQTrackId TrackId = 0;
		FString TrackName;
		FMoQTrackProperties Properties;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQAnnounceMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQUnannounceMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQTrackId TrackId = 0;

		bool Serialize(TArray<uint8>& OutBuffer) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQUnannounceMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQSubscribeMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQSubscriptionId SubscriptionId = 0;
		FString TrackPattern;
		FMoQTrackProperties RequestedProperties;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQUnsubscribeMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQSubscriptionId SubscriptionId = 0;
		FString TrackPattern;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQUnsubscribeMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQSubscribeOkMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQSubscriptionId SubscriptionId = 0;
		FMoQTrackId TrackId = 0;
		FString TrackName;
		FMoQTrackProperties ResolvedProperties;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeOkMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQSubscribeErrorMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQSubscriptionId SubscriptionId = 0;
		uint16 ErrorCode = 0;
		FString ErrorReason;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeErrorMessage& OutMessage, FString& OutError);
	};

	struct OPEN3DTRANSPORTQUIC_API FMoQObjectMessage
	{
		uint16 Version = Constants::ProtocolVersion;
		FMoQTrackId TrackId = 0;
		uint32 Sequence = 0;
		uint64 TimestampMicros = 0;
		uint8 Priority = 128;
		EMoQReliabilityMode Reliability = EMoQReliabilityMode::ReliableOrdered;
		TArray<uint8> Payload;

		bool Serialize(TArray<uint8>& OutBuffer, FString& OutError) const;
		static bool Deserialize(const TArray<uint8>& Buffer, FMoQObjectMessage& OutMessage, FString& OutError);
	};

	OPEN3DTRANSPORTQUIC_API uint16 GetProtocolVersion();
	OPEN3DTRANSPORTQUIC_API FString ReliabilityToString(EMoQReliabilityMode Mode);
	OPEN3DTRANSPORTQUIC_API bool ValidateTrackName(const FString& TrackName, FString& OutError);
	OPEN3DTRANSPORTQUIC_API bool ValidateTrackPattern(const FString& Pattern, FString& OutError);
}
