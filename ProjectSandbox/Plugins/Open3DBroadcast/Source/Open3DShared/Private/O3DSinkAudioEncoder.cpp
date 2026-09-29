// Copyright (c) Open3DStream Contributors

#include "O3DSinkAudioEncoder.h"

#include "Misc/ScopeLock.h"

void FO3DAudioSubjectSlot::Set(const FString& InSubject)
{
	FScopeLock Lock(&Mutex);
	Subject = InSubject;
}

void FO3DAudioSubjectSlot::Reset()
{
	FScopeLock Lock(&Mutex);
	Subject.Reset();
}

FString FO3DAudioSubjectSlot::Get() const
{
	FScopeLock Lock(&Mutex);
	return Subject;
}

FO3DSinkAudioEncoder::FO3DSinkAudioEncoder(FSettings InSettings)
	: Settings(MoveTemp(InSettings))
{
}

FO3DSinkAudioEncoder::~FO3DSinkAudioEncoder() = default;

FO3DSinkAudioEncoder::FStreamEncoder* FO3DSinkAudioEncoder::FindOrCreate(const FString& StreamLabel)
{
	if (TUniquePtr<FStreamEncoder>* Existing = Encoders.Find(StreamLabel))
	{
		return Existing->Get();
	}

	const int32 Cap = FMath::Max(1, Settings.MaxStreams);
	while (Encoders.Num() >= Cap)
	{
		const FString* Oldest = nullptr;
		uint64 OldestUse = MAX_uint64;
		for (const TPair<FString, TUniquePtr<FStreamEncoder>>& Pair : Encoders)
		{
			if (Pair.Value->LastUse < OldestUse)
			{
				OldestUse = Pair.Value->LastUse;
				Oldest = &Pair.Key;
			}
		}
		if (!Oldest)
		{
			break;
		}
		const FString KeyToRemove = *Oldest;
		Encoders.Remove(KeyToRemove);
	}

	TUniquePtr<FStreamEncoder> NewEncoder = MakeUnique<FStreamEncoder>();
	if (!NewEncoder->Encoder.Initialize(Settings.Config, Settings.DefaultStreamLabel, Settings.DefaultSubject))
	{
		return nullptr;
	}

	FStreamEncoder* Raw = NewEncoder.Get();
	Encoders.Add(StreamLabel, MoveTemp(NewEncoder));
	return Raw;
}

bool FO3DSinkAudioEncoder::Encode(const FString& StreamLabel,
	const FString& SubjectOverride,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec,
	O3DAudio::FEncodedFrame& OutFrame)
{
	if (!Interleaved || NumFrames <= 0)
	{
		return false;
	}

	const FString& Subject = SubjectOverride.IsEmpty() ? Settings.DefaultSubject : SubjectOverride;

	FScopeLock Lock(&Mutex);
	FStreamEncoder* Stream = FindOrCreate(StreamLabel);
	if (!Stream)
	{
		return false;
	}
	Stream->LastUse = ++UseCounter;

	if (!Stream->Encoder.BuildEncodedFrame(StreamLabel, Subject, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, OutFrame))
	{
		return false;
	}

	if (OutFrame.Meta.SubjectName.IsEmpty())
	{
		OutFrame.Meta.SubjectName = Subject;
	}
	OutFrame.Meta.SourceGuid = Settings.SourceGuid;
	return true;
}

bool FO3DSinkAudioEncoder::EncodeUnified(const FString& StreamLabel,
	const FString& SubjectOverride,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec,
	TArray<uint8>& OutMessage)
{
	O3DAudio::FEncodedFrame Frame;
	if (!Encode(StreamLabel, SubjectOverride, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frame))
	{
		return false;
	}
	return O3DAudio::CreateUnifiedAudioMessage(Frame, TimestampSec, OutMessage);
}
