// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/wire_format.h"
THIRD_PARTY_INCLUDES_END

// The envelope codec is the core's (ADR 0009 item 4); these wrap it for UE types.
static_assert(O3DS::UnifiedWireHeaderSize == static_cast<int32>(O3DS::Wire::kEnvelopeV2HeaderSize), "Envelope v2 header size");
static_assert(O3DS::UnifiedWireHeaderSizeV1 == static_cast<int32>(O3DS::Wire::kEnvelopeV1HeaderSize), "Envelope v1 header size");

namespace O3DS
{
	bool ParseUnifiedMessage(const uint8* Data, int32 Size, FUnifiedHeader& OutHeader, const uint8*& OutPayloadPtr, int32& OutPayloadSize)
	{
		if (Data == nullptr || Size <= 0)
		{
			return false;
		}
		Wire::EnvelopeHeader Header;
		if (!Wire::ReadEnvelopeHeader(Data, static_cast<size_t>(Size), Header))
		{
			return false;
		}
		FUnifiedHeader H;
		H.MagicBE = FUnifiedHeader::ReadBE32(Data);
		H.Version = Header.version;
		H.Kind = Header.kind;
		H.Codec = Header.codec;
		H.Flags = Header.flags;
		H.TimestampUsHost = Header.timestamp_us;
		H.PayloadSizeHost = Header.payload_size;
		H.Seq = Header.seq;
		H.HeaderSize = static_cast<int32>(Header.header_size);
		OutHeader = H;
		OutPayloadPtr = Data + Header.header_size;
		OutPayloadSize = static_cast<int32>(Header.payload_size);
		return true;
	}

	bool HasUnifiedEnvelopeMagic(const uint8* Data, int32 Size)
	{
		return Data != nullptr && Size > 0 && Wire::HasEnvelopeMagic(Data, static_cast<size_t>(Size));
	}

	bool WriteUnifiedHeaderInPlace(EUnifiedKind Kind, EUnifiedCodec Codec, double TimestampSec, TArray<uint8>& InOutMessage, uint32 Seq)
	{
		const int32 PayloadSize = InOutMessage.Num() - UnifiedWireHeaderSize;
		if (PayloadSize <= 0 || PayloadSize > UnifiedMaxPayloadSize)
		{
			return false;
		}
		Wire::WriteEnvelopeHeaderV2(InOutMessage.GetData(), static_cast<Wire::EnvelopeKind>(Kind), static_cast<Wire::EnvelopeCodec>(Codec),
			Wire::EnvelopeTimestampUs(TimestampSec), static_cast<uint32>(PayloadSize), Seq);
		return true;
	}

	bool CreateUnifiedMessage(EUnifiedKind Kind, EUnifiedCodec Codec, const uint8* PayloadData, int32 PayloadSize, double TimestampSec, TArray<uint8>& OutMessage, uint32 Seq)
	{
		if (!PayloadData || PayloadSize <= 0 || PayloadSize > UnifiedMaxPayloadSize)
		{
			return false;
		}
		OutMessage.SetNumUninitialized(UnifiedWireHeaderSize + PayloadSize);
		FMemory::Memcpy(OutMessage.GetData() + UnifiedWireHeaderSize, PayloadData, PayloadSize);
		return WriteUnifiedHeaderInPlace(Kind, Codec, TimestampSec, OutMessage, Seq);
	}

	bool WriteControlEnvelope(TConstArrayView<uint8> Payload, double TimestampSec, TArray<uint8>& OutMessage, uint32 Seq)
	{
		OutMessage.Reset();
		if (Payload.Num() <= 0 || Payload.Num() > UnifiedMaxControlPayloadSize)
		{
			return false;
		}
		return CreateUnifiedMessage(EUnifiedKind::Control, EUnifiedCodec::O3DControl, Payload.GetData(), Payload.Num(), TimestampSec, OutMessage, Seq);
	}

	bool TryGetControlPayload(const uint8* Data, int32 Size, TConstArrayView<uint8>& OutPayload)
	{
		OutPayload = TConstArrayView<uint8>();
		FUnifiedHeader Header;
		const uint8* PayloadPtr = nullptr;
		int32 PayloadSize = 0;
		if (!ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize))
		{
			return false;
		}
		if (Header.GetKind() != EUnifiedKind::Control || Header.GetCodec() != EUnifiedCodec::O3DControl
			|| PayloadSize <= 0 || PayloadSize > UnifiedMaxControlPayloadSize)
		{
			return false;
		}
		OutPayload = TConstArrayView<uint8>(PayloadPtr, PayloadSize);
		return true;
	}
}
