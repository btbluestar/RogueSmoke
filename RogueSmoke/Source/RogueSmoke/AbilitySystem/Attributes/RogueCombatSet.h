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
	ATTRIBUTE_ACCESSORS(URogueCombatSet, WeaponDamageBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, FireRateBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, PierceCount);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, ChainCount);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BurnChance);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, PoisonChance);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, MagazineBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, ReloadSpeedBonus);

protected:
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	// UHT requires this on every class declaring replicated properties; base does the real work.
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_MoveSpeed(const FAngelscriptGameplayAttributeData& Old)           { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_AbilityPower(const FAngelscriptGameplayAttributeData& Old)         { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_CooldownReduction(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageRadiusBonus(const FAngelscriptGameplayAttributeData& Old)   { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageClusterBonus(const FAngelscriptGameplayAttributeData& Old)  { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_WeaponDamageBonus(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_FireRateBonus(const FAngelscriptGameplayAttributeData& Old)        { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_PierceCount(const FAngelscriptGameplayAttributeData& Old)          { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_ChainCount(const FAngelscriptGameplayAttributeData& Old)           { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BurnChance(const FAngelscriptGameplayAttributeData& Old)           { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_PoisonChance(const FAngelscriptGameplayAttributeData& Old)         { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_MagazineBonus(const FAngelscriptGameplayAttributeData& Old)        { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_ReloadSpeedBonus(const FAngelscriptGameplayAttributeData& Old)     { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }

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

	// --- Weapon upgrade track (WEAPON_UPGRADES_PLAN.md). Bonuses are additive fractions applied as
	// effective = base * (1 + bonus); counts/chances are raw values. All default 0 so an unupgraded
	// weapon behaves exactly as its DataAsset says. Instant ADD_BASE GEs stack on repeat picks. ---

	// Scales weapon hit damage (GA_WeaponFire). Distinct from AbilityPower: separate build axes.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_WeaponDamageBonus, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData WeaponDamageBonus;

	// Shrinks the refire interval: FireInterval / (1 + FireRateBonus).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_FireRateBonus, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData FireRateBonus;

	// Extra enemies a bullet passes through (floor'd to int by the seam).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PierceCount, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData PierceCount;

	// Chain arcs per enemy hit (nearest others in range take a damage fraction).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ChainCount, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData ChainCount;

	// 0..1 per-hit proc chances; DoT magnitude derives from the hit's damage (see FWeaponShotParams).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BurnChance, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BurnChance;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PoisonChance, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData PoisonChance;

	// Magazine size * (1 + MagazineBonus), rounded up.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MagazineBonus, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MagazineBonus;

	// Shrinks reload time: ReloadSeconds / (1 + ReloadSpeedBonus).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ReloadSpeedBonus, Category = "Weapon", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData ReloadSpeedBonus;
};
