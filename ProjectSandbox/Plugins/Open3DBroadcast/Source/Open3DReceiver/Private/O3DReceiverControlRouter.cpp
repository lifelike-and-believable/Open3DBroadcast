// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DReceiverControlRouter.h"

#include "O3DControlBus.h"
#include "O3DControlConvert.h"
#include "O3DControlSettings.h"
#include "O3DReceiverLogs.h"
#include "O3DRuntimeContext.h"

FO3DReceiverControlRouter::FO3DReceiverControlRouter(FO3DControlBus::FInstance& InBus)
	: Bus(InBus)
{
}

FO3DReceiverControlRouter::FO3DReceiverControlRouter()
	: FO3DReceiverControlRouter(FO3DRuntimeContext::Default()->GetControlBus())
{
}

void FO3DReceiverControlRouter::ApplyConfig()
{
	const UO3DControlSettings* Project = GetDefault<UO3DControlSettings>();
	O3DS::Control::ReceiverConfig Config;
	Config.max_keys_per_source = static_cast<size_t>(FMath::Clamp(Project->MaxControlKeysPerSource, 1, static_cast<int32>(O3DS::ControlLimits::kMaxKeysPerSource)));
	Config.max_live_bytes_per_s = static_cast<double>(FMath::Max(Project->MaxControlLiveBytesPerSecond, 2048));
	Config.max_snapshot_bytes_per_s = static_cast<double>(FMath::Max(Project->MaxControlSnapshotBytesPerSecond, 2048));
	for (const FString& Prefix : Project->ControlAllowlist)
	{
		if (!Prefix.IsEmpty())
		{
			Config.allow_prefixes.push_back(O3DControl::ToUtf8(Prefix));
		}
	}
	Receiver.SetConfig(Config);
	Aligner.SetMaxHold(FMath::Max(Project->MaxAlignmentHoldMs, 0) / 1000.0);
}

void FO3DReceiverControlRouter::HandlePayload(bool bEnabled, const uint8* Data, int32 NumBytes, double NowSeconds, const FString& StreamId)
{
	check(IsInGameThread());
	if (!bEnabled)
	{
		++PayloadsDroppedDisabled;
		return;
	}
	bWasEnabled = true;

	// Core clocks are this receiver's own FPlatformTime, taken on the game thread.
	std::vector<O3DS::Control::Change> Changes;
	const O3DS::Control::ParseError Error = Receiver.Submit(Data, static_cast<size_t>(NumBytes), NowSeconds, Changes);
	if (Error != O3DS::Control::ParseError::None)
	{
		UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Rejected control message (%d bytes): %s"), NumBytes, UTF8_TO_TCHAR(O3DS::Control::ToString(Error)));
		return;
	}
	for (O3DS::Control::Change& Change : Changes)
	{
		Route(MoveTemp(Change), NowSeconds, StreamId);
	}
}

void FO3DReceiverControlRouter::Route(O3DS::Control::Change&& Change, double NowSeconds, const FString& StreamId)
{
	SourcesSeen.Add(O3DControl::FromUtf8(Change.source_id));
	if (GetDefault<UO3DControlSettings>()->bAlignControlToMocap)
	{
		Aligner.Push(MoveTemp(Change), NowSeconds);
	}
	else
	{
		Publish(Change, StreamId);
	}
}

void FO3DReceiverControlRouter::Publish(const O3DS::Control::Change& Change, const FString& StreamId) const
{
	Bus.Publish(O3DControl::FromCore(Change, StreamId));
}

void FO3DReceiverControlRouter::Discard()
{
	Receiver.Reset();
	std::vector<O3DS::Control::Change> Held;
	Aligner.Flush(Held); // dropped, not published
	for (const FString& SourceId : SourcesSeen)
	{
		Bus.ForgetSource(SourceId);
	}
	SourcesSeen.Reset();
	bWasEnabled = false;
}

void FO3DReceiverControlRouter::Tick(bool bEnabled, double NowSeconds, const FString& StreamId, FPresentedSenderTime PresentedSenderTime)
{
	if (!bEnabled)
	{
		if (bWasEnabled)
		{
			Discard(); // silently: no Cleared delegates (ADR 0011 item 8)
		}
		return;
	}

	std::vector<O3DS::Control::Change> Changes;
	Receiver.Tick(NowSeconds, Changes); // incomplete snapshots, quiet sources
	for (O3DS::Control::Change& Change : Changes)
	{
		Route(MoveTemp(Change), NowSeconds, StreamId);
	}

	std::vector<O3DS::Control::Change> Released;
	if (GetDefault<UO3DControlSettings>()->bAlignControlToMocap)
	{
		Aligner.SetMaxHold(FMath::Max(GetDefault<UO3DControlSettings>()->MaxAlignmentHoldMs, 0) / 1000.0);
		Aligner.Release(NowSeconds, [this, &PresentedSenderTime](const std::string& SourceId, uint64_t& OutUs)
		{
			const std::vector<std::string>* Subjects = Receiver.FindMocapSubjects(SourceId);
			if (Subjects == nullptr || Subjects->empty())
			{
				return false; // a control-only sender
			}
			return PresentedSenderTime(*Subjects, OutUs);
		}, Released);
	}
	else
	{
		Aligner.Flush(Released); // alignment turned off while changes were held
	}
	for (const O3DS::Control::Change& Change : Released)
	{
		Publish(Change, StreamId);
	}
}

void FO3DReceiverControlRouter::FlushHeld(const FString& StreamId)
{
	std::vector<O3DS::Control::Change> Held;
	Aligner.Flush(Held);
	for (const O3DS::Control::Change& Change : Held)
	{
		Publish(Change, StreamId);
	}
}
