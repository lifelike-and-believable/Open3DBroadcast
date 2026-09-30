// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
THIRD_PARTY_INCLUDES_START
#include "livekit_ffi.h"
THIRD_PARTY_INCLUDES_END

/**
 * Per-instance LiveKit FFI function table (ADR 0006 option F2, WP-S7).
 *
 * FO3DWebRTCSender and FO3DWebRTCReceiver call LiveKit only through the table they were
 * constructed with. Production code uses GetLinkedLkFfiApi(), which points every entry at the
 * symbol linked from livekit_ffi.dll. Tests build their own table with fake functions. Nothing
 * here is global mutable state: each transport copies the table at construction.
 *
 * Each member has the same name and signature as the livekit_ffi.h function it stands for, so
 * a call reads `Ffi.lk_send_data_ex(...)`. Only the symbols this module uses are listed.
 * WP-T2 is expected to generalise this seam; keep it a plain struct of function pointers.
 */
struct FLkFfiApi
{
	void (*lk_free_str)(char* p) = nullptr;

	LkClientHandle* (*lk_client_create)(void) = nullptr;
	void (*lk_client_destroy)(LkClientHandle*) = nullptr;

	LkResult (*lk_client_set_data_callback)(LkClientHandle*, LkDataCallback cb, void* user) = nullptr;
	LkResult (*lk_client_set_data_callback_ex)(LkClientHandle*, LkDataCallbackEx cb, void* user) = nullptr;
	LkResult (*lk_client_set_audio_callback_ex)(LkClientHandle*, LkAudioCallbackEx cb, void* user) = nullptr;
	LkResult (*lk_set_connection_callback)(LkClientHandle*, LkConnectionCallback cb, void* user) = nullptr;

	LkResult (*lk_connect_with_role_async)(LkClientHandle*, const char* url, const char* token, LkRole role) = nullptr;
	LkResult (*lk_disconnect)(LkClientHandle*) = nullptr;
	LkResult (*lk_refresh_token)(LkClientHandle*, const char* token) = nullptr;

	LkResult (*lk_set_audio_publish_options)(LkClientHandle*, int32_t bitrate_bps, int32_t enable_dtx, int32_t stereo) = nullptr;
	LkResult (*lk_set_audio_output_format)(LkClientHandle*, int32_t sample_rate, int32_t channels) = nullptr;
	LkResult (*lk_audio_track_create)(LkClientHandle*, const LkAudioTrackConfig* config, LkAudioTrackHandle** out_track) = nullptr;
	LkResult (*lk_audio_track_destroy)(LkAudioTrackHandle*) = nullptr;
	LkResult (*lk_audio_track_publish_pcm_i16)(LkAudioTrackHandle*, const int16_t* pcm_interleaved, size_t frames_per_channel) = nullptr;

	LkResult (*lk_send_data_ex)(LkClientHandle*, const uint8_t* bytes, size_t len, LkReliability reliability, int32_t ordered, const char* label) = nullptr;
	LkResult (*lk_set_default_data_labels)(LkClientHandle*, const char* reliable_label, const char* lossy_label) = nullptr;
	LkResult (*lk_set_log_level)(LkClientHandle*, LkLogLevel level) = nullptr;

	/** True when every function pointer is set. Transports refuse to initialize otherwise. */
	bool IsComplete() const;

	/**
	 * Converts Result.message to an FString and frees it with lk_free_str.
	 * Returns an empty string when there is no message. Call at most once per result.
	 */
	FString TakeMessage(const LkResult& Result) const;
};

/** The production table, built from the symbols linked from livekit_ffi.dll. */
const FLkFfiApi& GetLinkedLkFfiApi();
