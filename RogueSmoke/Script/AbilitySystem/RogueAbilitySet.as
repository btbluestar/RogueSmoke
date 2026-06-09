// RogueAbilitySet.as
// Data-driven bundle of abilities + effects + attribute sets to grant a hero's ASC. Direct
// AngelScript port of Lyra's ULyraAbilitySet (F:\UEAS\...\AbilitySystem\LyraAbilitySet.h) —
// per project direction, copy Lyra instead of inventing a granting system.
//
// A DA_<Hero>_AbilitySet asset is authored per hero (Vanguard/Bombardier) and granted server-side
// on possession. Input-bound abilities carry an InputTag the controller maps Enhanced Input to.

struct FRogueAbilitySet_GameplayAbility
{
    UPROPERTY(EditDefaultsOnly)
    TSubclassOf<UGameplayAbility> Ability;

    UPROPERTY(EditDefaultsOnly)
    int Level = 1;

    // Input tag that activates this ability (matched against the hero's InputConfig). Empty = passive.
    UPROPERTY(EditDefaultsOnly, meta = (Categories = "InputTag"))
    FGameplayTag InputTag;
}

struct FRogueAbilitySet_GameplayEffect
{
    UPROPERTY(EditDefaultsOnly)
    TSubclassOf<UGameplayEffect> GameplayEffect;

    UPROPERTY(EditDefaultsOnly)
    float Level = 1.0;
}

struct FRogueAbilitySet_AttributeSet
{
    UPROPERTY(EditDefaultsOnly)
    TSubclassOf<UAngelscriptAttributeSet> AttributeSet;
}

// One granted, input-bound ability: returned so the caller can activate it by tag on input.
struct FRogueGrantedAbility
{
    FGameplayTag InputTag;
    FGameplayAbilitySpecHandle Handle;
}

class URogueAbilitySet : UPrimaryDataAsset
{
    UPROPERTY(EditDefaultsOnly, Category = "Attribute Sets")
    TArray<FRogueAbilitySet_AttributeSet> GrantedAttributes;

    UPROPERTY(EditDefaultsOnly, Category = "Gameplay Effects")
    TArray<FRogueAbilitySet_GameplayEffect> GrantedEffects;

    UPROPERTY(EditDefaultsOnly, Category = "Gameplay Abilities")
    TArray<FRogueAbilitySet_GameplayAbility> GrantedAbilities;

    // Server-only. Registers attribute sets, applies effects, grants abilities. Input-bound
    // abilities are appended to OutGranted (InputTag -> spec handle) for the caller to bind input.
    void GiveToAbilitySystem(UAngelscriptAbilitySystemComponent ASC, TArray<FRogueGrantedAbility>& OutGranted)
    {
        if (ASC == nullptr)
            return;

        // 1) Attribute sets (RegisterAttributeSet guards against double-adds).
        for (const FRogueAbilitySet_AttributeSet& Entry : GrantedAttributes)
        {
            if (Entry.AttributeSet.Get() != nullptr)
                ASC.RegisterAttributeSet(Entry.AttributeSet);
        }

        // 2) Gameplay effects (e.g. initial stat block).
        for (const FRogueAbilitySet_GameplayEffect& Entry : GrantedEffects)
        {
            if (Entry.GameplayEffect.Get() != nullptr)
                ASC.ApplyGameplayEffectToTarget(Entry.GameplayEffect, ASC, Entry.Level, FGameplayEffectContextHandle());
        }

        // 3) Abilities.
        for (const FRogueAbilitySet_GameplayAbility& Entry : GrantedAbilities)
        {
            if (Entry.Ability.Get() == nullptr)
                continue;

            FGameplayAbilitySpecHandle Handle = ASC.GiveAbility(Entry.Ability, Entry.Level, -1, this);

            if (Entry.InputTag.IsValid())
            {
                FRogueGrantedAbility Granted;
                Granted.InputTag = Entry.InputTag;
                Granted.Handle = Handle;
                OutGranted.Add(Granted);
            }
        }
    }
}
