// Copyright Lifelike & Believable. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

// The Open3DStream core (src/o3ds) compiled from source, so the plugin needs no prebuilt core
// library and builds with RunUAT BuildPlugin alone (docs/adr/0003, WP-F1).
//
// Layout:
//   Source/ThirdParty/Open3DStreamCore/   generated mirror of the core, o3ds_generated.h, the
//                                          FlatBuffers runtime headers and CRCpp's CRC.h
//                                          (Build/Scripts/sync_o3ds_core.py; see SYNC_STAMP.txt)
//   Private/Core/O3DSCore_*.cpp            generated: one translation unit per mirrored .cpp,
//                                          compiled with the core's warnings switched off
//   Private/Open3DStreamCoreModule.cpp     module boilerplate and the FlatBuffers version check
//
// The mirror sits outside this module folder because UBT compiles every .cpp under a module
// folder; the core sources are compiled only through the Private/Core wrappers.
//
// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DStreamCore : ModuleRules
{
    public Open3DStreamCore(ReadOnlyTargetRules Target) : base(Target)
    {
        // The core translation units include no engine headers beyond HAL/Platform.h, so no
        // shared PCH is forced into them.
        PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;

        // The core sources keep file-local helpers with the same names in several files, which
        // would collide in a unity translation unit.
        bUseUnity = false;

        // The core uses neither exceptions nor RTTI; sync_o3ds_core.py fails if that changes.
        bEnableExceptions = false;
        bUseRTTI = false;

        O3DBuildFlags.Apply(Target, this);

        string CoreDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Open3DStreamCore"));

        // "o3ds/model.h", "o3ds_generated.h" and "flatbuffers/flatbuffers.h" resolve from here, for
        // this module and every module that depends on it. Third-party headers: system include
        // (BUILD-3). CoreDir/o3ds itself is never an include path: o3ds/math.h would shadow <math.h>.
        PublicSystemIncludePaths.Add(CoreDir);

        // CRC.h is used by o3ds/model.cpp only.
        PrivateIncludePaths.Add(Path.Combine(CoreDir, "crccpp"));
        PrivateDefinitions.Add("CRCPP_USE_NAMESPACE");

        // O3DS_API (src/o3ds/o3ds_export.h) exports the core's classes and functions from this
        // module in modular builds and imports them in the modules that depend on it.
        PublicDefinitions.Add("O3DS_API=OPEN3DSTREAMCORE_API");

        PublicDependencyModuleNames.Add("Core");
    }
}
