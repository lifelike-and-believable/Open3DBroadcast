// Copyright Epic Games, Inc. All Rights Reserved.

#include "Open3DBroadcast.h"

#define LOCTEXT_NAMESPACE "FOpen3DBroadcastModule"

void FOpen3DBroadcastModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FOpen3DBroadcastModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOpen3DBroadcastModule, Open3DBroadcast)
