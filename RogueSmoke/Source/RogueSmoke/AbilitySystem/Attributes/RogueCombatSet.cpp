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
}
