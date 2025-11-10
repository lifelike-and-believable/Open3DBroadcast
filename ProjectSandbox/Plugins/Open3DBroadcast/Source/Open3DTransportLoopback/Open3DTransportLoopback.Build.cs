using UnrealBuildTool;
//using O3DBroadcastBuild;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportLoopback : ModuleRules
{
    public Open3DTransportLoopback(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        //O3DModuleRules.ApplyTransportDefines(this);

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
