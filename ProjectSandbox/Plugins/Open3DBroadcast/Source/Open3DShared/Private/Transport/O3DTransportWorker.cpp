// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DTransportWorker.h"

#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Transport/O3DSendQueue.h"

class FO3DTransportWorker::FRunnableImpl final : public FRunnable
{
public:
	explicit FRunnableImpl(FO3DTransportWorker& InOwner)
		: Owner(InOwner)
	{
	}

	virtual uint32 Run() override
	{
		Owner.RunLoop();
		return 0;
	}

	virtual void Stop() override
	{
		// FRunnableThread may call this from its own shutdown path; same effect as the owner's Stop request.
		Owner.bStopRequested.store(true, std::memory_order_release);
		Owner.Wake();
	}

private:
	FO3DTransportWorker& Owner;
};

FO3DTransportWorker::FO3DTransportWorker() = default;

FO3DTransportWorker::~FO3DTransportWorker()
{
	Stop();
}

bool FO3DTransportWorker::Start(const TCHAR* ThreadName, FBody InBody, TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> InWakeQueue)
{
	if (Thread != nullptr || !InBody)
	{
		return false;
	}

	Body = MoveTemp(InBody);
	WakeQueue = MoveTemp(InWakeQueue);
	bStopRequested.store(false, std::memory_order_release);
	Iterations.store(0, std::memory_order_relaxed);
	Runnable = MakeUnique<FRunnableImpl>(*this);
	bRunning.store(true, std::memory_order_release);

	Thread = FRunnableThread::Create(Runnable.Get(), ThreadName ? ThreadName : TEXT("O3D_TransportWorker"));
	if (Thread == nullptr)
	{
		bRunning.store(false, std::memory_order_release);
		Runnable.Reset();
		Body = FBody();
		WakeQueue.Reset();
		return false;
	}
	return true;
}

void FO3DTransportWorker::Stop()
{
	bStopRequested.store(true, std::memory_order_release);
	if (Thread == nullptr)
	{
		return;
	}

	Wake();
	Thread->WaitForCompletion();
	delete Thread;
	Thread = nullptr;
	Runnable.Reset();
	// The thread has ended, so the body (and whatever it captured) is released here, on the caller.
	Body = FBody();
	// WakeQueue is kept until the next Start, so a late Wake() from another thread never races a reset.
	bRunning.store(false, std::memory_order_release);
}

void FO3DTransportWorker::Wake()
{
	if (WakeQueue.IsValid())
	{
		WakeQueue->Wake();
	}
	OwnEvent->Trigger();
}

void FO3DTransportWorker::WaitFor(uint32 WaitMs)
{
	if (WakeQueue.IsValid())
	{
		WakeQueue->WaitForWork(WaitMs);
	}
	else
	{
		OwnEvent->Wait(WaitMs);
	}
}

void FO3DTransportWorker::RunLoop()
{
	while (!bStopRequested.load(std::memory_order_acquire))
	{
		const uint32 WaitMs = Body();
		Iterations.fetch_add(1, std::memory_order_relaxed);
		if (WaitMs > 0 && !bStopRequested.load(std::memory_order_acquire))
		{
			WaitFor(WaitMs);
		}
	}
}

FO3DReconnectPolicy::FO3DReconnectPolicy(const FO3DReconnectPolicySettings& InSettings, int32 Seed)
	: Settings(InSettings)
{
	Settings.InitialDelaySeconds = FMath::Max(0.0, Settings.InitialDelaySeconds);
	Settings.MaxDelaySeconds = FMath::Max(Settings.InitialDelaySeconds, Settings.MaxDelaySeconds);
	Settings.Multiplier = FMath::Max(1.0, Settings.Multiplier);
	Settings.JitterFraction = FMath::Clamp(Settings.JitterFraction, 0.0, 1.0);
	Settings.MaxAttempts = FMath::Max(0, Settings.MaxAttempts);
	Random.Initialize(Seed != 0 ? Seed : static_cast<int32>(FPlatformTime::Cycles()));
}

double FO3DReconnectPolicy::GetBaseDelaySeconds(int32 Failure) const
{
	// Grow until the cap; the loop stops at the cap, so a long outage cannot overflow the double.
	double Delay = Settings.InitialDelaySeconds;
	for (int32 Step = 1; Step < Failure && Delay < Settings.MaxDelaySeconds; ++Step)
	{
		Delay *= Settings.Multiplier;
	}
	return FMath::Min(Delay, Settings.MaxDelaySeconds);
}

double FO3DReconnectPolicy::OnFailure(double NowSec)
{
	++FailedAttempts;
	if (IsExhausted())
	{
		NextAttemptSec = TNumericLimits<double>::Max();
		return -1.0;
	}

	double Delay = GetBaseDelaySeconds(FailedAttempts);
	if (Settings.JitterFraction > 0.0)
	{
		const double Jitter = static_cast<double>(Random.FRandRange(-1.0f, 1.0f)) * Settings.JitterFraction;
		Delay *= (1.0 + Jitter);
	}
	Delay = FMath::Clamp(Delay, 0.0, Settings.MaxDelaySeconds);
	NextAttemptSec = NowSec + Delay;
	return Delay;
}

void FO3DReconnectPolicy::OnSuccess()
{
	Reset();
}

void FO3DReconnectPolicy::Reset()
{
	FailedAttempts = 0;
	NextAttemptSec = 0.0;
}

bool FO3DReconnectPolicy::IsDue(double NowSec) const
{
	return !IsExhausted() && NowSec >= NextAttemptSec;
}

bool FO3DReconnectPolicy::IsExhausted() const
{
	return Settings.MaxAttempts > 0 && FailedAttempts >= Settings.MaxAttempts;
}
