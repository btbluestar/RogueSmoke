// RogueCombatSet.cpp
#include "RogueCombatSet.h"
#include "Net/UnrealNetwork.h"

URogueCombatSet::URogueCombatSet()
{
	InitMoveSpeed(600.0f);
	InitAbilityPower(1.0f);
	InitCooldownReduction(0.0f);
	InitBarrageRadiusBonus(0.0f);
	InitBarrageClusterBonus(0.0f);
	InitWeaponDamageBonus(0.0f);
	InitFireRateBonus(0.0f);
	InitPierceCount(0.0f);
	InitChainCount(0.0f);
	InitBurnChance(0.0f);
	InitPoisonChance(0.0f);
	InitMagazineBonus(0.0f);
	InitReloadSpeedBonus(0.0f);
}

void URogueCombatSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void URogueCombatSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetCooldownReductionAttribute())
	{
		// Matches the retired UStatsComponent clamp; keeps the taunt->barrage loop from going free.
		NewValue = FMath::Clamp(NewValue, 0.0f, 0.9f);
	}
	else if (Attribute == GetMoveSpeedAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetBurnChanceAttribute() || Attribute == GetPoisonChanceAttribute())
	{
		// Proc chances are probabilities; 4 stacks of a +0.25 card = guaranteed, not >100%.
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetWeaponDamageBonusAttribute() || Attribute == GetFireRateBonusAttribute() ||
	         Attribute == GetPierceCountAttribute() || Attribute == GetChainCountAttribute() ||
	         Attribute == GetMagazineBonusAttribute() || Attribute == GetReloadSpeedBonusAttribute())
	{
		// No negative weapon stats — a debuff system would want its own design pass, not underflow.
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}
