#include "Open3DSharedNext.h"

#define LOCTEXT_NAMESPACE "FOpen3DSharedNextModule"

DEFINE_LOG_CATEGORY(LogOpen3DSharedNext);

void FOpen3DSharedNextModule::StartupModule()
{
    UE_LOG(LogOpen3DSharedNext, Display, TEXT("Open3DSharedNext module started"));
}

void FOpen3DSharedNextModule::ShutdownModule()
{
    UE_LOG(LogOpen3DSharedNext, Display, TEXT("Open3DSharedNext module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpen3DSharedNextModule, Open3DSharedNext)
