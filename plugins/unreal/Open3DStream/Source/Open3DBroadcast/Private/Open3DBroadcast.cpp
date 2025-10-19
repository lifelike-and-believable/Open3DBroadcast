// Copyright Epic Games, Inc. All Rights Reserved.

#include "Open3DBroadcast.h"

#define LOCTEXT_NAMESPACE "FOpen3DBroadcastModule"

DEFINE_LOG_CATEGORY(LogO3DSBroadcast);

void FOpen3DBroadcastModule::StartupModule()
{
#if O3DS_WITH_BROADCAST
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	UE_LOG(LogTemp, Log, TEXT("Open3DBroadcast module started"));
#endif
}

void FOpen3DBroadcastModule::ShutdownModule()
{
#if O3DS_WITH_BROADCAST
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	UE_LOG(LogTemp, Log, TEXT("Open3DBroadcast module shutdown"));
#endif
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOpen3DBroadcastModule, Open3DBroadcast)