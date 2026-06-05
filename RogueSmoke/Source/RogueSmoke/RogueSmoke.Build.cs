// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class RogueSmoke : ModuleRules
{
	public RogueSmoke(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"NetCore",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"RogueSmoke",
			"RogueSmoke/Combat",
			"RogueSmoke/Enemies",
			"RogueSmoke/Spawning",
			"RogueSmoke/Variant_Horror",
			"RogueSmoke/Variant_Horror/UI",
			"RogueSmoke/Variant_Shooter",
			"RogueSmoke/Variant_Shooter/AI",
			"RogueSmoke/Variant_Shooter/UI",
			"RogueSmoke/Variant_Shooter/Weapons"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
