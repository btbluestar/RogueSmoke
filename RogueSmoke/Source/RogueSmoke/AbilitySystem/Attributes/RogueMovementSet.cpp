// RogueMovementSet.cpp
#include "RogueMovementSet.h"

#include "Net/UnrealNetwork.h"

URogueMovementSet::URogueMovementSet()
{
	InitStamina(3.0f);
	InitMaxStamina(3.0f);
}

void URogueMovementSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void URogueMovementSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetStaminaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxStamina());
	}
	else if (Attribute == GetMaxStaminaAttribute())
	{
		// At least one pip must be the ceiling; prevents divide-by-zero in pip UI.
		NewValue = FMath::Max(NewValue, 1.0f);
	}
}
