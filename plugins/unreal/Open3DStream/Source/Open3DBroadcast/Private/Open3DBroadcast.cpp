// Copyright Epic Games, Inc. All Rights Reserved.

#include "Open3DBroadcast.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "IDetailCustomization.h"
#include "O3DSBroadcastComponentCustomization.h"

class FO3DSBroadcastComponentCustomization;

#define LOCTEXT_NAMESPACE "FOpen3DBroadcastModule"

DEFINE_LOG_CATEGORY(LogO3DSBroadcast);

void FOpen3DBroadcastModule::StartupModule()
{
#if O3DS_WITH_BROADCAST
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	UE_LOG(LogTemp, Log, TEXT("Open3DBroadcast module started"));

#if WITH_EDITOR
	FPropertyEditorModule& PropModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropModule.RegisterCustomClassLayout(
		"O3DSBroadcastComponent",
		FOnGetDetailCustomizationInstance::CreateStatic(&FO3DSBroadcastComponentCustomization::MakeInstance)
	);
	PropModule.NotifyCustomizationModuleChanged();
#endif
#endif
}

void FOpen3DBroadcastModule::ShutdownModule()
{
#if O3DS_WITH_BROADCAST
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	UE_LOG(LogTemp, Log, TEXT("Open3DBroadcast module shutdown"));
#if WITH_EDITOR
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropModule.UnregisterCustomClassLayout("O3DSBroadcastComponent");
		PropModule.NotifyCustomizationModuleChanged();
	}
#endif
#endif
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOpen3DBroadcastModule, Open3DBroadcast)