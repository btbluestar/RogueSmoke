// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// unreal-test-mcp — editor module. UncookedOnly per design; test authoring, PIE control, reflection host.

using UnrealBuildTool;

public class UeMcpEditor : ModuleRules
{
    public UeMcpEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "GameplayTags",        // Wave Fan-out 7 — tags.* handlers
            "UeMcpRuntime"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",
            "EditorSubsystem",
            "AutomationController",
            "FunctionalTesting",
            "Kismet",
            "BlueprintGraph",
            "AssetRegistry",
            "InputCore",           // input handlers — FKey + EKeys + FInputKeyEventArgs
            "LevelEditor",         // Wave E F3 — SLevelViewport for viewport.screenshot
            "NavigationSystem",    // Wave E F4 — plugin.rebuild_test_arena
            "RenderCore",          // perf.frame_stats — GGameThreadTime / GRenderThreadTime
            "RHI",                 // perf.frame_stats — RHIGetGPUFrameCycles
            "AIModule",            // Wave F Agent-11 — ai.* blackboard + start_bt
            "GameplayTasks",       // transitive AIModule dep
            "PythonScriptPlugin",
            "Projects",
            "Json",
            "JsonUtilities",
            "Slate",
            "SlateCore",
            "UMG"                  // Wave F (agent-5) — UMG widget interaction handlers (ui.*)
        });
    }
}
