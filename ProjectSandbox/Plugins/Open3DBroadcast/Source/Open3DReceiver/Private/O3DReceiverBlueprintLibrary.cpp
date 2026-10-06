// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DReceiverBlueprintLibrary.h"

#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkSourceSettings.h"
#include "O3DReceiverSource.h"
#include "O3DReceiverSourceFactory.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "Transport/O3DTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogO3DReceiverBlueprint, Log, All);

bool UO3DReceiverBlueprintLibrary::CreateLiveLinkSource(FName TransportName, const TMap<FString, FString>& Options, FName ContextName, bool bEnableAudio, FLiveLinkSourceHandle& SourceHandle)
{
	SourceHandle.SetSourcePointer(nullptr);

	if (!FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver).Contains(TransportName))
	{
		UE_LOG(LogO3DReceiverBlueprint, Warning, TEXT("Create Open3DStream LiveLink Source: no receiver is registered for transport '%s'."), *TransportName.ToString());
		return false;
	}

	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	if (!ModularFeatures.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		UE_LOG(LogO3DReceiverBlueprint, Warning, TEXT("Create Open3DStream LiveLink Source: LiveLink is not available (is the Live Link plugin enabled?)."));
		return false;
	}
	ILiveLinkClient& LiveLinkClient = ModularFeatures.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

	FO3DReceiverSourceConfig Config;
	Config.TransportName = TransportName;
	Config.ContextName = ContextName;
	Config.bEnableAudio = bEnableAudio;
	Config.TransportOptions = Options;
	// A credential never stays in the settings LiveLink saves (ADR 0004 item 4).
	O3DReceiver::MigrateLegacySecretOptions(Config, TEXT("the options given to Create Open3DStream LiveLink Source"));

	const TSharedPtr<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	const FGuid SourceGuid = LiveLinkClient.AddSource(Source);
	if (!SourceGuid.IsValid())
	{
		UE_LOG(LogO3DReceiverBlueprint, Warning, TEXT("Create Open3DStream LiveLink Source: LiveLink did not add the source."));
		return false;
	}

	// As a source created in the LiveLink panel, so a LiveLink preset can save and recreate it
	// (the same steps as LiveLink's own ULiveLinkMessageBusFinder::ConnectToProvider).
	if (ULiveLinkSourceSettings* Settings = LiveLinkClient.GetSourceSettings(SourceGuid))
	{
		Settings->ConnectionString = O3DReceiver::ExportConnectionString(Config);
		Settings->Factory = UO3DReceiverSourceFactory::StaticClass();
	}

	SourceHandle.SetSourcePointer(Source);
	return true;
}
