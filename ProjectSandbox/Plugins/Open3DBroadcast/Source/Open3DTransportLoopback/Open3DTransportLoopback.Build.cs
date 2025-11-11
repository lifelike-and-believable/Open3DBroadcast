using UnrealBuildTool;
//using O3DBroadcastBuild;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportLoopback : ModuleRules
{
    public Open3DTransportLoopback(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        if (!O3DBuildFlags.IsSenderEnabled(Target) || !O3DBuildFlags.IsReceiverEnabled(Target))
        {
            return;
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Slate",
                "SlateCore",
                "AppFramework"
            });
        }
    }
}
