// RogueCombatSet.h
// Offense / mobility / ability-tuning attributes for GAS. Replaces the offense+mobility half of
// the retired AngelScript UStatsComponent. C++ for clean replication; AngelScript reads via the
// name-based ASC API and GameplayEffects (incl. roguelike upgrades) modify these.
//
// Lean by design: only the attributes the current abilities/upgrades use. Add new stats here
// (Crit, AttackSpeed, Luck, PickupRadius, ...) by copying one attribute + its OnRep trampoline.

#pragma once

#include "CoreMinimal.h"
#include "AngelscriptAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "RogueCombatSet.generated.h"

#ifndef ATTRIBUTE_ACCESSORS
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)
#endif

UCLASS()
class ROGUESMOKE_API URogueCombatSet : public UAngelscriptAttributeSet
{
	GENERATED_BODY()

public:
	URogueCombatSet();

	ATTRIBUTE_ACCESSORS(URogueCombatSet, MoveSpeed);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, AbilityPower);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, CooldownReduction);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BarrageRadiusBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BarrageClusterBonus);

protected:
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	// UHT requires this on every class declaring replicated properties; base does the real work.
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_MoveSpeed(const FAngelscriptGameplayAttributeData& Old)           { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_AbilityPower(const FAngelscriptGameplayAttributeData& Old)         { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_CooldownReduction(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageRadiusBonus(const FAngelscriptGameplayAttributeData& Old)   { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageClusterBonus(const FAngelscriptGameplayAttributeData& Old)  { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }

private:
	// Drives ACharacter::CharacterMovement->MaxWalkSpeed (hero listens for changes).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MoveSpeed, Category = "Combat", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MoveSpeed;

	// Scales ability damage / effect magnitude.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AbilityPower, Category = "Combat", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData AbilityPower;

	// 0..0.9: shortens ability cooldowns (drives the Cooldown GE duration).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CooldownReduction, Category = "Combat", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData CooldownReduction;

	// Upgrade-tunable Barrage payoff (Chain Detonation adds to these via a GameplayEffect).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BarrageRadiusBonus, Category = "Combat", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BarrageRadiusBonus;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BarrageClusterBonus, Category = "Combat", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BarrageClusterBonus;
};
