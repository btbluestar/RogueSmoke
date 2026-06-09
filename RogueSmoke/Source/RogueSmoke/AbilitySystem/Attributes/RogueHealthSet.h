// RogueHealthSet.h
// Player survival attributes for GAS (Lyra-style: ULyraHealthSet is the reference).
// Replaces the survival half of the old AngelScript UStatsComponent. Written in C++ on purpose
// (attribute-set replication/accessors are cleaner in C++); abilities live in AngelScript.
//
// Subclasses UAngelscriptAttributeSet so AngelScript can read/write via the name-based ASC API
// (ASC.GetAttributeCurrentValue(URogueHealthSet, n"Health")) with no per-attribute C++ wrapper.
//
// Damage model (mirrors the retired UStatsComponent): incoming Damage is mitigated by Armor,
// soaked by Shield, then removed from Health — all in PostGameplayEffectExecute on the server.

#pragma once

#include "CoreMinimal.h"
#include "AngelscriptAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "RogueHealthSet.generated.h"

// Standard GAS accessor bundle (Get/Set/Init + static FGameplayAttribute getter).
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

// Broadcast on the server the first time Health reaches 0. The AngelScript hero subscribes to
// drive death/down handling (replaces UStatsComponent::OnDeath).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRogueOutOfHealthDelegate, AActor*, OwningActor);

UCLASS()
class ROGUESMOKE_API URogueHealthSet : public UAngelscriptAttributeSet
{
	GENERATED_BODY()

public:
	URogueHealthSet();

	ATTRIBUTE_ACCESSORS(URogueHealthSet, Health);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, MaxHealth);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, Shield);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, MaxShield);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, Armor);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, Damage);
	ATTRIBUTE_ACCESSORS(URogueHealthSet, Healing);

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FRogueOutOfHealthDelegate OnOutOfHealth;

protected:
	// Authoritative attribute resolution (Damage -> Health via Armor/Shield, clamps, death event).
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	// UHT requires this on every class declaring replicated properties; the base does the actual
	// generic registration (it walks the most-derived class), so we just forward.
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const;

	// RepNotify trampolines: GAS needs SetBaseAttributeValueFromReplication on clients, which
	// UAngelscriptAttributeSet::OnRep_Attribute performs. One per replicated attribute (UHT does
	// not expand custom macros, so these are written out).
	UFUNCTION() void OnRep_Health(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_MaxHealth(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_Shield(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_MaxShield(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_Armor(const FAngelscriptGameplayAttributeData& Old)     { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }

private:
	// ---- Stateful, replicated (teammate HUDs read these) ----
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Health", meta = (HideFromModifiers, AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Health;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Health", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MaxHealth;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Shield, Category = "Health", meta = (HideFromModifiers, AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Shield;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxShield, Category = "Health", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MaxShield;

	// RoR2-style mitigation: mitigated = Damage * 100/(100+Armor). Negative armor amplifies.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Armor, Category = "Health", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Armor;

	// ---- Meta attributes (transient, not replicated; only executions write them) ----
	// Incoming damage, mapped to -Health (after Armor/Shield) in PostGameplayEffectExecute.
	UPROPERTY(BlueprintReadOnly, Category = "Health", meta = (HideFromModifiers, AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Damage;

	// Incoming healing, mapped to +Health.
	UPROPERTY(BlueprintReadOnly, Category = "Health", meta = (HideFromModifiers, AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Healing;

	bool bOutOfHealth = false;
};
