#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"

class ISerializedFrameConsumer;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DMoQReceiver, Log, All);

/**
 * Stub receiver implementation for Phase 0 - build verification only.
 * Full implementation will be completed in Phase 3.
 */
class FO3DMoQReceiver : public IOpen3DReceiver
{
public:
	FO3DMoQReceiver();
	virtual ~FO3DMoQReceiver();

	// IOpen3DReceiver interface
	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override { return false; } // Phase 4
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override {} // Phase 4

private:
	FO3DTransportStats Stats;
	TSharedPtr<ISerializedFrameConsumer> Consumer;
};
