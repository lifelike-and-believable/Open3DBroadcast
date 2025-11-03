// Copyright (c) Open3DStream Contributors

#include "Modules/ModuleManager.h"
#include "Open3DShared.h"

#define LOCTEXT_NAMESPACE "FOpen3DSharedModule"

void FOpen3DSharedModule::StartupModule()
{
    // Minimal startup to satisfy module initialization and allow dependents to load safely
    UE_LOG(LogTemp, Display, TEXT("Open3DShared module started"));
}

void FOpen3DSharedModule::ShutdownModule()
{
    UE_LOG(LogTemp, Display, TEXT("Open3DShared module shutdown"));
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FOpen3DSharedModule, Open3DShared)
