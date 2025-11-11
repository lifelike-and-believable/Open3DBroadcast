using UnrealBuildTool;
//using O3DBroadcastBuild;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportSockets : ModuleRules
{
    public Open3DTransportSockets(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        if (!O3DBuildFlags.IsSocketsEnabled(Target))
        {
            return;
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Sockets",
            "Networking"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });
    }
}
