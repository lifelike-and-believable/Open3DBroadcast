#include "O3DSenderAudioSinkBase.h"

bool FO3DSenderAudioSinkBase::SubmitPcm(const FString& StreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
	{
		return false;
	}

	FString EffectiveLabel = StreamLabel;
	if (EffectiveLabel.IsEmpty())
	{
		EffectiveLabel = TEXT("audio_default");
	}

	return OnSubmitPcmInternal(EffectiveLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}
