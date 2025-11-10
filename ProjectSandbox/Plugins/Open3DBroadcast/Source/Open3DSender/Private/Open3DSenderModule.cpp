#include "Modules/ModuleManager.h"
#include "O3DSenderLogs.h"

#if WITH_EDITOR
#include "PropertyEditorModule.h"
#include "O3DSenderComponentCustomization.h"
#include "O3DSenderComponent.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY(LogO3DSender);
DEFINE_LOG_CATEGORY(LogO3DSenderComponent);
DEFINE_LOG_CATEGORY(LogO3DSenderSerializer);
DEFINE_LOG_CATEGORY(LogO3DSenderAudio);

class FOpen3DSenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogO3DSender, Verbose, TEXT("Open3DSender module started"));

#if WITH_EDITOR
		if (GIsEditor)
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.RegisterCustomClassLayout(UO3DSenderComponent::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FO3DSenderComponentCustomization::MakeInstance));
			PropertyModule.NotifyCustomizationModuleChanged();
		}
#endif // WITH_EDITOR
	}

	virtual void ShutdownModule() override
	{
#if WITH_EDITOR
		if (GIsEditor)
		{
			if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
			{
				FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
				PropertyModule.UnregisterCustomClassLayout(UO3DSenderComponent::StaticClass()->GetFName());
				PropertyModule.NotifyCustomizationModuleChanged();
			}
		}
#endif // WITH_EDITOR
		UE_LOG(LogO3DSender, Verbose, TEXT("Open3DSender module shutdown"));
	}
};

IMPLEMENT_MODULE(FOpen3DSenderModule, Open3DSender)
