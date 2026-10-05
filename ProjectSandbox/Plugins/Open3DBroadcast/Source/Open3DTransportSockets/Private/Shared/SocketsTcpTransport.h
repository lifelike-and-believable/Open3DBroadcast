// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/tcp_stream_parser.h"
THIRD_PARTY_INCLUDES_END

namespace O3DSockets::Tcp
{
	/**
	 * Shared TCP framing helpers used by the sockets sender/receiver implementations.
	 * The layout and the stream parser live in the core (src/o3ds/tcp_stream_parser.h) so they
	 * are unit-tested and fuzzed there (WP-S6, ADR 0006 S2).
	 */
	inline constexpr int32 FrameMagicSize = static_cast<int32>(O3DS::kTcpFrameMagicSize);
	inline constexpr int32 FrameHeaderSize = static_cast<int32>(O3DS::kTcpFrameHeaderSize);

	inline void WriteFrameHeader(uint8* Destination, int32 PayloadSize)
	{
		check(Destination != nullptr);
		check(PayloadSize >= 0);
		O3DS::writeTcpFrameHeader(Destination, static_cast<uint32>(PayloadSize));
	}

	/** Size of the unified-envelope keepalive payload (see MakeKeepalivePayload). */
	inline constexpr int32 KeepalivePayloadSize = O3DS::UnifiedWireHeaderSize;

	/**
	 * The payload of the keepalive the sender writes when it has had nothing to send for
	 * tcp.keepalive milliseconds (TRB-6), framed like any other payload: an envelope header
	 * (magic "O3DU", version 2) with kind Audio and a zero payload size, so the receiver's idle
	 * timer is reset and it does not reconnect while the sender is idle. The sender never
	 * produces an audio envelope with an empty payload otherwise (CreateUnifiedMessage rejects
	 * one), so receivers recognise it unambiguously (FO3DUnifiedReceiveDemux: Keepalive).
	 */
	inline TArray<uint8> MakeKeepalivePayload()
	{
		TArray<uint8> Payload;
		Payload.SetNumZeroed(KeepalivePayloadSize);
		uint8* Envelope = Payload.GetData();
		Envelope[0] = 'O';
		Envelope[1] = '3';
		Envelope[2] = 'D';
		Envelope[3] = 'U';
		Envelope[4] = 2; // envelope version
		Envelope[5] = static_cast<uint8>(O3DS::EUnifiedKind::Audio);
		// codec, flags, timestamp, payload size and sequence stay zero.
		return Payload;
	}

	/** Receiver: payload bytes received and not yet handed to Poll; at least one tcp.maxframe frame always fits. */
	inline constexpr int32 DefaultReceiveQueueBytes = 8 * 1024 * 1024;

	/** Advanced option keys and defaults for the TCP transport (documented in Transport_Module_Comparison.md). */
	/** Receiver: seconds to wait for a connect to complete before retrying (TRB-5). */
	static constexpr TCHAR ConnectTimeoutOptionKey[] = TEXT("tcp.connecttimeout");
	inline constexpr int32 DefaultConnectTimeoutSeconds = 5;

	/** Receiver: largest frame payload accepted, in bytes (TRB-9). */
	static constexpr TCHAR MaxFrameOptionKey[] = TEXT("tcp.maxframe");
	inline constexpr int32 DefaultMaxFrameBytes = static_cast<int32>(O3DS::kTcpDefaultMaxPayload);
	inline constexpr int32 MaxFrameBytesLimit = static_cast<int32>(O3DS::kTcpMaxPayloadLimit);
	inline constexpr int32 MinFrameBytes = 1024;

	/** Receiver: first reconnect delay, in milliseconds; doubles on each failed attempt (TRB-4). */
	static constexpr TCHAR BackoffOptionKey[] = TEXT("tcp.backoff");
	inline constexpr int32 DefaultBackoffMs = 500;

	/** Receiver: longest reconnect delay, in milliseconds (TRB-4). */
	static constexpr TCHAR MaxBackoffOptionKey[] = TEXT("tcp.maxbackoff");
	inline constexpr int32 DefaultMaxBackoffMs = 5000;

	/** Sender: queue cap in bytes; frames beyond it are dropped whole (TRB-14). */
	static constexpr TCHAR MaxQueueOptionKey[] = TEXT("tcp.maxqueue");
	inline constexpr int32 DefaultMaxQueueBytes = 4 * 1024 * 1024;
	inline constexpr int32 MinQueueBytes = 64 * 1024;

	/** Sender: frames older than this many milliseconds are dropped before sending; 0 disables (TRB-14). */
	static constexpr TCHAR MaxQueueAgeOptionKey[] = TEXT("tcp.maxqueueage");
	inline constexpr int32 DefaultMaxQueueAgeMs = 1000;

	/** Sender: drop the client when a frame makes no progress for this many milliseconds (TRB-2). */
	static constexpr TCHAR StallTimeoutOptionKey[] = TEXT("tcp.stalltimeout");
	inline constexpr int32 DefaultStallTimeoutMs = 2000;
	inline constexpr int32 MinStallTimeoutMs = 100;

	/** Sender: send a keepalive after this many idle milliseconds; 0 disables (TRB-6). */
	static constexpr TCHAR KeepaliveOptionKey[] = TEXT("tcp.keepalive");
	inline constexpr int32 DefaultKeepaliveMs = 1000;
}
