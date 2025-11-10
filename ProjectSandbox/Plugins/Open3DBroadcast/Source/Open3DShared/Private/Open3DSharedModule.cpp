#include "Modules/ModuleManager.h"
#include "O3DSharedLogs.h"

DEFINE_LOG_CATEGORY(LogO3DShared);


#define LOCTEXT_NAMESPACE "FOpen3DSharedModule"

class FOpen3DSharedModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogO3DShared, Display, TEXT("Open3DShared module started"));
    }
    virtual void ShutdownModule() override
    {
        UE_LOG(LogO3DShared, Display, TEXT("Open3DShared module shutdown"));
    }
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpen3DSharedModule, Open3DShared)
