// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DJitterSoundWave.h"

#include "O3DAudioJitterBuffer.h"

int32 UO3DJitterSoundWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// Audio render thread. USoundWaveProcedural::GeneratePCMData plays whatever is left in OutAudio,
	// so it must hold exactly the samples returned (none when nothing is ready: the base class then
	// writes silence).
	if (!JitterBuffer.IsValid() || NumSamples <= 0)
	{
		OutAudio.Reset();
		return 0;
	}

	OutAudio.SetNumUninitialized(NumSamples * sizeof(int16), EAllowShrinking::No);
	const int32 Written = JitterBuffer->Pull(reinterpret_cast<int16*>(OutAudio.GetData()), NumSamples);
	if (Written <= 0)
	{
		OutAudio.Reset();
		return 0;
	}
	if (Written < NumSamples)
	{
		// The buffer ran dry mid-request and starts its pre-roll again: pad this callback with silence.
		FMemory::Memzero(OutAudio.GetData() + Written * sizeof(int16), (NumSamples - Written) * sizeof(int16));
	}
	return NumSamples;
}
