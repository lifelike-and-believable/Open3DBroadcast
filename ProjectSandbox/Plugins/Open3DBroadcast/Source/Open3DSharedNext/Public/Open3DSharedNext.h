// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOpen3DSharedNext, Log, All);

class OPEN3DSHAREDNEXT_API FOpen3DSharedNextModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};