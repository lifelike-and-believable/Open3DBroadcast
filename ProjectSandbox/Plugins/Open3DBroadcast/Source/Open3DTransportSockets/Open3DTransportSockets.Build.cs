using UnrealBuildTool;
using System.Collections.Generic;
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

        List<string> PrivateModules = new List<string>
        {
            "Open3DShared"
        };

        if (O3DBuildFlags.IsSenderEnabled(Target))
        {
            PrivateModules.Add("Open3DSender");
        }

        if (O3DBuildFlags.IsReceiverEnabled(Target))
        {
            PrivateModules.Add("Open3DReceiver");
        }

        PrivateDependencyModuleNames.AddRange(PrivateModules);

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
