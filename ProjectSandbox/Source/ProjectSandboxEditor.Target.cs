// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class ProjectSandboxEditorTarget : TargetRules
{
	public ProjectSandboxEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		// The settings of the engine that builds (ADR 0014: UE 5.7 and 5.8); UnrealBuildTool defines
		// UE_5_<n>_OR_LATER up to its own minor version.
#if UE_5_8_OR_LATER
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
#else
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
#endif

		ExtraModuleNames.AddRange( new string[] { "ProjectSandbox" } );
	}
}
