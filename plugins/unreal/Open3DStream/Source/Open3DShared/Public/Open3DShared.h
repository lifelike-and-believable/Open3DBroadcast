// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Open3DShared module: Provides shared WebRTC, Opus, and utility code
 * used by both Open3DStream (receiver) and Open3DBroadcast (sender) modules.
 */
class OPEN3DSHARED_API FOpen3DSharedModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
