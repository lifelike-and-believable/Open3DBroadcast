#include "Receiver/MoQReceiver.h"

DEFINE_LOG_CATEGORY(LogO3DMoQReceiver);

FO3DMoQReceiver::FO3DMoQReceiver()
{
	UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("FO3DMoQReceiver created (stub implementation)"));
}

FO3DMoQReceiver::~FO3DMoQReceiver()
{
	UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("FO3DMoQReceiver destroyed"));
}

bool FO3DMoQReceiver::Initialize(const FO3DTransportConfig& Config)
{
	UE_LOG(LogO3DMoQReceiver, Warning, TEXT("FO3DMoQReceiver::Initialize() - stub implementation (Phase 0)"));
	UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver not yet implemented - Phase 0 build verification only"));
	return false;
}

void FO3DMoQReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DMoQReceiver::Start()
{
	UE_LOG(LogO3DMoQReceiver, Warning, TEXT("FO3DMoQReceiver::Start() - stub implementation"));
	return false;
}

void FO3DMoQReceiver::Stop()
{
	UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("FO3DMoQReceiver::Stop()"));
}

int32 FO3DMoQReceiver::Poll()
{
	// Stub - no data available
	return 0;
}

FO3DTransportStats FO3DMoQReceiver::GetStats() const
{
	return Stats;
}
