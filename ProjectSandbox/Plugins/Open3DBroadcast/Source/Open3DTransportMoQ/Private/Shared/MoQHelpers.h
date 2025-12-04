// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DTransportTypes.h"
#include "moq_ffi.h"

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
	
	/**
	 * Get an advanced option from the transport config.
	 * Performs case-insensitive key matching and trims whitespace.
	 * 
	 * @param Config Transport configuration
	 * @param Key Option key to look up
	 * @return Option value or empty string if not found
	 */
	FString GetAdvancedOption(const FO3DTransportConfig& Config, const TCHAR* Key);
	
	/**
	 * Parse a uint64 from a string.
	 * 
	 * @param Input String to parse
	 * @param OutValue Parsed value on success
	 * @return true if parsing succeeded
	 */
	bool ParseUInt64(const FString& Input, uint64& OutValue);
	
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
	 * Resolve the queue size in bytes from config.
	 * Checks queue_bytes > moq.queue_bytes > moq.qbytes, defaults to 8MB.
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
}
