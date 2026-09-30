// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

/**
 * Per-instance moq-ffi function table (ADR 0006 option F2, WP-S8).
 *
 * Every moq-ffi call in this module goes through an instance of this table, which the session
 * wrapper, sender and receiver receive at construction. Production code uses GetProduction(),
 * built from the linked (delay-loaded) symbols. Tests build a fake table and pass it in; no
 * global state is changed.
 *
 * Members are TFunction rather than raw function pointers because moq_client_create() takes no
 * user data, so a fake needs a capture to reach its own per-test state without globals.
 * The indirect call costs nothing measurable next to the FFI call it wraps.
 *
 * The header is public so the Open3DBroadcastTests module can build a fake table; tests reach
 * the transport through Testing/MoQTesting.h (WP-T2). Only this module calls GetProduction().
 */
struct FMoQFfiApi
{
	TFunction<bool()> Init;
	TFunction<MoqClient*()> ClientCreate;
	TFunction<void(MoqClient*)> ClientDestroy;
	/** Blocks until the connection succeeds, fails, or moq-ffi's own timeout expires. */
	TFunction<MoqResult(MoqClient*, const char*, MoqConnectionCallback, void*)> Connect;
	TFunction<MoqResult(MoqClient*)> Disconnect;
	TFunction<MoqResult(MoqClient*, const char*)> AnnounceNamespace;
	TFunction<MoqPublisher*(MoqClient*, const char*, const char*, MoqDeliveryMode)> CreatePublisherEx;
	TFunction<void(MoqPublisher*)> PublisherDestroy;
	TFunction<MoqResult(MoqPublisher*, const uint8_t*, size_t, MoqDeliveryMode)> PublishData;
	TFunction<MoqSubscriber*(MoqClient*, const char*, const char*, MoqDataCallback, void*)> Subscribe;
	TFunction<void(MoqSubscriber*)> SubscriberDestroy;
	/** Frees MoqResult::message strings. */
	TFunction<void(const char*)> FreeStr;
	/** Static string owned by the library; never freed. */
	TFunction<const char*()> Version;
	/**
	 * Thread-local error text for the calling thread (see CopyLastErrorOnThisThread). The string
	 * is owned by moq-ffi and must not be freed.
	 */
	TFunction<const char*()> LastError;

	/**
	 * Runs a call that may block (moq_connect, a queued subscribe) off the game thread.
	 * Production uses a background task-graph task. A test can run it inline or hold it.
	 */
	TFunction<void(TUniqueFunction<void()>&&)> LaunchBlocking;

	/** The table bound to the linked moq-ffi symbols. Built once; safe to call from any thread. */
	static TSharedRef<const FMoQFfiApi, ESPMode::ThreadSafe> GetProduction();

	/**
	 * Names of every moq-ffi export this table binds. FMoQFfiSupport validates the loaded
	 * library against exactly this list (TRF-29).
	 */
	static TArray<const TCHAR*> GetRequiredSymbolNames();
};

using FMoQFfiApiRef = TSharedRef<const FMoQFfiApi, ESPMode::ThreadSafe>;

namespace MoQFfi
{
	/**
	 * Copies moq_last_error() for the calling thread (TRF-39).
	 *
	 * moq-ffi keeps the last error in thread-local storage. The returned pointer is owned by
	 * moq-ffi, must not be freed, and stays valid only until the next moq_last_error() call on
	 * the same thread. The value is not cleared by later successful calls, so it is meaningful
	 * only immediately after a call on this same thread that reported failure through its
	 * return value. Never call this from a connection or data callback to explain an error that
	 * another thread produced.
	 */
	FString CopyLastErrorOnThisThread(const FMoQFfiApi& Api);

	/** Copies a MoqResult::message string and frees it with moq_free_str. Null gives an empty string. */
	FString CopyAndFreeString(const FMoQFfiApi& Api, const char* FfiString);
}
