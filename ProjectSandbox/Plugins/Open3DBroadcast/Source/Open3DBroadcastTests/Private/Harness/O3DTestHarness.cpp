// Copyright (c) Open3DStream Contributors

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "o3ds/capture.h"
#include "o3ds/model.h"
#include "o3ds/replay.h"

#include <sstream>
#include <string>
#include <vector>

namespace O3DTests
{
	bool PollUntil(double TimeoutSeconds, TFunctionRef<bool()> Condition, TFunctionRef<void()> Pump)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Pump();
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::YieldThread();
		}
		Pump();
		return Condition();
	}

	bool PollUntil(double TimeoutSeconds, TFunctionRef<bool()> Condition)
	{
		return PollUntil(TimeoutSeconds, Condition, []() {});
	}

	int32 FindFreeLoopbackPort(bool bTcp)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		Addr->SetPort(0);
		if (!bIsValid)
		{
			return 0;
		}

		FSocket* Probe = SocketSubsystem->CreateSocket(bTcp ? NAME_Stream : NAME_DGram, TEXT("O3DTests_PortProbe"), false);
		if (!Probe)
		{
			return 0;
		}

		int32 Port = 0;
		if (Probe->Bind(*Addr))
		{
			Probe->GetAddress(*Addr);
			Port = Addr->GetPort();
		}
		SocketSubsystem->DestroySocket(Probe);
		return Port;
	}

	TArray<TArray<uint8>> MakeRecordedFrames(const FString& SubjectName, int32 Count)
	{
		TArray<TArray<uint8>> Frames;

		O3DS::SubjectList List;
		const FTCHARToUTF8 SubjectUtf8(*SubjectName);
		O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
		const char* BoneNames[3] = { "Root", "Spine", "Head" };
		for (int32 Bone = 0; Bone < 3; ++Bone)
		{
			O3DS::Transform* Transform = Subject->addTransform(BoneNames[Bone], Bone - 1);
			Transform->transformOrder.push_back(O3DS::TTranslation);
			Transform->transformOrder.push_back(O3DS::TRotation);
		}

		std::stringstream Capture(std::ios::in | std::ios::out | std::ios::binary);
		O3DS::CaptureHeaderInfo Header;
		Header.source_desc = "O3DTests::MakeRecordedFrames";
		if (!O3DS::WriteCaptureHeader(Capture, Header))
		{
			return Frames;
		}

		uint64 RecvUs = 1000000;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Subject->mTransforms[0]->translation.value = O3DS::Vector3d(0.25 * Index, 0.0, 90.0);
			Subject->mTransforms[1]->translation.value = O3DS::Vector3d(0.0, 0.01 * Index, 20.0);

			O3DS::CaptureRecord Record;
			Record.recv_wallclock_us = RecvUs;
			List.Serialize(Record.wire_bytes, 10.0 + Index / 60.0);
			if (!O3DS::WriteCaptureRecord(Capture, Record))
			{
				return Frames;
			}
			RecvUs += 16667;
		}

		Capture.seekg(0);
		O3DS::ReplayConfig Config;
		O3DS::ReplayCapture(Capture, Config, [&Frames](O3DS::Frame&& Frame, double)
		{
			TArray<uint8>& Bytes = Frames.AddDefaulted_GetRef();
			Bytes.Append(reinterpret_cast<const uint8*>(Frame.bytes.data()), static_cast<int32>(Frame.bytes.size()));
			return true;
		});
		return Frames;
	}

	FString MakeUniqueName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	bool AreNetworkTestsEnabled()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("O3DB_NETWORK_TESTS")).TrimStartAndEnd() == TEXT("1");
	}
}

void FO3DRecordingFrameConsumer::SubmitFrame(const FString& /*Subject*/, const TArray<uint8>& Buffer, double /*TimestampSeconds*/)
{
	FScopeLock Lock(&Mutex);
	Frames.Add(Buffer);
	CallerThreads.Add(FPlatformTLS::GetCurrentThreadId());
}

int32 FO3DRecordingFrameConsumer::Num() const
{
	FScopeLock Lock(&Mutex);
	return Frames.Num();
}

TArray<TArray<uint8>> FO3DRecordingFrameConsumer::GetFrames() const
{
	FScopeLock Lock(&Mutex);
	return Frames;
}

TArray<uint32> FO3DRecordingFrameConsumer::GetCallerThreads() const
{
	FScopeLock Lock(&Mutex);
	return CallerThreads;
}

void FO3DRecordingFrameConsumer::Reset()
{
	FScopeLock Lock(&Mutex);
	Frames.Reset();
	CallerThreads.Reset();
}

#endif // WITH_DEV_AUTOMATION_TESTS
