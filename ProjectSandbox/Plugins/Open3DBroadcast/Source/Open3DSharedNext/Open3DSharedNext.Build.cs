using UnrealBuildTool;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DSharedNext : ModuleRules
{
    public Open3DSharedNext(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[] {});
    }
}
