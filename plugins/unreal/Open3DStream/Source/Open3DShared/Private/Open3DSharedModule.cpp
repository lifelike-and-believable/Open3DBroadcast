// Copyright (c) Open3DStream Contributors

#include "Modules/ModuleManager.h"

class FOpen3DSharedModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // Minimal startup to satisfy module initialization and allow dependents to load safely
        UE_LOG(LogTemp, Display, TEXT("Open3DShared module started"));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogTemp, Display, TEXT("Open3DShared module shutdown"));
    }
};

IMPLEMENT_MODULE(FOpen3DSharedModule, Open3DShared)
