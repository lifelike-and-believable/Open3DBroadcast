// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;

// Editor UI for the plugin (ADR 0010, WP-F7): the UO3DSenderComponent Details customization, the
// LiveLink "Add Source" panel of the receiver, and one generic transport options panel built from
// each transport's option schema. Type "Editor" in the .uplugin, so it loads only in editor
// targets; it ships in the Fab package. Runtime modules never depend on this module.
[SupportedTargetTypes(TargetType.Editor)]
public class Open3DBroadcastEditor : ModuleRules
{
    public Open3DBroadcastEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Same O3D_* definitions as the runtime modules whose headers it includes.
        O3DBuildFlags.Apply(Target, this);

        // /EHsc: O3DSenderComponent.h and O3DReceiverSource.h include the o3ds core headers, which
        // the Sender and Receiver modules compile with exceptions on (BUILD-5).
        bEnableExceptions = true;

        // Public, checked against Public/: SO3DTransportOptionsPanel and SO3DTransportConfigPanelBase
        // are Slate widgets (Slate, SlateCore); IO3DOptionTarget names the sender and receiver
        // UObject types and FO3DSecretStore types (Open3DShared, Open3DSender, Open3DReceiver).
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        // InputCore: EKeys in SO3DTransportConfigPanelBase::OnKeyDown. PropertyEditor: the Details
        // customization and the receiver panel's details view. UnrealEd: FScopedTransaction.
        // LiveLinkInterface: ULiveLinkSourceFactory::FOnLiveLinkSourceCreated. No EditorStyle: the
        // widgets use FAppStyle defaults.
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "InputCore",
            "PropertyEditor",
            "UnrealEd",
            "LiveLinkInterface"
        });
    }
}
