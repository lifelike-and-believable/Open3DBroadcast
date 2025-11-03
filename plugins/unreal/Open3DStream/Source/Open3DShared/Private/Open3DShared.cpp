// Copyright (c) Open3DStream Contributors

#include "Open3DShared.h"

void FOpen3DSharedModule::StartupModule()
{
    // Minimal startup to satisfy module initialization and allow dependents to load safely
    UE_LOG(LogTemp, Display, TEXT("Open3DShared module started"));
}

void FOpen3DSharedModule::ShutdownModule()
{
    UE_LOG(LogTemp, Display, TEXT("Open3DShared module shutdown"));
}

IMPLEMENT_MODULE(FOpen3DSharedModule, Open3DShared)
