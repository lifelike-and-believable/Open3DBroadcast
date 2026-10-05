// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DSenderCapture.h"

#include "O3DSenderLogs.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/capture.h"
#include "o3ds/sequencing.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <fstream>
#include <memory>

namespace O3DSenderCapturePrivate
{
	struct FState
	{
		FCriticalSection Lock;
		std::unique_ptr<std::ofstream> Stream;
		FString Path;
		int64 Records = 0;
	};

	FState& GetState()
	{
		static FState State;
		return State;
	}

	std::atomic<bool> GActive{ false };
}

bool FO3DSenderCapture::Start(const FString& InPath)
{
	using namespace O3DSenderCapturePrivate;
	Stop();

	FString Path = InPath;
	if (Path.IsEmpty())
	{
		Path = FString::Printf(TEXT("O3DSenderCapture-%s.o3dscap"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
	}
	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("O3DCaptures"), Path);
	}
	Path = FPaths::ConvertRelativePathToFull(Path);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);

	FState& State = GetState();
	FScopeLock Guard(&State.Lock);
	auto Stream = std::make_unique<std::ofstream>(TCHAR_TO_UTF8(*Path), std::ios::binary | std::ios::trunc);
	O3DS::CaptureHeaderInfo Header;
	Header.flags = 1; // timestamps are UTC microseconds
	Header.base_wallclock_us = O3DS::NowUtcMicros();
	Header.source_desc = "Open3DBroadcast sender capture";
	if (!*Stream || !O3DS::WriteCaptureHeader(*Stream, Header))
	{
		UE_LOG(LogO3DSenderComponent, Error, TEXT("Sender capture: cannot write '%s'."), *Path);
		return false;
	}
	State.Stream = MoveTemp(Stream);
	State.Path = Path;
	State.Records = 0;
	GActive.store(true);
	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture started: %s"), *Path);
	return true;
}

int64 FO3DSenderCapture::Stop()
{
	using namespace O3DSenderCapturePrivate;
	FState& State = GetState();
	FScopeLock Guard(&State.Lock);
	if (!State.Stream)
	{
		return 0;
	}
	GActive.store(false);
	State.Stream->flush();
	State.Stream.reset();
	UE_LOG(LogO3DSenderComponent, Log, TEXT("Sender capture stopped: %lld payloads in %s"), State.Records, *State.Path);
	return State.Records;
}

bool FO3DSenderCapture::IsActive()
{
	return O3DSenderCapturePrivate::GActive.load(std::memory_order_relaxed);
}

FString FO3DSenderCapture::GetPath()
{
	using namespace O3DSenderCapturePrivate;
	FState& State = GetState();
	FScopeLock Guard(&State.Lock);
	return State.Path;
}

void FO3DSenderCapture::Record(TConstArrayView<uint8> Payload)
{
	using namespace O3DSenderCapturePrivate;
	if (!IsActive() || Payload.Num() == 0)
	{
		return;
	}
	FState& State = GetState();
	FScopeLock Guard(&State.Lock);
	if (!State.Stream)
	{
		return;
	}
	O3DS::CaptureRecord RecordData;
	RecordData.recv_wallclock_us = O3DS::NowUtcMicros();
	RecordData.wire_bytes.assign(reinterpret_cast<const char*>(Payload.GetData()), reinterpret_cast<const char*>(Payload.GetData()) + Payload.Num());
	if (O3DS::WriteCaptureRecord(*State.Stream, RecordData))
	{
		++State.Records;
	}
}

// Registered when the module loads and unregistered when it unloads (SND-32 pattern).
static FAutoConsoleCommand GO3DSenderCaptureStartCommand(
	TEXT("o3d.Sender.Capture.Start"),
	TEXT("Record every serialized sender payload to a .o3dscap file: o3d.Sender.Capture.Start [file]. Relative paths go under Saved/O3DCaptures."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		FO3DSenderCapture::Start(Args.Num() > 0 ? Args[0] : FString());
	}));

static FAutoConsoleCommand GO3DSenderCaptureStopCommand(
	TEXT("o3d.Sender.Capture.Stop"),
	TEXT("Stop the sender capture started with o3d.Sender.Capture.Start."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FO3DSenderCapture::Stop();
	}));
