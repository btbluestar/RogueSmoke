// RogueHealthSet.cpp
#include "RogueHealthSet.h"

#include "GameplayEffectExtension.h"
#include "GameplayEffectTypes.h"
#include "Net/UnrealNetwork.h"

URogueHealthSet::URogueHealthSet()
{
	InitHealth(100.0f);
	InitMaxHealth(100.0f);
	InitShield(0.0f);
	InitMaxShield(0.0f);
	InitArmor(0.0f);
	InitDamage(0.0f);
	InitHealing(0.0f);
}

void URogueHealthSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void URogueHealthSet::ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetShieldAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxShield());
	}
	else if (Attribute == GetMaxHealthAttribute() || Attribute == GetMaxShieldAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}

void URogueHealthSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	ClampAttribute(Attribute, NewValue);
}

void URogueHealthSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	// Server-authoritative. Resolve the meta attributes into Health, applying Armor then Shield.
	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		const float LocalDamage = GetDamage();
		SetDamage(0.0f);

		if (LocalDamage > 0.0f)
		{
			const float ArmorValue = GetArmor();
			const float Mitigated = (ArmorValue >= 0.0f)
				? LocalDamage * (100.0f / (100.0f + ArmorValue))
				: LocalDamage * (2.0f - 100.0f / (100.0f - ArmorValue));

			float Remaining = Mitigated;
			const float CurrentShield = GetShield();
			if (CurrentShield > 0.0f)
			{
				const float Absorbed = FMath::Min(CurrentShield, Remaining);
				SetShield(CurrentShield - Absorbed);
				Remaining -= Absorbed;
			}

			SetHealth(FMath::Clamp(GetHealth() - Remaining, 0.0f, GetMaxHealth()));
		}
	}
	else if (Data.EvaluatedData.Attribute == GetHealingAttribute())
	{
		const float LocalHealing = GetHealing();
		SetHealing(0.0f);
		SetHealth(FMath::Clamp(GetHealth() + LocalHealing, 0.0f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	}

	if (GetHealth() <= 0.0f && !bOutOfHealth)
	{
		bOutOfHealth = true;
		OnOutOfHealth.Broadcast(GetOwningActor());
	}
	bOutOfHealth = GetHealth() <= 0.0f;
}
