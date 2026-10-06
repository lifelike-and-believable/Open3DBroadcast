// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DAudioJitterBuffer.h"

#include "Misc/ScopeLock.h"

void FO3DAudioJitterBuffer::Configure(int32 InSampleRate, int32 InNumChannels, float InTargetLatencyMs)
{
	FScopeLock ScopeLock(&Lock);
	NumChannels = FMath::Max(1, InNumChannels);
	const int64 SamplesPerSecond = static_cast<int64>(FMath::Max(1, InSampleRate)) * NumChannels;
	const auto ToWholeFrames = [this, SamplesPerSecond](float Ms)
	{
		const int64 Raw = static_cast<int64>(SamplesPerSecond * FMath::Max(0.0f, Ms) / 1000.0f);
		return static_cast<int32>(FMath::Min<int64>(Raw - Raw % NumChannels, MAX_int32 / 2));
	};
	// Pre-roll needs at least one frame, so a 0 ms target starts on the first audio.
	TargetSamples = FMath::Max(NumChannels, ToWholeFrames(InTargetLatencyMs));
	MaxSamples = TargetSamples + ToWholeFrames(TrimMarginMs);
	Samples.Reset();
	ReadIndex = 0;
	bBuffering = true;
	DroppedSamples = 0;
}

void FO3DAudioJitterBuffer::Push(TConstArrayView<uint8> PCM16Bytes)
{
	const int32 PushedSamples = PCM16Bytes.Num() / static_cast<int32>(sizeof(int16));
	const int32 Count = PushedSamples - PushedSamples % NumChannels;
	if (Count <= 0)
	{
		return;
	}

	FScopeLock ScopeLock(&Lock);
	// Drop what was already played before appending, so the array does not grow without bound.
	if (ReadIndex > 0 && ReadIndex >= Samples.Num() / 2)
	{
		Samples.RemoveAt(0, ReadIndex, EAllowShrinking::No);
		ReadIndex = 0;
	}
	const int32 Start = Samples.Num();
	Samples.AddUninitialized(Count);
	FMemory::Memcpy(Samples.GetData() + Start, PCM16Bytes.GetData(), Count * sizeof(int16));

	const int32 Queued = QueuedLocked();
	if (Queued > MaxSamples)
	{
		const int32 Drop = Queued - TargetSamples;
		ReadIndex += Drop;
		DroppedSamples += Drop;
	}
}

int32 FO3DAudioJitterBuffer::Pull(int16* Out, int32 NumSamples)
{
	FScopeLock ScopeLock(&Lock);
	const int32 Queued = QueuedLocked();
	if (bBuffering)
	{
		if (Queued < TargetSamples)
		{
			return 0;
		}
		bBuffering = false;
	}

	const int32 Wanted = NumSamples - NumSamples % NumChannels;
	const int32 Count = FMath::Min(Wanted, Queued);
	if (Count > 0)
	{
		FMemory::Memcpy(Out, Samples.GetData() + ReadIndex, Count * sizeof(int16));
		ReadIndex += Count;
	}
	if (Count < Wanted)
	{
		bBuffering = true;
	}
	return Count;
}

void FO3DAudioJitterBuffer::Reset()
{
	FScopeLock ScopeLock(&Lock);
	Samples.Reset();
	ReadIndex = 0;
	bBuffering = true;
}

int32 FO3DAudioJitterBuffer::GetQueuedSamples() const
{
	FScopeLock ScopeLock(&Lock);
	return QueuedLocked();
}

bool FO3DAudioJitterBuffer::IsBuffering() const
{
	FScopeLock ScopeLock(&Lock);
	return bBuffering;
}

int64 FO3DAudioJitterBuffer::GetDroppedSamples() const
{
	FScopeLock ScopeLock(&Lock);
	return DroppedSamples;
}
