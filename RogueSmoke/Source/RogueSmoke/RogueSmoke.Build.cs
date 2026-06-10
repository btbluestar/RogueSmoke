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
			"Slate",
			// GAS (Lyra-style ability system; ASC bound to AngelScript via the AngelscriptGAS plugin).
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			// AngelscriptGAS exposes UAngelscriptAbilitySystemComponent / UAngelscriptAttributeSet etc.
			// (the C++ bases our PlayerState/HeroBase/attribute sets build on).
			"AngelscriptGAS",
			// CommonUI: layer-stack push shim (BP_AddWidget is private, so AngelScript can't
			// reflect it — RogueUIStatics wraps the public C++ template AddWidget instead).
			"CommonUI"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"RogueSmoke",
			"RogueSmoke/AbilitySystem",
			"RogueSmoke/AbilitySystem/Attributes",
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
