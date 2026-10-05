// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DAudioBus.h"
#include "O3DControlBus.h"
#include "O3DPerformanceMetrics.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FO3DRuntimeContext;

using FO3DRuntimeContextRef = TSharedRef<FO3DRuntimeContext, ESPMode::ThreadSafe>;
using FO3DRuntimeContextPtr = TSharedPtr<FO3DRuntimeContext, ESPMode::ThreadSafe>;

/**
 * The data-path services one set of receivers, senders and gameplay components shares
 * (docs/adr/0012-runtime-services-and-global-state.md): performance metrics (with their
 * transport metrics registry), an audio bus and a control bus. Two contexts never see each
 * other's audio, control or metrics.
 *
 * Default() is the process default. The static accessors FO3DAudioBus::OnPcm16() and
 * PublishPcm16(), the FO3DControlBus statics and FO3DPerformanceMetrics::Get() use it, so code
 * written against them behaves as before. A test builds its own context and reads its own
 * numbers.
 *
 * Boundary (ADR 0012 item 6): the context is handed to transports, receiver sources, sender
 * components and gameplay components. The classes they own (decoder, publisher, scheduler,
 * pipeline, serializer) receive the specific metrics, bus or handle they need, never the context.
 *
 * Process-wide by nature and therefore not in a context: the transport registry, the secret
 * store, the audio input devices, the MoQ dispatcher, the console variables and the control
 * receive override (FO3DControlBus::SetReceiveOverride).
 *
 * Threading: the metrics are thread-safe; both buses are game thread only, as their own
 * documentation says. Held by thread-safe shared reference, so a transport thread may keep its
 * context alive.
 */
class OPEN3DSHARED_API FO3DRuntimeContext
{
public:
	/** Name is for diagnostics and for selecting a named context; the default context's is NAME_None. */
	explicit FO3DRuntimeContext(FName InName);
	~FO3DRuntimeContext();

	FO3DRuntimeContext(const FO3DRuntimeContext&) = delete;
	FO3DRuntimeContext& operator=(const FO3DRuntimeContext&) = delete;

	/**
	 * The process default context. Created by the Open3DShared module at startup (or on first use,
	 * if that comes earlier) and kept until static destruction. Thread-safe.
	 */
	static const FO3DRuntimeContextRef& Default();

	/** Context when it is set, otherwise Default(). FO3DTransportConfig::Context is resolved with this. Thread-safe. */
	static FO3DRuntimeContextRef OrDefault(const FO3DRuntimeContextPtr& Context)
	{
		return Context.IsValid() ? Context.ToSharedRef() : Default();
	}

	/**
	 * A sender transport's metrics handle (ADR 0012 item 4): Provided when set, otherwise a new
	 * handle named FallbackOwnerName from this context. False, with OutHandle untouched, when
	 * Provided belongs to another context's metrics. Thread-safe.
	 */
	bool ResolveSenderMetrics(const TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe>& Provided, const FString& FallbackOwnerName, FO3DSenderMetricsHandleRef& OutHandle)
	{
		if (!Provided.IsValid())
		{
			OutHandle = Metrics.AcquireSenderMetrics(FallbackOwnerName);
			return true;
		}
		if (&Provided->GetAggregate() != &Metrics)
		{
			return false;
		}
		OutHandle = Provided.ToSharedRef();
		return true;
	}

	FName GetName() const { return Name; }

	FO3DPerformanceMetrics& GetMetrics() { return Metrics; }
	const FO3DPerformanceMetrics& GetMetrics() const { return Metrics; }

	/** Game thread only (see FO3DAudioBus). */
	FO3DAudioBus::FInstance& GetAudioBus() { return AudioBus; }

	/** Game thread only (see FO3DControlBus). */
	FO3DControlBus::FInstance& GetControlBus() { return ControlBus; }

private:
	const FName Name;
	FO3DPerformanceMetrics Metrics;
	FO3DAudioBus::FInstance AudioBus;
	FO3DControlBus::FInstance ControlBus;
};
