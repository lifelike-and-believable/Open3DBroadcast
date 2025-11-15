#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"

/**
 * Lightweight helper base that validates audio submissions and normalises stream labels before
 * forwarding PCM frames to transport-specific implementations.
 */
class OPEN3DSENDER_API FO3DSenderAudioSinkBase : public IO3DSenderAudioSink
{
public:
	explicit FO3DSenderAudioSinkBase(FO3DTransportAudioConfig InConfig)
		: AudioConfig(MoveTemp(InConfig))
	{
	}

	virtual ~FO3DSenderAudioSinkBase() = default;

	virtual bool SubmitPcm(const FString& StreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override final;

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) = 0;

	const FO3DTransportAudioConfig& GetAudioConfig() const { return AudioConfig; }

private:
	FO3DTransportAudioConfig AudioConfig;
};
