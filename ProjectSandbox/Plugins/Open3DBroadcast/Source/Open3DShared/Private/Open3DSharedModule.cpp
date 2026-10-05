// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "O3DRuntimeContext.h"
#include "O3DSharedLogs.h"

DEFINE_LOG_CATEGORY(LogO3DShared);


#define LOCTEXT_NAMESPACE "FOpen3DSharedModule"

/** Houses shared utilities (transport types, helpers) consumed by sender/receiver modules. */
class FOpen3DSharedModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // Create the default runtime context now rather than on first use (ADR 0012). Like the
        // metrics singleton it replaces, it lives until static destruction: transports may still
        // reach it from their own threads.
        FO3DRuntimeContext::Default();
        UE_LOG(LogO3DShared, Display, TEXT("Open3DShared module started"));
    }
    virtual void ShutdownModule() override
    {
        UE_LOG(LogO3DShared, Display, TEXT("Open3DShared module shutdown"));
    }
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpen3DSharedModule, Open3DShared)
