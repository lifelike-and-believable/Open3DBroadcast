// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "LiveLinkSourceFactory.h"
#include "Modules/ModuleManager.h"
#include "O3DReceiverSourceFactory.h"
#include "O3DSenderComponent.h"
#include "O3DSenderComponentCustomization.h"
#include "PropertyEditorModule.h"
#include "SO3DReceiverSourceFactoryPanel.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DBroadcastEditor, Log, All);

/**
 * Editor UI of the plugin (ADR 0010, WP-F7). Loads at PostEngineInit, after the runtime modules
 * have registered their transports, and before any Details panel or LiveLink panel can open.
 *
 * - Registers the UO3DSenderComponent Details customization.
 * - Gives UO3DReceiverSourceFactory its LiveLink "Add Source" panel.
 * Both are undone in ShutdownModule.
 */
class FOpen3DBroadcastEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		SenderComponentClassName = UO3DSenderComponent::StaticClass()->GetFName();
		PropertyModule.RegisterCustomClassLayout(SenderComponentClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(&FO3DSenderComponentCustomization::MakeInstance));
		PropertyModule.NotifyCustomizationModuleChanged();

		O3DReceiver::SetSourceFactoryPanelBuilder([](ULiveLinkSourceFactory::FOnLiveLinkSourceCreated OnSourceCreated) -> TSharedPtr<SWidget>
		{
			return SNew(SO3DReceiverSourceFactoryPanel)
				.OnSourceCreated(MoveTemp(OnSourceCreated));
		});

		UE_LOG(LogOpen3DBroadcastEditor, Verbose, TEXT("Open3DBroadcastEditor module started"));
	}

	virtual void ShutdownModule() override
	{
		// The Receiver module may outlive this one; it must not call into unloaded code.
		O3DReceiver::SetSourceFactoryPanelBuilder(nullptr);

		if (!SenderComponentClassName.IsNone() && FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomClassLayout(SenderComponentClassName);
			PropertyModule.NotifyCustomizationModuleChanged();
		}
		SenderComponentClassName = NAME_None;

		UE_LOG(LogOpen3DBroadcastEditor, Verbose, TEXT("Open3DBroadcastEditor module shut down"));
	}

private:
	/** Kept from StartupModule: UO3DSenderComponent::StaticClass() may be gone during shutdown. */
	FName SenderComponentClassName = NAME_None;
};

IMPLEMENT_MODULE(FOpen3DBroadcastEditorModule, Open3DBroadcastEditor)
