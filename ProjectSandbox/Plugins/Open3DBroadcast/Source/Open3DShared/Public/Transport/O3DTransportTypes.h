// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "O3DRedact.h"
#include "Transport/O3DTransportOptionsView.h"

/**
 * Plain transport types shared by IOpen3DSender, IOpen3DReceiver and the transport registry
 * (ADR 0007 item 1). The old path "O3DTransportTypes.h" forwards here for one release.
 */

/** Which side of a transport a registry query or a config is about (ADR 0007 items 4 and 8). */
enum class EO3DTransportRole : uint8
{
	Sender,
	Receiver,
};

/** "Sender" or "Receiver". */
inline const TCHAR* LexToString(EO3DTransportRole Role)
{
	return Role == EO3DTransportRole::Receiver ? TEXT("Receiver") : TEXT("Sender");
}

/**
 * What a transport promises about delivery (ADR 0005 (iii)). Reported through
 * FO3DTransportCapabilities::Delivery (ADR 0007 item 4, WP-A1 PR 3). Unknown is treated as
 * Unreliable, so residual coding is used only on ReliableOrdered transports.
 */
enum class EO3DDeliveryGuarantee : uint8
{
	Unknown,
	/** Messages may be lost, duplicated or reordered. */
	Unreliable,
	/** Every accepted message arrives once and in send order while the connection lasts. */
	ReliableOrdered,
};

/** Error codes of FO3DTransportResult (ADR 0007 item 3). */
enum class EO3DTransportError : uint8
{
	None,
	/** The config is missing a value or has one the transport cannot use. */
	InvalidConfig,
	/** Receiver Start() without a frame consumer (SetConsumer). */
	NoConsumer,
	/** The call needs Initialize (or Start) first. */
	NotRunning,
	/** A library, socket subsystem, thread or other resource the transport needs is missing. */
	ResourceUnavailable,
	/** The local address or port is taken. */
	AddressInUse,
	/** Connecting, binding or listening failed. */
	ConnectFailed,
	/** Credentials were missing or refused. */
	AuthFailed,
	Timeout,
	/** The transport does not support the requested mode in this build or with this config. */
	Unsupported,
	Internal,
};

/**
 * Result of a lifecycle call (Initialize, Start) and the reason given with a connection-state
 * change (ADR 0007 item 3, WP-A1 PR 3; SHR-14). Message is for logs and the UI and never contains a
 * secret. Per-frame calls return EO3DSendResult instead, so the frame path allocates no string.
 */
struct FO3DTransportResult
{
	EO3DTransportError Code = EO3DTransportError::None;
	FString Message;

	bool IsOk() const { return Code == EO3DTransportError::None; }

	/** Lets callers write `if (!Sender->Start())`; use IsOk() and Code where the reason matters. */
	explicit operator bool() const { return IsOk(); }

	static FO3DTransportResult Ok()
	{
		return FO3DTransportResult();
	}

	static FO3DTransportResult Error(EO3DTransportError InCode, FString InMessage = FString())
	{
		FO3DTransportResult Result;
		Result.Code = InCode;
		Result.Message = MoveTemp(InMessage);
		return Result;
	}
};

/** Result of one SendSerialized or SendControl call (ADR 0007 item 3). */
enum class EO3DSendResult : uint8
{
	/** Accepted: queued for the transport's worker, or already handed to the network. */
	Queued,
	/** Dropped because the transport's queue or channel is full. A dropped frame counts in DroppedFrames. */
	DroppedBackpressure,
	/** Not initialized, not started, or stopped. */
	NotRunning,
	/** Running, but there is no peer or session to send to yet (e.g. a TCP sender with no receiver). */
	NotConnected,
	/** Empty, malformed, or (SendControl) not a control envelope. */
	Invalid,
	/**
	 * Larger than the transport can carry (FO3DTransportCapabilities::MaxPayloadBytes). Not in
	 * ADR 0007's list; added so an oversize frame is told apart from other invalid input.
	 */
	TooLarge,
	/**
	 * The transport does not carry this kind of message, or not in its current role (SendControl
	 * on a transport without control, or on a WebRTC subscriber). Not in ADR 0007's list.
	 */
	Unsupported,
};

/**
 * Connection state of a sender or receiver (ADR 0007 item 3). Reported by GetConnectionState()
 * and FO3DTransportStats::State, and announced through the state-changed callback.
 */
enum class EO3DConnectionState : uint8
{
	/** Not started, or stopped. */
	Idle,
	/** Started and waiting for a first peer, session or connection. */
	Connecting,
	/** Able to deliver: a peer or session exists, or the transport needs none (UDP, Loopback). */
	Connected,
	/** Was connected, lost it, and is waiting for or retrying a connection. */
	Reconnecting,
	/** Start failed or the transport gave up. Stop and Start (or Initialize) again to retry. */
	Failed,
};

/**
 * Called on every connection-state change with the new state and the reason (Ok for a normal
 * transition). Runs on the thread that made the change: the calling (game) thread, before the
 * call returns, for changes made by Initialize, Start and Stop; a worker, socket or FFI thread
 * otherwise. Calls are serialised per transport instance and arrive in state order. The callback
 * must not call back into the transport. See FO3DConnectionStateTracker.
 */
using FO3DConnectionStateCallback = TFunction<void(EO3DConnectionState /*NewState*/, const FO3DTransportResult& /*Reason*/)>;

/** Called when a sender gains a new peer (ADR 0005 (vi)); see IOpen3DSender::SetPeerJoinedCallback. */
using FO3DPeerJoinedCallback = TFunction<void()>;

/**
 * What a transport can do with a given config (ADR 0007 item 4, WP-A1 PR 3). Returned by
 * FO3DTransportDescriptor::GetCapabilities and by GetCapabilities() on a sender or receiver,
 * which reports the same values for the config it was initialized with. It absorbs ADR 0005's
 * delivery guarantee (Delivery) and ADR 0011's SupportsControl (bControl).
 */
struct FO3DTransportCapabilities
{
	/** The transport has a sender. */
	bool bSend = false;
	/** The transport has a receiver. */
	bool bReceive = false;
	/** The sender takes PCM through CreateAudioSink. */
	bool bAudioSend = false;
	/** The receiver delivers PCM through SetAudioSink. */
	bool bAudioReceive = false;
	/** Sender and receiver carry control messages (ADR 0011). Not in ADR 0007's list; ADR 0011 item 6 moves SupportsControl here. */
	bool bControl = false;
	/** The connection can carry data from receiver to sender (TCP, WebRTC, NNG pair). Nothing uses the back-channel in v1. */
	bool bBidirectional = false;
	/** The sender reports a newly joined peer through IOpen3DSender::SetPeerJoinedCallback (ADR 0005 (vi)): TCP and NNG. */
	bool bPeerJoinSignal = false;
	/** Delivery guarantee for mocap frames, with ADR 0005 (iii)'s values. */
	EO3DDeliveryGuarantee Delivery = EO3DDeliveryGuarantee::Unknown;
	/** Largest SendSerialized payload in bytes; 0 means the transport sets no limit of its own. */
	int32 MaxPayloadBytes = 0;

	bool operator==(const FO3DTransportCapabilities& Other) const
	{
		return bSend == Other.bSend && bReceive == Other.bReceive && bAudioSend == Other.bAudioSend
			&& bAudioReceive == Other.bAudioReceive && bControl == Other.bControl && bBidirectional == Other.bBidirectional
			&& bPeerJoinSignal == Other.bPeerJoinSignal && Delivery == Other.Delivery && MaxPayloadBytes == Other.MaxPayloadBytes;
	}

	bool operator!=(const FO3DTransportCapabilities& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * One serialized frame for IOpen3DSender::SendSerialized (ADR 0007 item 3). The transport takes
 * ownership of Bytes. Subject names the subject the frame carries, for stats, logs and
 * per-subject transport metadata. It stays an FString (ADR 0007 names an FName) because some
 * transports put it on the wire, and an FName can come back with another casing.
 */
struct FO3DSendPayload
{
	TArray<uint8> Bytes;
	FString Subject;
	/** Capture time already encoded in Bytes (FPlatformTime clock); transports reuse it for local latency metadata. */
	double CaptureTimeSec = 0.0;
	/** The frame is a full sync (ADR 0005 (ii)). Set by the sender pipeline once ADR 0008 lands; no transport reads it yet. */
	bool bFullSync = false;

	FO3DSendPayload() = default;

	FO3DSendPayload(TArray<uint8>&& InBytes, FString InSubject, double InCaptureTimeSec, bool bInFullSync = false)
		: Bytes(MoveTemp(InBytes))
		, Subject(MoveTemp(InSubject))
		, CaptureTimeSec(InCaptureTimeSec)
		, bFullSync(bInFullSync)
	{
	}

	/** A payload holding a copy of Data. */
	static FO3DSendPayload MakeCopy(const uint8* Data, int32 Len, FString InSubject = FString(), double InCaptureTimeSec = 0.0, bool bInFullSync = false)
	{
		TArray<uint8> Copy;
		if (Data != nullptr && Len > 0)
		{
			Copy.Append(Data, Len);
		}
		return FO3DSendPayload(MoveTemp(Copy), MoveTemp(InSubject), InCaptureTimeSec, bInFullSync);
	}
};

namespace O3DTransport
{
	/** True for EO3DSendResult::Queued. */
	inline bool IsAccepted(EO3DSendResult Result)
	{
		return Result == EO3DSendResult::Queued;
	}
}

inline const TCHAR* LexToString(EO3DTransportError Code)
{
	switch (Code)
	{
	case EO3DTransportError::None: return TEXT("None");
	case EO3DTransportError::InvalidConfig: return TEXT("InvalidConfig");
	case EO3DTransportError::NoConsumer: return TEXT("NoConsumer");
	case EO3DTransportError::NotRunning: return TEXT("NotRunning");
	case EO3DTransportError::ResourceUnavailable: return TEXT("ResourceUnavailable");
	case EO3DTransportError::AddressInUse: return TEXT("AddressInUse");
	case EO3DTransportError::ConnectFailed: return TEXT("ConnectFailed");
	case EO3DTransportError::AuthFailed: return TEXT("AuthFailed");
	case EO3DTransportError::Timeout: return TEXT("Timeout");
	case EO3DTransportError::Unsupported: return TEXT("Unsupported");
	case EO3DTransportError::Internal: return TEXT("Internal");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EO3DSendResult Result)
{
	switch (Result)
	{
	case EO3DSendResult::Queued: return TEXT("Queued");
	case EO3DSendResult::DroppedBackpressure: return TEXT("DroppedBackpressure");
	case EO3DSendResult::NotRunning: return TEXT("NotRunning");
	case EO3DSendResult::NotConnected: return TEXT("NotConnected");
	case EO3DSendResult::Invalid: return TEXT("Invalid");
	case EO3DSendResult::TooLarge: return TEXT("TooLarge");
	case EO3DSendResult::Unsupported: return TEXT("Unsupported");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EO3DConnectionState State)
{
	switch (State)
	{
	case EO3DConnectionState::Idle: return TEXT("Idle");
	case EO3DConnectionState::Connecting: return TEXT("Connecting");
	case EO3DConnectionState::Connected: return TEXT("Connected");
	case EO3DConnectionState::Reconnecting: return TEXT("Reconnecting");
	case EO3DConnectionState::Failed: return TEXT("Failed");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EO3DDeliveryGuarantee Delivery)
{
	switch (Delivery)
	{
	case EO3DDeliveryGuarantee::Unknown: return TEXT("Unknown");
	case EO3DDeliveryGuarantee::Unreliable: return TEXT("Unreliable");
	case EO3DDeliveryGuarantee::ReliableOrdered: return TEXT("ReliableOrdered");
	default: return TEXT("Unknown");
	}
}

/** "Ok", or "<Code>: <Message>". For logs. */
inline FString LexToString(const FO3DTransportResult& Result)
{
	if (Result.IsOk())
	{
		return TEXT("Ok");
	}
	return Result.Message.IsEmpty() ? FString(LexToString(Result.Code)) : FString::Printf(TEXT("%s: %s"), LexToString(Result.Code), *Result.Message);
}

/** Lightweight audio configuration shared between the sender component and transports. */ 
struct FO3DTransportAudioConfig
{
    /** Toggle audio capture/sending. */
    bool bEnableAudio = false;

    /** Preferred audio codec identifier (e.g. "PCM16", "Opus"). Empty uses transport default. */
    FString Codec;

    /** Target PCM sample rate the transport expects from the capture path. */
    int32 SampleRate = 48000;

    /** Target channel count (1 = mono, 2 = stereo). */
    int32 NumChannels = 1;

    /** Desired encoded bitrate in kbps (transport may clamp). */
    int32 BitrateKbps = 64;

    /** Optional transport-specific capture mode hint (e.g. "mix", "input"). */
    FString Mode;

    /** Optional input device identifier when capturing microphone input. */
    FString InputDevice;

    /** Additional transport specific overrides. */
    TMap<FString, FString> AdvancedParams;

    FString ToDebugString() const
    {
        return FString::Printf(TEXT("[Enabled=%d Codec=%s SR=%d Ch=%d Bitrate=%d Mode=%s Device=%s Adv=%d]"),
            bEnableAudio ? 1 : 0,
            Codec.IsEmpty() ? TEXT("<default>") : *Codec,
            SampleRate,
            NumChannels,
            BitrateKbps,
            *Mode,
            *InputDevice,
            AdvancedParams.Num());
    }
};

/**
 * Canonical configuration parameters used by transport implementations. Values are intentionally
 * high-level; transports may choose to interpret/augment them as needed.
 *
 * WP-A1 PR 5a (ADR 0007 item 8, SHR-36): the LiveKit fields (Token, bUseAutoTokenFetch,
 * TokenEndpointUrl, TokenRefreshLeadTimeSec), Backend and bPersistToken are gone. WebRTC declares
 * them as options in its schema ("webrtc.useAutoTokenFetch", ...) and reads its token from Secrets.
 * Not a USTRUCT and never saved, so removing fields loses no saved data: what users save is the
 * option map of the sender component or receiver source.
 */
struct FO3DTransportConfig
{
    FO3DTransportConfig() = default;

    /** A config for the registered transport InTransport in role InRole (TRB-27). */
    FO3DTransportConfig(FName InTransport, EO3DTransportRole InRole)
        : Transport(InTransport)
        , Role(InRole)
    {
    }

    /**
     * The registered transport name ("TCP", "UDP", "NNG", "MoQ", "Loopback", "WebRTC"), the same
     * name the registry, the pickers and the secret store use (TRB-27, ADR 0007 item 8, WP-A1
     * PR 5c). FName, so it compares case-insensitively, like the registry's lookups.
     */
    FName Transport;

    /**
     * Which side this config is for (TRB-27, WP-A1 PR 5c). The sender component sets Sender and
     * the receiver source Receiver. Transport-specific roles (an NNG socket's listen or dial
     * side) are options of their own ("nng.role"), not this field.
     */
    EO3DTransportRole Role = EO3DTransportRole::Sender;

    /** Canonical URI representation for the endpoint. */
    FString Uri;

    /** Optional secondary identifier (room name, stream id, channel name, etc.). */
    FString StreamId;

    /**
     * The sender component's Subject Name; empty for a receiver (WP-A1 PR 5a). Set by the host
     * before the configure function runs, so a transport can use it as a default without reading
     * the component (MoQ: the default stream id, and so the default track name). Not parsed by
     * any transport that reads StreamId (NNG reads a host, port and topic from StreamId).
     */
    FString SubjectName;

    /**
     * Values of the option keys the transport customization declares secret, resolved from
     * FO3DSecretStore (session, environment, then per-user settings) when the config is built.
     * Never copied into AdvancedParams, never persisted, never logged.
     */
    TMap<FString, FString> Secrets;

    /**
     * The role's option values (the namespaced option map, secrets excluded), as the host copied
     * them before the configure function ran; the configure function may add to them. Keys are
     * case-insensitive. Read them through GetOptions() or the O3DTransportOptions getters. The
     * name is kept from before WP-A1 PR 5a so code and tests that fill a config directly still
     * work.
     */
    TMap<FString, FString> AdvancedParams;

    /**
     * The schema the transport declared for this role (FO3DTransportRoleOptions::OptionSchema),
     * shared with the registered descriptor, or null when the config was not built by a host.
     * GetOptions() uses it for defaults and visibility.
     */
    TSharedPtr<const FO3DTransportOptionSchema> OptionSchema;

    /** A view of AdvancedParams with OptionSchema. Valid while this config is alive and unchanged. */
    FO3DTransportOptionsView GetOptions() const
    {
        return FO3DTransportOptionsView(AdvancedParams, OptionSchema.Get());
    }

    /** Optional audio configuration shared with transports that support audio. */
    FO3DTransportAudioConfig Audio;

    /**
     * Build a concise debug string summarising the configuration. Secrets print as key and
     * presence only; AdvancedParams values whose key looks sensitive and URL query values,
     * user-info and fragments are replaced by "<redacted>" (ADR 0004 item 7).
     */
    FString ToDebugString() const
    {
        FString ParamsSummary;
        if (AdvancedParams.Num() > 0)
        {
            TArray<FString> Pairs;
            Pairs.Reserve(AdvancedParams.Num());
            for (const TPair<FString, FString>& Pair : AdvancedParams)
            {
                Pairs.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *O3DRedact::Value(Pair.Key, O3DRedact::Url(Pair.Value))));
            }
            ParamsSummary = FString::Join(Pairs, TEXT(","));
        }

        FString SecretsSummary;
        if (Secrets.Num() > 0)
        {
            TArray<FString> Keys;
            Keys.Reserve(Secrets.Num());
            for (const TPair<FString, FString>& Pair : Secrets)
            {
                Keys.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, Pair.Value.IsEmpty() ? TEXT("<empty>") : TEXT("<set>")));
            }
            SecretsSummary = FString::Join(Keys, TEXT(","));
        }

        FString AudioSummary;
        if (Audio.bEnableAudio)
        {
            AudioSummary = Audio.ToDebugString();
        }
        else
        {
            AudioSummary = TEXT("[Enabled=0]");
        }

        return FString::Printf(TEXT("[Transport=%s Role=%s Uri=%s StreamId=%s Advanced={%s} Secrets={%s} Audio=%s]"),
            *Transport.ToString(),
            LexToString(Role),
            *O3DRedact::Url(Uri),
            *O3DRedact::Url(StreamId),
            *ParamsSummary,
            *SecretsSummary,
            *AudioSummary);
    }
};

/** Lightweight stats aggregated by transports for diagnostics. */
struct FO3DTransportStats
{
    int64 FramesSent = 0;
    int64 FramesReceived = 0;
    int64 BytesSent = 0;
    int64 BytesReceived = 0;
    int64 DroppedFrames = 0;
    double AverageLatencyMs = 0.0;
    double MaxLatencyMs = 0.0;

    // ADR 0007 item 3 (WP-A1 PR 3). Appended, so the fields above keep their offsets.

    /** Connection state when the snapshot was taken (GetConnectionState()). */
    EO3DConnectionState State = EO3DConnectionState::Idle;
    /** Sends that failed after they were accepted (0 where the transport does not count them yet). */
    int64 SendErrors = 0;
    /** Received data that was malformed or failed to decode (0 where the transport does not count it yet). */
    int64 ReceiveErrors = 0;
    /** Items waiting in the transport's send queue (0 where the transport has none or does not report it yet; the shared queue of WP-A1 step 4 reports it everywhere). */
    int64 PendingFrames = 0;
    /** Bytes waiting in the transport's send queue (as PendingFrames). */
    int64 PendingBytes = 0;

    void Reset()
    {
        FramesSent = 0;
        FramesReceived = 0;
        BytesSent = 0;
        BytesReceived = 0;
        DroppedFrames = 0;
        AverageLatencyMs = 0.0;
        MaxLatencyMs = 0.0;
        State = EO3DConnectionState::Idle;
        SendErrors = 0;
        ReceiveErrors = 0;
        PendingFrames = 0;
        PendingBytes = 0;
    }
};
