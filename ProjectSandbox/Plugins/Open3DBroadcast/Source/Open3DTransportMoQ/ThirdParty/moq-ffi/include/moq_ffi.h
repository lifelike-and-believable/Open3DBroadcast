/*
 * MoQ FFI - C API for Media over QUIC Transport
 * 
 * This header provides a C interface to the Rust moq-transport library.
 * It enables C++ applications (including Unreal Engine plugins) to use
 * the MoQ protocol for low-latency media streaming.
 */

#ifndef MOQ_FFI_H
#define MOQ_FFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────────────────────────────
 * Platform-specific exports
 * ─────────────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
    #ifdef MOQ_FFI_EXPORTS
        #define MOQ_API __declspec(dllexport)
    #else
        #define MOQ_API __declspec(dllimport)
    #endif
#else
    #define MOQ_API __attribute__((visibility("default")))
#endif

/* ───────────────────────────────────────────────
 * Types
 * ─────────────────────────────────────────────── */

/**
 * Opaque handle to a MoQ client session
 */
typedef struct MoqClient MoqClient;

/**
 * Opaque handle to a MoQ publisher
 */
typedef struct MoqPublisher MoqPublisher;

/**
 * Opaque handle to a MoQ subscriber
 */
typedef struct MoqSubscriber MoqSubscriber;

/**
 * Result code for MoQ operations
 */
typedef enum {
    MOQ_OK = 0,
    MOQ_ERROR_INVALID_ARGUMENT = 1,
    MOQ_ERROR_CONNECTION_FAILED = 2,
    MOQ_ERROR_NOT_CONNECTED = 3,
    MOQ_ERROR_TIMEOUT = 4,
    MOQ_ERROR_INTERNAL = 5,
    MOQ_ERROR_UNSUPPORTED = 6,
    MOQ_ERROR_BUFFER_TOO_SMALL = 7,
} MoqResultCode;

/**
 * Result structure
 */
typedef struct {
    MoqResultCode code;
    const char* message;  // Owned by FFI layer, free with moq_free_str()
} MoqResult;

/**
 * Connection state callback
 */
typedef enum {
    MOQ_STATE_DISCONNECTED = 0,
    MOQ_STATE_CONNECTING = 1,
    MOQ_STATE_CONNECTED = 2,
    MOQ_STATE_FAILED = 3,
} MoqConnectionState;

/**
 * Delivery mode for data transmission
 */
typedef enum {
    MOQ_DELIVERY_DATAGRAM = 0,  // Lossy, for high-frequency updates
    MOQ_DELIVERY_STREAM = 1,    // Reliable, for critical data
} MoqDeliveryMode;

/* ───────────────────────────────────────────────
 * Callbacks
 * ─────────────────────────────────────────────── */

/**
 * Connection state change callback
 * @param user_data User-provided context pointer
 * @param state New connection state
 */
typedef void (*MoqConnectionCallback)(void* user_data, MoqConnectionState state);

/**
 * Data received callback
 * @param user_data User-provided context pointer
 * @param data Pointer to received data buffer
 * @param data_len Length of received data
 */
typedef void (*MoqDataCallback)(void* user_data, const uint8_t* data, size_t data_len);

/**
 * Track announcement callback
 * @param user_data User-provided context pointer
 * @param namespace Namespace of the announced track
 * @param track_name Name of the announced track
 */
typedef void (*MoqTrackCallback)(void* user_data, const char* namespace_str, const char* track_name);

/* ───────────────────────────────────────────────
 * Initialization
 * ─────────────────────────────────────────────── */

/**
 * Initialize the MoQ FFI crypto provider
 *
 * Must be called during module initialization before any TLS operations.
 * Safe to call multiple times - subsequent calls are no-ops.
 *
 * This ensures the rustls crypto provider is installed in the process
 * before any TLS connections are attempted.
 *
 * @return true on success (always succeeds)
 * 
 * @note Thread-safe: can be called from any thread
 * @note Idempotent: safe to call multiple times
 * @note Available since: v0.1.0
 * 
 * Example usage:
 * @code
 *   // C++: Call immediately after DLL load / module initialization
 *   moq_init();
 *   
 *   // ... then later ...
 *   // Now safe to create clients and connect
 *   MoqClient* client = moq_client_create();
 * @endcode
 */
MOQ_API bool moq_init(void);

/* ───────────────────────────────────────────────
 * Client Management
 * ─────────────────────────────────────────────── */

/**
 * Create a new MoQ client instance
 * @return Handle to the client or NULL on failure
 */
MOQ_API MoqClient* moq_client_create(void);

/**
 * Destroy a MoQ client and free resources
 * @param client Client handle
 */
MOQ_API void moq_client_destroy(MoqClient* client);

/**
 * Connect to a MoQ relay server
 * 
 * Supported URL schemes:
 * - https:// - WebTransport over QUIC (Draft 07 and Draft 14)
 * 
 * Future enhancements (Draft 14):
 * - quic:// - Raw QUIC connection (planned)
 * 
 * @param client Client handle
 * @param url Connection URL (e.g., "https://relay.example.com:443")
 * @param connection_callback Optional callback for connection state changes
 * @param user_data User context pointer passed to callbacks
 * @return Result of the connection attempt
 * 
 * @note Draft 07 (CloudFlare): WebTransport only
 * @note Draft 14 (Latest): WebTransport (raw QUIC planned for future release)
 */
MOQ_API MoqResult moq_connect(
    MoqClient* client,
    const char* url,
    MoqConnectionCallback connection_callback,
    void* user_data
);

/**
 * Disconnect from the MoQ relay
 * @param client Client handle
 * @return Result of the disconnection
 */
MOQ_API MoqResult moq_disconnect(MoqClient* client);

/**
 * Check if client is currently connected
 * @param client Client handle
 * @return true if connected, false otherwise
 */
MOQ_API bool moq_is_connected(const MoqClient* client);

/* ───────────────────────────────────────────────
 * Publishing
 * ─────────────────────────────────────────────── */

/**
 * Announce a namespace for publishing
 * @param client Client handle
 * @param namespace_str Namespace to announce
 * @return Result of the announcement
 */
MOQ_API MoqResult moq_announce_namespace(MoqClient* client, const char* namespace_str);

/**
 * Create a publisher for a specific track (defaults to stream mode)
 * @param client Client handle
 * @param namespace_str Namespace of the track
 * @param track_name Name of the track
 * @return Handle to the publisher or NULL on failure
 * @deprecated Use moq_create_publisher_ex() to specify delivery mode
 */
MOQ_API MoqPublisher* moq_create_publisher(
    MoqClient* client,
    const char* namespace_str,
    const char* track_name
);

/**
 * Create a publisher for a specific track with explicit delivery mode
 * @param client Client handle
 * @param namespace_str Namespace of the track
 * @param track_name Name of the track
 * @param delivery_mode Delivery mode (datagram or stream)
 * @return Handle to the publisher or NULL on failure
 */
MOQ_API MoqPublisher* moq_create_publisher_ex(
    MoqClient* client,
    const char* namespace_str,
    const char* track_name,
    MoqDeliveryMode delivery_mode
);

/**
 * Destroy a publisher
 * @param publisher Publisher handle
 */
MOQ_API void moq_publisher_destroy(MoqPublisher* publisher);

/**
 * Publish data on a track
 * @param publisher Publisher handle
 * @param data Data buffer to publish
 * @param data_len Length of data
 * @param delivery_mode Delivery mode (datagram or stream)
 * @return Result of the publish operation
 */
MOQ_API MoqResult moq_publish_data(
    MoqPublisher* publisher,
    const uint8_t* data,
    size_t data_len,
    MoqDeliveryMode delivery_mode
);

/* ───────────────────────────────────────────────
 * Subscribing
 * ─────────────────────────────────────────────── */

/**
 * Subscribe to a track
 * @param client Client handle
 * @param namespace_str Namespace of the track
 * @param track_name Name of the track
 * @param data_callback Callback for received data
 * @param user_data User context pointer passed to callbacks
 * @return Handle to the subscriber or NULL on failure
 */
MOQ_API MoqSubscriber* moq_subscribe(
    MoqClient* client,
    const char* namespace_str,
    const char* track_name,
    MoqDataCallback data_callback,
    void* user_data
);

/**
 * Unsubscribe and destroy a subscriber
 * @param subscriber Subscriber handle
 */
MOQ_API void moq_subscriber_destroy(MoqSubscriber* subscriber);

/* ───────────────────────────────────────────────
 * Utilities
 * ─────────────────────────────────────────────── */

/**
 * Free a string allocated by the FFI layer
 * @param str String to free
 */
MOQ_API void moq_free_str(const char* str);

/**
 * Get the version string of the MoQ FFI library
 * @return Version string (do not free)
 */
MOQ_API const char* moq_version(void);

/**
 * Get the last error message for the current thread
 * @return Error message string (do not free) or NULL if no error
 */
MOQ_API const char* moq_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_FFI_H */
