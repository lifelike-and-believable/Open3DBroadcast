// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Templates/SharedPointer.h"
#include "O3DJitterSoundWave.generated.h"

class FO3DAudioJitterBuffer;

/**
 * A procedural sound wave that plays from an FO3DAudioJitterBuffer (RCV-20). The mixer asks for
 * audio on the audio render thread through OnGeneratePCMAudio, as USynthSound does in the engine;
 * the buffer decides what to hand out. Shares the buffer with UO3DRemoteAudioComponent, so either
 * can go first.
 */
UCLASS(Transient)
class UO3DJitterSoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	void SetJitterBuffer(TSharedPtr<FO3DAudioJitterBuffer, ESPMode::ThreadSafe> InBuffer) { JitterBuffer = MoveTemp(InBuffer); }

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

private:
	TSharedPtr<FO3DAudioJitterBuffer, ESPMode::ThreadSafe> JitterBuffer;
};
