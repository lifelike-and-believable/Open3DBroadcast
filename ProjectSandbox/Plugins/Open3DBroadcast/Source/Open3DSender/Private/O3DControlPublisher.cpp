// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DControlPublisher.h"

#include "HAL/PlatformTime.h"
#include "O3DControlConvert.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DSenderInterface.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
#include "o3ds/sequencing.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	/** Sender clock in microseconds: FPlatformTime, the clock FO3DSenderSerializer stamps SubjectList.time with. */
	uint64 SenderClockUs(double NowSeconds)
	{
		return NowSeconds > 0.0 ? static_cast<uint64>(NowSeconds * 1.0e6) : 0;
	}

	FString Describe(O3DS::Control::PublishResult Result)
	{
		return FString(UTF8_TO_TCHAR(O3DS::Control::ToString(Result)));
	}
}

FO3DControlPublisher::FO3DControlPublisher(const FString& SourceName)
	: SourceId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
	, Core(MakeUnique<O3DS::Control::ControlPublisher>(O3DControl::ToUtf8(SourceId), O3DControl::ToUtf8(SourceName)))
{
}

FO3DControlPublisher::~FO3DControlPublisher() = default;

void FO3DControlPublisher::SetConfig(double SnapshotIntervalSeconds, int32 EventRedundancy, double MaxValueRateHz)
{
	O3DS::Control::PublisherConfig Config = Core->GetConfig();
	Config.snapshot_interval_s = SnapshotIntervalSeconds;
	Config.event_redundancy = EventRedundancy;
	Config.max_value_rate_hz = MaxValueRateHz;
	Core->SetConfig(Config); // the core clamps every field
}

void FO3DControlPublisher::SetMocapSubjects(const TArray<FString>& Subjects)
{
	if (Subjects == LastMocapSubjects)
	{
		return;
	}
	LastMocapSubjects = Subjects;
	std::vector<std::string> Names;
	Names.reserve(Subjects.Num());
	for (const FString& Subject : Subjects)
	{
		Names.push_back(O3DControl::ToUtf8(Subject));
	}
	Core->SetMocapSubjects(std::move(Names));
}

void FO3DControlPublisher::Start()
{
	Core->Start(O3DS::NewSessionEpoch());
}

void FO3DControlPublisher::Stop()
{
	Core->Stop();
}

bool FO3DControlPublisher::IsRunning() const
{
	return Core->IsRunning();
}

bool FO3DControlPublisher::SetValue(const FString& Key, const FString& TargetSubject, const FO3DControlValue& Value, FString* OutError)
{
	O3DS::Control::Value CoreValue;
	O3DControl::ToCore(Value, CoreValue);
	const O3DS::Control::PublishResult Result = Core->SetValue(O3DControl::ToUtf8(Key), O3DControl::ToUtf8(TargetSubject), CoreValue);
	if (Result != O3DS::Control::PublishResult::Ok && OutError)
	{
		*OutError = Describe(Result);
	}
	return Result == O3DS::Control::PublishResult::Ok;
}

bool FO3DControlPublisher::ClearValue(const FString& Key, const FString& TargetSubject)
{
	return Core->ClearValue(O3DControl::ToUtf8(Key), O3DControl::ToUtf8(TargetSubject)) == O3DS::Control::PublishResult::Ok;
}

void FO3DControlPublisher::ClearAll()
{
	Core->ClearAll();
}

bool FO3DControlPublisher::FireEvent(const FString& Name, const FString& TargetSubject, const FO3DControlValue& Value, FString* OutError)
{
	O3DS::Control::Value CoreValue;
	O3DControl::ToCore(Value, CoreValue);
	const O3DS::Control::PublishResult Result = Core->FireEvent(O3DControl::ToUtf8(Name), O3DControl::ToUtf8(TargetSubject), CoreValue,
		SenderClockUs(FPlatformTime::Seconds()));
	if (Result != O3DS::Control::PublishResult::Ok && OutError)
	{
		*OutError = Describe(Result);
	}
	return Result == O3DS::Control::PublishResult::Ok;
}

bool FO3DControlPublisher::FindValue(const FString& Key, const FString& TargetSubject, FO3DControlValue& OutValue) const
{
	const O3DS::Control::Value* Found = Core->FindValue(O3DControl::ToUtf8(Key), O3DControl::ToUtf8(TargetSubject));
	if (Found == nullptr)
	{
		return false;
	}
	OutValue = O3DControl::FromCore(*Found);
	return true;
}

int32 FO3DControlPublisher::Tick(IOpen3DSender& Sender)
{
	if (!Core->IsRunning())
	{
		return 0;
	}
	const double NowSeconds = FPlatformTime::Seconds();
	std::vector<O3DS::Control::OutgoingMessage> Out;
	Core->Tick(NowSeconds, SenderClockUs(NowSeconds), O3DS::NowUtcMicros(), Out);

	int32 Accepted = 0;
	TArray<uint8> Envelope;
	for (const O3DS::Control::OutgoingMessage& Message : Out)
	{
		const TConstArrayView<uint8> Payload(Message.bytes.data(), static_cast<int32>(Message.bytes.size()));
		// Anything but Queued (not connected yet, queue full, ...) goes back to the core, which
		// retries events until their TTL and repairs values with the next snapshot (ADR 0011).
		if (O3DS::WriteControlEnvelope(Payload, NowSeconds, Envelope)
			&& Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued)
		{
			++Accepted;
		}
		else
		{
			Core->OnSendRefused(Message, NowSeconds);
		}
	}
	return Accepted;
}
