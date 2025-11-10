using UnrealBuildTool;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportSockets : ModuleRules
{
    public Open3DTransportSockets(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
            "Open3DSharedNext",
            "Open3DSender",
            "Open3DReceiver"
        });
    }
}
