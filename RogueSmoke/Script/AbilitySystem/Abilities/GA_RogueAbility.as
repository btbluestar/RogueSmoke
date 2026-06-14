// GA_RogueAbility.as
// Base GAS ability for RogueSmoke heroes. Replaces the retired custom UAbilityComponent flow
// (D-0006) with real GAS: GAS owns cooldown/cost/activation/replication; the ability body does
// the server-authoritative combat through the seam (UCombatSubsystem) and plays cosmetics.
//
// Subclasses UAngelscriptGASAbility (the AngelscriptGAS plugin base). Concrete abilities override
// ActivateAbility(). Cooldown is a GameplayEffect assigned on the ability (CooldownGameplayEffectClass),
// applied by CommitAbility() — CooldownReduction (URogueCombatSet) feeds its duration via an MMC.
//
// Net model: InstancedPerActor + ServerOnly. Activation is server-initiated (the controller routes
// input through the pawn's Server_ActivateAbilityInput RPC), so the ability body runs authoritatively
// on the server; cosmetics replicate via GameplayCues. Game-state mutation is still gated on HasAuthority().
class UGA_RogueAbility : UAngelscriptGASAbility
{
    default InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    default NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

    // The hero pawn this ability acts through (avatar). Null-safe.
    AHeroCharacter GetHero() const
    {
        return Cast<AHeroCharacter>(GetAvatarActorFromActorInfo());
    }

    // World location the ability acts at. MVP: the caster. Override for aim-reticle abilities.
    FVector GetActivationLocation() const
    {
        AActor Avatar = GetAvatarActorFromActorInfo();
        return Avatar != nullptr ? Avatar.GetActorLocation() : FVector::ZeroVector;
    }

    // Current value of an attribute on the owner's combat set (e.g. upgrade-tuned ability magnitudes).
    float GetCombatAttribute(FName AttributeName, float DefaultValue = 0.0) const
    {
        UAngelscriptAbilitySystemComponent ASC = Cast<UAngelscriptAbilitySystemComponent>(GetAbilitySystemComponentFromActorInfo());
        if (ASC != nullptr)
            return ASC.GetAttributeCurrentValue(URogueCombatSet, AttributeName, DefaultValue);
        return DefaultValue;
    }
}
