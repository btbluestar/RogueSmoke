// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// unreal-test-mcp — runtime module. No editor dependencies; stays cook-safe for future runtime harness use.

using UnrealBuildTool;

public class UeMcpRuntime : ModuleRules
{
    public UeMcpRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            // TestFixtures content pack (Wave E Gap 5.5):
            "AIModule",            // AAIController, MoveToActor
            "NavigationSystem",    // navmesh runtime for zombie pathing
            "GameplayTasks",       // transitive dep surfaced by AIModule headers
            // AUeMcpTestFixture base class (Wave E F9) — AFunctionalTest subclass
            // exposed in module public headers.
            "FunctionalTesting"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Json",
            "JsonUtilities",
            "Sockets",
            "Networking"
        });
    }
}
