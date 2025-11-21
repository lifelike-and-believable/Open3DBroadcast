// Copyright (c) Open3DStream Contributors

#include "MoQ/MoQProtocol.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformMath.h"
#include "Misc/AssertionMacros.h"
#include "Misc/Char.h"

namespace O3DMoQ
{
	namespace
	{
		template <typename TValue>
		void WritePrimitive(TArray<uint8>& Buffer, TValue Value)
		{
			const int32 Offset = Buffer.AddUninitialized(sizeof(TValue));
			FMemory::Memcpy(Buffer.GetData() + Offset, &Value, sizeof(TValue));
		}

		template <typename TValue>
		bool ReadPrimitive(const TArray<uint8>& Buffer, int32& Offset, TValue& OutValue)
		{
			if (Offset + static_cast<int32>(sizeof(TValue)) > Buffer.Num())
			{
				return false;
			}

			FMemory::Memcpy(&OutValue, Buffer.GetData() + Offset, sizeof(TValue));
			Offset += sizeof(TValue);
			return true;
		}

		bool WriteStringField(const FString& Value, int32 MaxBytes, TArray<uint8>& Buffer, FString& OutError)
		{
			FTCHARToUTF8 Utf8Value(*Value);
			const int32 ByteLength = Utf8Value.Length();

			if (ByteLength > MaxBytes)
			{
				OutError = FString::Printf(TEXT("Field exceeds max byte length (%d > %d)."), ByteLength, MaxBytes);
				return false;
			}

			if (ByteLength > TNumericLimits<uint16>::Max())
			{
				OutError = TEXT("Field too large to serialize.");
				return false;
			}

			WritePrimitive<uint16>(Buffer, static_cast<uint16>(ByteLength));
			if (ByteLength > 0)
			{
				const int32 Offset = Buffer.AddUninitialized(ByteLength);
				FMemory::Memcpy(Buffer.GetData() + Offset, Utf8Value.Get(), ByteLength);
			}

			return true;
		}

		bool ReadStringField(const TArray<uint8>& Buffer, int32& Offset, int32 MaxBytes, FString& OutValue, FString& OutError)
		{
			uint16 ByteLength = 0;
			if (!ReadPrimitive<uint16>(Buffer, Offset, ByteLength))
			{
				OutError = TEXT("String length truncated.");
				return false;
			}

			if (ByteLength > MaxBytes)
			{
				OutError = TEXT("String length exceeds allowed maximum.");
				return false;
			}

			if (Offset + ByteLength > Buffer.Num())
			{
				OutError = TEXT("String payload truncated.");
				return false;
			}

			if (ByteLength == 0)
			{
				OutValue.Reset();
				return true;
			}

			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Buffer.GetData() + Offset), ByteLength);
			OutValue = FString(Converter.Length(), Converter.Get());
			Offset += ByteLength;
			return true;
		}

		void WriteTrackProperties(const FMoQTrackProperties& Properties, TArray<uint8>& Buffer)
		{
			WritePrimitive<uint8>(Buffer, Properties.Priority);
			WritePrimitive<uint8>(Buffer, static_cast<uint8>(Properties.Reliability));
			WritePrimitive<uint8>(Buffer, Properties.bDatagramsAllowed ? 1 : 0);
			WritePrimitive<uint8>(Buffer, Properties.bIsAudio ? 1 : 0);
		}

		bool ReadTrackProperties(const TArray<uint8>& Buffer, int32& Offset, FMoQTrackProperties& OutProperties)
		{
			uint8 Priority = 0;
			uint8 ReliabilityValue = 0;
			uint8 Datagrams = 0;
			uint8 Audio = 0;

			if (!ReadPrimitive<uint8>(Buffer, Offset, Priority) ||
				!ReadPrimitive<uint8>(Buffer, Offset, ReliabilityValue) ||
				!ReadPrimitive<uint8>(Buffer, Offset, Datagrams) ||
				!ReadPrimitive<uint8>(Buffer, Offset, Audio))
			{
				return false;
			}

			OutProperties.Priority = Priority;
			OutProperties.Reliability = (ReliabilityValue == static_cast<uint8>(EMoQReliabilityMode::UnreliableSequenced))
				? EMoQReliabilityMode::UnreliableSequenced
				: EMoQReliabilityMode::ReliableOrdered;
			OutProperties.bDatagramsAllowed = Datagrams != 0;
			OutProperties.bIsAudio = Audio != 0;
			OutProperties.ClampPriority();
			return true;
		}

		bool ValidateIdentifier(const FString& Value, int32 MaxBytes, const TCHAR* Label, bool bAllowWildcard, FString& OutError)
		{
			if (Value.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s cannot be empty."), Label);
				return false;
			}

			if (!Value.Equals(Value.TrimStartAndEnd()))
			{
				OutError = FString::Printf(TEXT("%s may not have leading or trailing whitespace."), Label);
				return false;
			}

			for (const TCHAR Char : Value)
			{
				if (Char == TEXT('*') && bAllowWildcard)
				{
					continue;
				}

				if (!FChar::IsPrint(Char))
				{
					OutError = FString::Printf(TEXT("%s contains non-printable characters."), Label);
					return false;
				}
			}

			FTCHARToUTF8 Utf8Value(*Value);
			const int32 ByteLength = Utf8Value.Length();
			if (ByteLength > MaxBytes)
			{
				OutError = FString::Printf(TEXT("%s exceeds max byte length (%d > %d)."), Label, ByteLength, MaxBytes);
				return false;
			}

			return true;
		}

		bool ValidatePayloadSize(const TArray<uint8>& Buffer, int32 MaxSize, FString& OutError)
		{
			if (Buffer.Num() > MaxSize)
			{
				OutError = FString::Printf(TEXT("Payload exceeds maximum allowed size (%d > %d)."), Buffer.Num(), MaxSize);
				return false;
			}
			return true;
		}
	}

	uint16 GetProtocolVersion()
	{
		return Constants::ProtocolVersion;
	}

	FString ReliabilityToString(EMoQReliabilityMode Mode)
	{
		switch (Mode)
		{
		case EMoQReliabilityMode::ReliableOrdered:
			return TEXT("ReliableOrdered");
		case EMoQReliabilityMode::UnreliableSequenced:
			return TEXT("UnreliableSequenced");
		default:
			return TEXT("Unknown");
		}
	}

	bool ValidateTrackName(const FString& TrackName, FString& OutError)
	{
		return ValidateIdentifier(TrackName, Constants::MaxTrackNameBytes, TEXT("Track name"), false, OutError);
	}

	bool ValidateTrackPattern(const FString& Pattern, FString& OutError)
	{
		return ValidateIdentifier(Pattern, Constants::MaxTrackPatternBytes, TEXT("Track pattern"), true, OutError);
	}

	void FMoQTrackProperties::ClampPriority()
	{
		const int32 Clamped = FMath::Clamp<int32>(Priority, 0, 255);
		Priority = static_cast<uint8>(Clamped);
	}

	bool FMoQAnnounceMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		if (!ValidateTrackName(TrackName, OutError))
		{
			return false;
		}

		OutBuffer.Reset();
		OutBuffer.Reserve(64 + TrackName.Len());

		const uint16 WireVersion = Version == 0 ? Constants::ProtocolVersion : Version;
		WritePrimitive<uint16>(OutBuffer, WireVersion);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::Announce));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, TrackId);
		WriteTrackProperties(Properties, OutBuffer);
		return WriteStringField(TrackName, Constants::MaxTrackNameBytes, OutBuffer, OutError);
	}

	bool FMoQAnnounceMessage::Deserialize(const TArray<uint8>& Buffer, FMoQAnnounceMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 TrackIdValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, TrackIdValue))
		{
			OutError = TEXT("Announce message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::Announce))
		{
			OutError = TEXT("Control type mismatch for Announce message.");
			return false;
		}

		FMoQTrackProperties Properties;
		if (!ReadTrackProperties(Buffer, Offset, Properties))
		{
			OutError = TEXT("Announce properties malformed.");
			return false;
		}

		FString TrackNameValue;
		if (!ReadStringField(Buffer, Offset, Constants::MaxTrackNameBytes, TrackNameValue, OutError))
		{
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.TrackId = TrackIdValue;
		OutMessage.TrackName = MoveTemp(TrackNameValue);
		OutMessage.Properties = Properties;
		return true;
	}

	bool FMoQUnannounceMessage::Serialize(TArray<uint8>& OutBuffer) const
	{
		OutBuffer.Reset();
		WritePrimitive<uint16>(OutBuffer, Version == 0 ? Constants::ProtocolVersion : Version);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::Unannounce));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, TrackId);
		return true;
	}

	bool FMoQUnannounceMessage::Deserialize(const TArray<uint8>& Buffer, FMoQUnannounceMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 TrackIdValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, TrackIdValue))
		{
			OutError = TEXT("Unannounce message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::Unannounce))
		{
			OutError = TEXT("Control type mismatch for Unannounce message.");
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.TrackId = TrackIdValue;
		return true;
	}

	bool FMoQSubscribeMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		if (!ValidateTrackPattern(TrackPattern, OutError))
		{
			return false;
		}

		OutBuffer.Reset();
		const uint16 WireVersion = Version == 0 ? Constants::ProtocolVersion : Version;
		WritePrimitive<uint16>(OutBuffer, WireVersion);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::Subscribe));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, SubscriptionId);
		WriteTrackProperties(RequestedProperties, OutBuffer);
		return WriteStringField(TrackPattern, Constants::MaxTrackPatternBytes, OutBuffer, OutError);
	}

	bool FMoQSubscribeMessage::Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 SubscriptionValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, SubscriptionValue))
		{
			OutError = TEXT("Subscribe message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::Subscribe))
		{
			OutError = TEXT("Control type mismatch for Subscribe message.");
			return false;
		}

		FMoQTrackProperties Properties;
		if (!ReadTrackProperties(Buffer, Offset, Properties))
		{
			OutError = TEXT("Subscribe properties malformed.");
			return false;
		}

		FString PatternValue;
		if (!ReadStringField(Buffer, Offset, Constants::MaxTrackPatternBytes, PatternValue, OutError))
		{
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.SubscriptionId = SubscriptionValue;
		OutMessage.TrackPattern = MoveTemp(PatternValue);
		OutMessage.RequestedProperties = Properties;
		return true;
	}

	bool FMoQUnsubscribeMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		if (!ValidateTrackPattern(TrackPattern, OutError))
		{
			return false;
		}

		OutBuffer.Reset();
		WritePrimitive<uint16>(OutBuffer, Version == 0 ? Constants::ProtocolVersion : Version);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::Unsubscribe));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, SubscriptionId);
		return WriteStringField(TrackPattern, Constants::MaxTrackPatternBytes, OutBuffer, OutError);
	}

	bool FMoQUnsubscribeMessage::Deserialize(const TArray<uint8>& Buffer, FMoQUnsubscribeMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 SubscriptionValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, SubscriptionValue))
		{
			OutError = TEXT("Unsubscribe message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::Unsubscribe))
		{
			OutError = TEXT("Control type mismatch for Unsubscribe message.");
			return false;
		}

		FString PatternValue;
		if (!ReadStringField(Buffer, Offset, Constants::MaxTrackPatternBytes, PatternValue, OutError))
		{
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.SubscriptionId = SubscriptionValue;
		OutMessage.TrackPattern = MoveTemp(PatternValue);
		return true;
	}

	bool FMoQSubscribeOkMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		if (!ValidateTrackName(TrackName, OutError))
		{
			return false;
		}

		OutBuffer.Reset();
		const uint16 WireVersion = Version == 0 ? Constants::ProtocolVersion : Version;
		WritePrimitive<uint16>(OutBuffer, WireVersion);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::SubscribeOk));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, SubscriptionId);
		WritePrimitive<uint32>(OutBuffer, TrackId);
		WriteTrackProperties(ResolvedProperties, OutBuffer);
		return WriteStringField(TrackName, Constants::MaxTrackNameBytes, OutBuffer, OutError);
	}

	bool FMoQSubscribeOkMessage::Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeOkMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 SubscriptionValue = 0;
		uint32 TrackIdValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, SubscriptionValue) ||
			!ReadPrimitive<uint32>(Buffer, Offset, TrackIdValue))
		{
			OutError = TEXT("SubscribeOk message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::SubscribeOk))
		{
			OutError = TEXT("Control type mismatch for SubscribeOk message.");
			return false;
		}

		FMoQTrackProperties Properties;
		if (!ReadTrackProperties(Buffer, Offset, Properties))
		{
			OutError = TEXT("SubscribeOk properties malformed.");
			return false;
		}

		FString TrackNameValue;
		if (!ReadStringField(Buffer, Offset, Constants::MaxTrackNameBytes, TrackNameValue, OutError))
		{
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.SubscriptionId = SubscriptionValue;
		OutMessage.TrackId = TrackIdValue;
		OutMessage.TrackName = MoveTemp(TrackNameValue);
		OutMessage.ResolvedProperties = Properties;
		return true;
	}

	bool FMoQSubscribeErrorMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		OutBuffer.Reset();
		const uint16 WireVersion = Version == 0 ? Constants::ProtocolVersion : Version;
		WritePrimitive<uint16>(OutBuffer, WireVersion);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(EMoQControlMessageType::SubscribeError));
		WritePrimitive<uint8>(OutBuffer, 0);
		WritePrimitive<uint32>(OutBuffer, SubscriptionId);
		WritePrimitive<uint16>(OutBuffer, ErrorCode);
		return WriteStringField(ErrorReason, Constants::MaxTrackPatternBytes, OutBuffer, OutError);
	}

	bool FMoQSubscribeErrorMessage::Deserialize(const TArray<uint8>& Buffer, FMoQSubscribeErrorMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 TypeValue = 0;
		uint8 Reserved = 0;
		uint32 SubscriptionValue = 0;
		uint16 ErrorCodeValue = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, TypeValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, Reserved) ||
			!ReadPrimitive<uint32>(Buffer, Offset, SubscriptionValue) ||
			!ReadPrimitive<uint16>(Buffer, Offset, ErrorCodeValue))
		{
			OutError = TEXT("SubscribeError message truncated.");
			return false;
		}

		if (TypeValue != static_cast<uint8>(EMoQControlMessageType::SubscribeError))
		{
			OutError = TEXT("Control type mismatch for SubscribeError message.");
			return false;
		}

		FString ReasonValue;
		if (!ReadStringField(Buffer, Offset, Constants::MaxTrackPatternBytes, ReasonValue, OutError))
		{
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.SubscriptionId = SubscriptionValue;
		OutMessage.ErrorCode = ErrorCodeValue;
		OutMessage.ErrorReason = MoveTemp(ReasonValue);
		return true;
	}

	bool FMoQObjectMessage::Serialize(TArray<uint8>& OutBuffer, FString& OutError) const
	{
		if (!ValidatePayloadSize(Payload, Constants::MaxObjectPayloadBytes, OutError))
		{
			return false;
		}

		OutBuffer.Reset();
		OutBuffer.Reserve(32 + Payload.Num());

		const uint16 WireVersion = Version == 0 ? Constants::ProtocolVersion : Version;
		WritePrimitive<uint16>(OutBuffer, WireVersion);
		WritePrimitive<uint8>(OutBuffer, static_cast<uint8>(Reliability));
		WritePrimitive<uint8>(OutBuffer, Priority);
		WritePrimitive<uint32>(OutBuffer, TrackId);
		WritePrimitive<uint32>(OutBuffer, Sequence);
		WritePrimitive<uint64>(OutBuffer, TimestampMicros);
		WritePrimitive<uint32>(OutBuffer, static_cast<uint32>(Payload.Num()));
		if (!Payload.IsEmpty())
		{
			const int32 Offset = OutBuffer.AddUninitialized(Payload.Num());
			FMemory::Memcpy(OutBuffer.GetData() + Offset, Payload.GetData(), Payload.Num());
		}

		return true;
	}

	bool FMoQObjectMessage::Deserialize(const TArray<uint8>& Buffer, FMoQObjectMessage& OutMessage, FString& OutError)
	{
		int32 Offset = 0;
		uint16 WireVersion = 0;
		uint8 ReliabilityValue = 0;
		uint8 PriorityValue = 0;
		uint32 TrackIdValue = 0;
		uint32 SequenceValue = 0;
		uint64 TimestampValue = 0;
		uint32 PayloadLength = 0;

		if (!ReadPrimitive<uint16>(Buffer, Offset, WireVersion) ||
			!ReadPrimitive<uint8>(Buffer, Offset, ReliabilityValue) ||
			!ReadPrimitive<uint8>(Buffer, Offset, PriorityValue) ||
			!ReadPrimitive<uint32>(Buffer, Offset, TrackIdValue) ||
			!ReadPrimitive<uint32>(Buffer, Offset, SequenceValue) ||
			!ReadPrimitive<uint64>(Buffer, Offset, TimestampValue) ||
			!ReadPrimitive<uint32>(Buffer, Offset, PayloadLength))
		{
			OutError = TEXT("Object message truncated.");
			return false;
		}

		if (PayloadLength > static_cast<uint32>(Constants::MaxObjectPayloadBytes))
		{
			OutError = TEXT("Object payload exceeds allowed limit.");
			return false;
		}

		if (Offset + static_cast<int32>(PayloadLength) > Buffer.Num())
		{
			OutError = TEXT("Object payload truncated.");
			return false;
		}

		OutMessage.Version = WireVersion;
		OutMessage.Reliability = (ReliabilityValue == static_cast<uint8>(EMoQReliabilityMode::UnreliableSequenced))
			? EMoQReliabilityMode::UnreliableSequenced
			: EMoQReliabilityMode::ReliableOrdered;
		OutMessage.Priority = PriorityValue;
		OutMessage.TrackId = TrackIdValue;
		OutMessage.Sequence = SequenceValue;
		OutMessage.TimestampMicros = TimestampValue;
		OutMessage.Payload.Reset();

		if (PayloadLength > 0)
		{
			OutMessage.Payload.AddUninitialized(PayloadLength);
			FMemory::Memcpy(OutMessage.Payload.GetData(), Buffer.GetData() + Offset, PayloadLength);
			Offset += PayloadLength;
		}

		return true;
	}
}
