// AbilityComponent.as
// Base ability: cooldown + the input -> server effect -> cosmetic broadcast flow
// (D-0006 custom component; MVP arch §5.1). Concrete abilities override ExecuteOnServer
// (authoritative effect) and PlayCosmetics (runs on everyone).
//
// NOTE (CODING_STANDARDS §1): the Hazelight fork shares one global script scope, so no
// `import` is needed to reach sibling classes / C++ types. If the editor's compiler asks
// for an import, use "Add Import To" (Shift+Alt+I).
class UAbilityComponent : UActorComponent
{
    // Replicate so a remote client's Server_Activate routes (host works regardless).
    default bReplicates = true;

    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    float Cooldown = 6.0;

    float CooldownRemaining = 0.0;

    // Overriding the Tick event is what makes this component tick in the AngelScript fork
    // (mirrors Blueprint) — there is no settable `default PrimaryComponentTick.bCanEverTick`.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (CooldownRemaining > 0.0)
            CooldownRemaining = Math::Max(0.0, CooldownRemaining - DeltaSeconds);
    }

    bool IsReady() const { return CooldownRemaining <= 0.0; }

    // Called on the OWNING CLIENT when the player presses the ability key.
    UFUNCTION(BlueprintCallable)
    void TryActivate()
    {
        if (!IsReady())
            return;

        CooldownRemaining = GetEffectiveCooldown();   // optimistic local cooldown (after CDR)
        Server_Activate(GetActivationLocation());
    }

    // Where the ability lands. Override in payoff abilities to use the aim reticle.
    FVector GetActivationLocation() const
    {
        return GetOwner().GetActorLocation();
    }

    // Cooldown after the owner's CooldownReduction stat (UStatsComponent), if present.
    float GetEffectiveCooldown() const
    {
        UStatsComponent Stats = UStatsComponent::Get(GetOwner());
        if (Stats != nullptr)
            return Stats.GetEffectiveCooldown(Cooldown);
        return Cooldown;
    }

    // ---- Override points for concrete abilities ----
    void ExecuteOnServer(FVector Location) {}    // authoritative gameplay effect (server only)
    void PlayCosmetics(FVector Location) {}      // VFX/SFX on every client

    // ---- Netcode: input -> server effect -> cosmetic broadcast ----
    // Reliable by default (CODING_STANDARDS §4.1): correct for the gameplay request.
    UFUNCTION(Server)
    void Server_Activate(FVector Location)
    {
        // Server re-checks readiness so a client can't spam past the cooldown.
        ExecuteOnServer(Location);
        Multicast_Cosmetics(Location);
    }

    // Cosmetic only -> Unreliable so it can't flood the reliable channel (CODING_STANDARDS §4.1).
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_Cosmetics(FVector Location)
    {
        PlayCosmetics(Location);
    }
}
