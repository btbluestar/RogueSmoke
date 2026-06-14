// RogueMovementSet.h
// Movement-economy attributes for GAS (D-0023: Deadlock-style stamina pips).
// Spend/regen logic lives in AngelScript on the hero (URogueLocomotionComponent or hero BeginPlay).
// Meta-progression upgrades are GameplayEffects on MaxStamina.
//
// Subclasses UAngelscriptAttributeSet so AngelScript can read/write via the name-based ASC API
// (ASC.GetAttributeCurrentValue(URogueMovementSet, n"Stamina")) with no per-attribute C++ wrapper.

#pragma once

#include "CoreMinimal.h"
#include "AngelscriptAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "RogueMovementSet.generated.h"

#ifndef ATTRIBUTE_ACCESSORS
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)
#endif

UCLASS()
class ROGUESMOKE_API URogueMovementSet : public UAngelscriptAttributeSet
{
	GENERATED_BODY()

public:
	URogueMovementSet();

	ATTRIBUTE_ACCESSORS(URogueMovementSet, Stamina);
	ATTRIBUTE_ACCESSORS(URogueMovementSet, MaxStamina);

protected:
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	// UHT requires this on every class declaring replicated properties; the base does the actual
	// generic registration (it walks the most-derived class), so we just forward.
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// RepNotify trampolines: GAS needs SetBaseAttributeValueFromReplication on clients, which
	// UAngelscriptAttributeSet::OnRep_Attribute performs. One per replicated attribute (UHT does
	// not expand custom macros, so these are written out).
	UFUNCTION() void OnRep_Stamina(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_MaxStamina(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }

private:
	// Current pip count (0..MaxStamina). Replicated so teammates' HUDs and AngelScript on the
	// owning client can react to pip changes.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Stamina, Category = "Movement", meta = (HideFromModifiers, AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Stamina;

	// Maximum pip count. Upgraded by MaxStamina GameplayEffects (meta-progression D-0023).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxStamina, Category = "Movement", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MaxStamina;
};
