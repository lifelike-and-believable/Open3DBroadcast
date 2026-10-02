// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

/**
 * Shared helper functions and constants for MoQ transport.
 * Extracted from sender/receiver to reduce code duplication.
 */
namespace MoQHelpers
{
	// ─────────────────────────────────────────────────────────────────────────
	// Configuration Constants
	// ─────────────────────────────────────────────────────────────────────────
	
	/** Configuration key for relay URL */
	static constexpr TCHAR kKeyRelayUrl[] = TEXT("relay_url");
	static constexpr TCHAR kKeyRelayUrlAlt[] = TEXT("moq.relay");
	
	/** Configuration key for track namespace */
	static constexpr TCHAR kKeyTrackNamespace[] = TEXT("track_namespace");
	static constexpr TCHAR kKeyTrackNamespaceAlt[] = TEXT("moq.namespace");
	
	/** Configuration key for track name */
	static constexpr TCHAR kKeyTrackName[] = TEXT("track_name");
	static constexpr TCHAR kKeyTrackNameAlt[] = TEXT("moq.track");
	
	/** Configuration key for session ID */
	static constexpr TCHAR kKeySessionId[] = TEXT("moq.session");
	
	/** Configuration key for delivery mode */
	static constexpr TCHAR kKeyDeliveryMode[] = TEXT("delivery_mode");
	static constexpr TCHAR kKeyDeliveryModeAlt[] = TEXT("moq.delivery");
	
	/** Configuration key for queue bytes */
	static constexpr TCHAR kKeyQueueBytes[] = TEXT("queue_bytes");
	static constexpr TCHAR kKeyQueueBytesAlt[] = TEXT("moq.queue_bytes");
	static constexpr TCHAR kKeyQueueBytesAlt2[] = TEXT("moq.qbytes");

	/** Configuration key for the connect timeout in seconds (WP-S8, TRF-11) */
	static constexpr TCHAR kKeyConnectTimeout[] = TEXT("connect_timeout");
	static constexpr TCHAR kKeyConnectTimeoutAlt[] = TEXT("moq.connect_timeout");
	
	// ─────────────────────────────────────────────────────────────────────────
	// Queue Size Limits
	// ─────────────────────────────────────────────────────────────────────────
	
	/** Default queue size in bytes */
	constexpr uint64 kDefaultQueueBytes = 8ull * 1024ull * 1024ull;   // 8 MB
	
	/** Minimum queue size in bytes */
	constexpr uint64 kMinQueueBytes = 256ull * 1024ull;               // 256 KB
	
	/** Maximum queue size in bytes */
	constexpr uint64 kMaxQueueBytes = 256ull * 1024ull * 1024ull;     // 256 MB
	
	// ─────────────────────────────────────────────────────────────────────────
	// Timing Constants
	// ─────────────────────────────────────────────────────────────────────────
	
	/** Minimum reconnect delay in seconds */
	constexpr double kMinReconnectDelaySeconds = 0.5;
	
	/** Maximum reconnect delay in seconds */
	constexpr double kMaxReconnectDelaySeconds = 10.0;
	
	/** Share of a backoff delay removed by jitter: delays fall in [(1 - J) * base, base] */
	constexpr double kBackoffJitterFraction = 0.25;

	/**
	 * Default connect timeout in seconds. moq-ffi gives up after 30 s on its own; the transport
	 * abandons an attempt sooner and retries on a fresh client.
	 */
	constexpr double kDefaultConnectTimeoutSeconds = 15.0;
	constexpr double kMinConnectTimeoutSeconds = 1.0;
	constexpr double kMaxConnectTimeoutSeconds = 120.0;

	/** Interval between error log messages to avoid spam */
	constexpr double kErrorLogIntervalSeconds = 5.0;
	
	/** Interval between drop log messages */
	constexpr double kDropLogIntervalSeconds = 2.0;
	
	// ─────────────────────────────────────────────────────────────────────────
	// Track Naming Constants
	// ─────────────────────────────────────────────────────────────────────────
	
	/** Default namespace prefix for mocap tracks */
	static constexpr TCHAR kMocapNamespacePrefix[] = TEXT("mocap");
	
	/** Default namespace prefix for audio tracks */
	static constexpr TCHAR kAudioNamespacePrefix[] = TEXT("audio");

	/** Default namespace prefix for control tracks (ADR 0011) */
	static constexpr TCHAR kControlNamespacePrefix[] = TEXT("control");
	
	/** Default track name when none specified */
	static constexpr TCHAR kDefaultTrackName[] = TEXT("primary");
	
	/** Default session name when none specified */
	static constexpr TCHAR kDefaultSessionName[] = TEXT("default");
	
	/** Maximum length for namespace components */
	constexpr int32 kMaxNamespaceLength = 256;
	
	/** Maximum length for track names */
	constexpr int32 kMaxTrackNameLength = 128;
	
	// ─────────────────────────────────────────────────────────────────────────
	// Helper Functions
	// ─────────────────────────────────────────────────────────────────────────
	
	// Options are read with O3DTransportOptions (Open3DShared): keys case-insensitive, values
	// trimmed, numbers parsed strictly (WP-A1 PR 4e, TRB-26).

	/**
	 * Sanitize a string for use as a track namespace or name component.
	 * Removes non-alphanumeric characters except underscore and dash.
	 * Optionally allows forward slashes for namespace paths.
	 * 
	 * @param Value Input string
	 * @param bAllowSlash Whether to allow '/' characters
	 * @return Sanitized string
	 */
	FString SanitizeComponent(const FString& Value, bool bAllowSlash);
	
	/**
	 * Build a default namespace for mocap tracks from config.
	 * Uses track_namespace > moq.namespace > moq.session/StreamId > "mocap/default"
	 * 
	 * @param Config Transport configuration
	 * @return Namespace string (e.g., "mocap/session1")
	 */
	FString BuildDefaultMocapNamespace(const FO3DTransportConfig& Config);
	
	/**
	 * Build a default namespace for audio tracks from config.
	 * Uses "audio" prefix instead of "mocap".
	 * 
	 * @param Config Transport configuration
	 * @return Namespace string (e.g., "audio/session1")
	 */
	FString BuildDefaultAudioNamespace(const FO3DTransportConfig& Config);

	/**
	 * Build the namespace for the control track (ADR 0011) from config.
	 * Uses "control" prefix instead of "mocap". A custom namespace without a mocap/ or audio/
	 * prefix gets "control/" prepended, so control never shares the mocap track.
	 *
	 * @param Config Transport configuration
	 * @return Namespace string (e.g., "control/session1")
	 */
	FString BuildDefaultControlNamespace(const FO3DTransportConfig& Config);
	
	/**
	 * Build a default track name from config.
	 * Uses track_name > moq.track > StreamId suffix > "primary"
	 * 
	 * @param Config Transport configuration
	 * @return Track name string
	 */
	FString BuildDefaultTrackName(const FO3DTransportConfig& Config);
	
	/**
	 * Resolve the relay URL from config.
	 * Checks relay_url > moq.relay > Uri
	 * 
	 * @param Config Transport configuration
	 * @return Relay URL or empty string if not found
	 */
	FString ResolveRelayUrl(const FO3DTransportConfig& Config);
	
	/**
	 * Resolve the delivery mode from config.
	 * Checks delivery_mode > moq.delivery, defaults to stream mode.
	 * 
	 * @param Config Transport configuration
	 * @return Delivery mode enum value
	 */
	MoqDeliveryMode ResolveDeliveryMode(const FO3DTransportConfig& Config);

	/**
	 * Capabilities of the MoQ transport (ADR 0007 item 4). Delivery is Unreliable in both delivery
	 * modes until ordering across groups in stream mode is verified (ADR 0005 Q5). The same for
	 * every config today. Any thread.
	 */
	FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& Config);
	
	/**
	 * Resolve the queue size in bytes from config.
	 * Checks queue_bytes > moq.queue_bytes > moq.qbytes, defaults to 8MB.
	 * Digits only (O3DTransportOptions::TryParseInt); anything else is the default.
	 * Clamps to min/max limits.
	 * 
	 * @param Config Transport configuration
	 * @return Queue size in bytes
	 */
	uint64 ResolveQueueBytes(const FO3DTransportConfig& Config);
	
	/**
	 * Compute reconnect delay with exponential backoff.
	 * 
	 * @param ConsecutiveFailures Number of consecutive connection failures
	 * @return Delay in seconds before next reconnect attempt
	 */
	double ComputeReconnectDelaySeconds(int32 ConsecutiveFailures);

	/**
	 * Capped exponential backoff with deterministic jitter (WP-S8, TRF-11, TRF-20).
	 * The base delay is ComputeReconnectDelaySeconds(ConsecutiveFailures); jitter removes up to
	 * kBackoffJitterFraction of it. The jitter is a hash of (JitterSeed, ConsecutiveFailures),
	 * so a given seed always yields the same schedule, and different seeds spread instances out.
	 *
	 * @return Delay in [(1 - kBackoffJitterFraction) * base, base]
	 */
	double ComputeBackoffDelaySeconds(int32 ConsecutiveFailures, uint64 JitterSeed);

	/** Jitter unit in [0, 1) derived from the seed and the failure count. */
	double ComputeJitterUnit(uint64 JitterSeed, int32 ConsecutiveFailures);

	/**
	 * Resolve the connect timeout in seconds from config.
	 * Checks connect_timeout > moq.connect_timeout (O3DTransportOptions::TryParseDouble), defaults
	 * to kDefaultConnectTimeoutSeconds, clamped to [kMinConnectTimeoutSeconds, kMaxConnectTimeoutSeconds].
	 */
	double ResolveConnectTimeoutSeconds(const FO3DTransportConfig& Config);

	// The codec of a received audio payload is read with O3DAudio::TryGetAudioPayloadCodec
	// (Open3DShared), through the receive demux (WP-S8 TRF-37; WP-A1 PR 4e).
}
