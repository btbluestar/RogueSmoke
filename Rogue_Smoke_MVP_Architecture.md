# Rogue Smoke — MVP Architecture & Synergy Sketch

> **Status:** v0.1 — companion to `Rogue_Smoke_GDD.md`
> **Scope:** the first-playable vertical slice (GDD §12) on **UE 5.7 + Hazelight AngelScript fork** (GDD §11.3).
> **Read this with:** GDD §5 (synergies), §7 (enemies), §11 (technical).

> ⚠️ **Verify against your fork before trusting signatures.** AngelScript-for-Unreal mirrors the Blueprint/C++ API, but exact helper names (`System::`, `Gameplay::`, `Math::`), subsystem/component `::Get` accessors, and Mass APIs **vary by engine/fork version**. Treat the code below as correct *patterns*; the running editor + compiler are the source of truth. Build a one-line "AngelScript OK" test actor first to confirm the pipeline before writing gameplay.

> 🔄 **The ability/stats/upgrade layer below is SUPERSEDED.** The custom `UAbilityComponent` /
> `UStatsComponent` / `UUpgradeEffect` sketches in §5–7 were replaced by **GAS** (Lyra-style) — see
> **DECISIONS.md D-0013**. Real code: GAS abilities in `Script/AbilitySystem/Abilities/`
> (`UGA_Taunt`/`UGA_Barrage` over `UAngelscriptGASAbility`), data-driven `URogueAbilitySet` /
> `URogueInputConfig`, C++ attribute sets + PlayerState/HeroBase in `Source/RogueSmoke/AbilitySystem/`,
> and `URogueUpgradeDef` (a GameplayEffect). The **seam** (`UCombatSubsystem`) and the synergy design
> (taunt → cluster → barrage) are unchanged; only the ability *plumbing* moved to GAS.

---

## 1. The core principle

**Script the decisions, compile the simulation.** (GDD §11.3)

- **AngelScript** owns anything you'll iterate on constantly: abilities, synergies, upgrade effects, objective/extraction flow, UI. Hot-reload during PIE is the whole point — you tune the fun without recompiling C++.
- **C++ / Mass** owns the hot path: enemy simulation at high counts, the authoritative combat queries, replicated health, spawning/pooling. These are touched rarely but run thousands of times per frame.
- The two layers meet at **one deliberately small seam: `UCombatSubsystem`** (C++). AngelScript abilities never iterate enemies themselves — they ask the subsystem. This keeps ability code legible *and* keeps the per-enemy work in compiled code that can dispatch to Mass.

---

## 2. Folder structure (MVP)

```
RogueSmoke/
├─ RogueSmoke.uproject
├─ Source/
│  └─ RogueSmoke/                     # C++ module — hot path & seams
│     ├─ RogueSmoke.Build.cs          # deps: Core, NetCore, MassEntity, MassActors, MassMovement…
│     ├─ Combat/
│     │  ├─ CombatSubsystem.h/.cpp     # THE SEAM: query + radial damage + pull/mark (authoritative)
│     │  └─ HealthComponent.h/.cpp     # replicated health for elite actors
│     ├─ Enemies/
│     │  └─ EliteEnemyBase.h/.cpp      # AActor base for elites/bosses (extended in script)
│     └─ Mass/
│        ├─ SwarmFragments.h           # FMassFragment data for fodder
│        ├─ SwarmTrait.h/.cpp          # FMassEntityTraitBase composing the fodder archetype
│        └─ SwarmProcessors.h/.cpp     # movement / pull / cluster-tag / apply-damage processors
├─ Script/                            # AngelScript — gameplay & iteration surface
│  ├─ Player/
│  │  ├─ HeroCharacter.as              # base: third-person shooter camera + input wiring
│  │  ├─ Vanguard.as                   # SETUP kit (taunt)
│  │  └─ Bombardier.as                 # PAYOFF kit (barrage)
│  ├─ Abilities/
│  │  ├─ AbilityComponent.as           # base: cooldown + server/multicast plumbing
│  │  ├─ TauntAbility.as               # SETUP: pull enemies + mark "Clustered"
│  │  └─ BarrageAbility.as             # PAYOFF: radial damage with cluster bonus
│  ├─ Enemies/
│  │  └─ ClusterableElite.as           # extends EliteEnemyBase; the "worth-taunting" unit
│  ├─ Upgrades/
│  │  ├─ UpgradeEffect.as              # base
│  │  └─ Upgrade_ChainDetonation.as    # a SYNERGY upgrade (deepens taunt→barrage)
│  ├─ Objective/
│  │  └─ RaidObjective.as              # one raid goal + basic extraction
│  └─ UI/
│     └─ UpgradeSelectWidget.as        # choose-1-of-N upgrade screen
└─ Content/                           # thin blueprints, VFX/SFX, maps, data assets
```

**Why this split:** fodder swarms run as **Mass** agents (cheap, data-oriented), elites/bosses run as **full Actors** (rich behavior, scriptable). The barrage must damage *both* representations — which is exactly why a unified C++ `UCombatSubsystem` exists rather than letting each ability query the world directly.

---

## 3. The seam: `UCombatSubsystem` (C++)

This is the only combat API AngelScript needs to know. It hides whether a target is a Mass agent or an Actor, and it is **authoritative** (mutating calls only do work on the server).

```cpp
// Source/RogueSmoke/Combat/CombatSubsystem.h
UCLASS()
class ROGUESMOKE_API UCombatSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    // AngelScript-friendly accessor: UCombatSubsystem::Get(this)
    UFUNCTION(BlueprintCallable, Category="Combat", meta=(WorldContext="WorldContext"))
    static UCombatSubsystem* Get(const UObject* WorldContext);

    // --- Queries (safe on any machine) ---
    UFUNCTION(BlueprintCallable, Category="Combat")
    int32 CountEnemiesInSphere(FVector Center, float Radius) const;

    // --- Setup operations (server-authoritative) ---
    // Nudge enemy movement toward Center for Duration. Works on Mass agents
    // (via a steer-target fragment) and on elite Actors (via their movement).
    UFUNCTION(BlueprintCallable, Category="Combat")
    void PullEnemiesToward(FVector Center, float Radius, float Strength, float Duration);

    // Flag enemies in radius as "Clustered" for Duration — the synergy condition
    // that payoff abilities read.
    UFUNCTION(BlueprintCallable, Category="Combat")
    void MarkClustered(FVector Center, float Radius, float Duration);

    // --- Payoff operation (server-authoritative) ---
    // Radial damage to every enemy representation in range. Currently-Clustered
    // enemies take BaseDamage * ClusterBonusMultiplier. Returns total dealt.
    UFUNCTION(BlueprintCallable, Category="Combat")
    float ApplyRadialDamage(FVector Center, float Radius, float BaseDamage,
                            float ClusterBonusMultiplier, AActor* DamageInstigator);
};
```

Internally: `CountEnemiesInSphere` / `ApplyRadialDamage` run a spatial query over both the Mass entity grid and a registered set of elite Actors. `MarkClustered` writes a `FClusterTagFragment` on Mass agents and a tag on Actors; `ApplyRadialDamage` reads it. All mutation guarded by `GetWorld()->GetNetMode() != NM_Client` (authority).

---

## 4. Mass side for the swarm (skeleton)

Fodder = Mass agents, not Actors. This is what makes "a lot of enemies" (GDD §7, §11.2) tractable. Spike this early to validate the enemy-count target.

```cpp
// Source/RogueSmoke/Mass/SwarmFragments.h  (skeleton — confirm Mass API for 5.7)
USTRUCT()
struct FSwarmHealthFragment : public FMassFragment
{
    GENERATED_BODY()
    float Health = 10.f;
};

USTRUCT()
struct FClusterTagFragment : public FMassFragment
{
    GENERATED_BODY()
    float ClusteredUntilSeconds = 0.f;   // > now() == currently clustered
};

USTRUCT()
struct FPullTargetFragment : public FMassFragment
{
    GENERATED_BODY()
    FVector Target = FVector::ZeroVector;
    float   ExpiresAt = 0.f;
    float   Strength  = 0.f;
};
```

Processors you'll need for the slice:
- **Movement/steering** — default wander/approach-players behavior.
- **Pull processor** — for agents with an unexpired `FPullTargetFragment`, bias velocity toward `Target` by `Strength` (this is what `PullEnemiesToward` writes).
- **Cluster processor** (or fold into pull) — `MarkClustered` sets `ClusteredUntilSeconds`.
- **Damage application** — `ApplyRadialDamage` decrements `FSwarmHealthFragment`; agents at/below 0 get destroyed and emit a death event.

Elites skip Mass entirely — they're Actors (next section) so they can have bespoke, scriptable behavior worth taunting.

---

## 5. AngelScript: abilities

### 5.1 Ability base — cooldown + netcode plumbing

```angelscript
// Script/Abilities/AbilityComponent.as
class UAbilityComponent : UActorComponent
{
    UPROPERTY(EditDefaultsOnly, Category = "Ability")
    float Cooldown = 6.0;

    float CooldownRemaining = 0.0;

    default PrimaryComponentTick.bCanEverTick = true;

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

        CooldownRemaining = Cooldown;          // optimistic local cooldown
        Server_Activate(GetActivationLocation());
    }

    // Where the ability lands. Override in payoff abilities to use the aim reticle.
    FVector GetActivationLocation() const
    {
        return GetOwner().GetActorLocation();
    }

    // ---- Override points for concrete abilities ----
    void ExecuteOnServer(FVector Location) {}   // authoritative gameplay effect
    void PlayCosmetics(FVector Location) {}      // VFX/SFX on every client

    // ---- Netcode: input → server effect → cosmetic broadcast ----
    UFUNCTION(Server)
    void Server_Activate(FVector Location)
    {
        ExecuteOnServer(Location);
        Multicast_Cosmetics(Location);
    }

    UFUNCTION(NetMulticast)
    void Multicast_Cosmetics(FVector Location)
    {
        PlayCosmetics(Location);
    }
}
```

> **Authority note:** `Server_Activate` requires the owning actor to be net-owned by the calling client's controller. All real effects happen inside `ExecuteOnServer` (server only); clients only ever request and play cosmetics. This is the host-authoritative model from GDD §11.1.

### 5.2 Taunt — the SETUP

```angelscript
// Script/Abilities/TauntAbility.as
class UTauntAbilityComponent : UAbilityComponent
{
    default Cooldown = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float Radius = 800.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float PullStrength = 1200.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float ClusterDuration = 3.0;

    void ExecuteOnServer(FVector Location) override
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get(this);
        if (Combat == nullptr)
            return;

        // SETUP, half 1: drag nearby enemies into a tight knot around the caster.
        Combat.PullEnemiesToward(Location, Radius, PullStrength, ClusterDuration);

        // SETUP, half 2: flag them "Clustered" so payoff abilities reward density.
        Combat.MarkClustered(Location, Radius, ClusterDuration);
    }

    void PlayCosmetics(FVector Location) override
    {
        // Distinct "SETUP READY" audio/visual language — GDD §5.3 / §10.
        Print("TAUNT: enemies pulled + marked Clustered");
    }
}
```

### 5.3 Barrage — the PAYOFF

```angelscript
// Script/Abilities/BarrageAbility.as
class UBarrageAbilityComponent : UAbilityComponent
{
    default Cooldown = 6.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float Radius = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float BaseDamage = 40.0;

    // The synergy payoff: clustered enemies take this multiple of damage.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float ClusterBonusMultiplier = 2.0;

    // MVP: land on the owner. Real impl: override to return the aim reticle hit.
    void ExecuteOnServer(FVector Location) override
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get(this);
        if (Combat == nullptr)
            return;

        int32 HitCount = Combat.CountEnemiesInSphere(Location, Radius);

        // PAYOFF: density already matters (more enemies = more total damage),
        // and Clustered enemies eat ClusterBonusMultiplier x on top.
        float Dealt = Combat.ApplyRadialDamage(
            Location, Radius, BaseDamage, ClusterBonusMultiplier, GetOwner());

        Print(f"BARRAGE hit {HitCount} enemies for {Dealt} total");
    }

    void PlayCosmetics(FVector Location) override
    {
        Print("BARRAGE: AoE detonation");
    }
}
```

**The synergy, in one sentence:** Taunt writes the `Clustered` condition; Barrage reads it for bonus damage. Neither ability *requires* the other (GDD §5.3 "optional but rewarding") — barrage alone still rewards natural density via `HitCount` — but together they're dramatically stronger.

---

## 6. AngelScript: the two MVP kits

Distinct kits make the synergy legible (GDD §4). Base class holds the camera + input; subclasses pick the ability.

```angelscript
// Script/Player/HeroCharacter.as
class AHeroCharacter : ACharacter
{
    UPROPERTY(DefaultComponent)
    USpringArmComponent CameraBoom;
    default CameraBoom.TargetArmLength = 350.0;             // third-person shooter (GDD §9, D-0014)
    default CameraBoom.SocketOffset = FVector(0.0, 60.0, 60.0);  // over-the-shoulder
    default CameraBoom.bUsePawnControlRotation = true;      // aim drives the boom
    default CameraBoom.bEnableCameraLag = true;

    UPROPERTY(DefaultComponent, Attach = CameraBoom)
    UCameraComponent FollowCamera;

    // Strafe / aim-locked: the character faces the control rotation, not the move direction.
    default bUseControllerRotationYaw = true;
    // (set CharacterMovement.bOrientRotationToMovement = false in the ctor/BeginPlay)

    // Bound to an Enhanced Input action on the owning client.
    UFUNCTION()
    void OnPrimaryAbilityPressed()
    {
        ActivatePrimary();
    }

    // Overridden per kit.
    void ActivatePrimary() {}
}
```

```angelscript
// Script/Player/Vanguard.as  — the SETUP hero
class AVanguard : AHeroCharacter
{
    UPROPERTY(DefaultComponent)
    UTauntAbilityComponent Taunt;

    void ActivatePrimary() override
    {
        Taunt.TryActivate();
    }
}
```

```angelscript
// Script/Player/Bombardier.as  — the PAYOFF hero
class ABombardier : AHeroCharacter
{
    UPROPERTY(DefaultComponent)
    UBarrageAbilityComponent Barrage;

    void ActivatePrimary() override
    {
        Barrage.TryActivate();
    }
}
```

---

## 7. AngelScript: a synergy upgrade

Roguelike upgrades are a great place to *deepen* synergies mid-run (GDD §6.2). This one makes the taunt→barrage combo bigger.

```angelscript
// Script/Upgrades/UpgradeEffect.as
class UUpgradeEffect : UObject
{
    UPROPERTY(EditDefaultsOnly) FText DisplayName;
    UPROPERTY(EditDefaultsOnly) FText Description;
    UPROPERTY(EditDefaultsOnly) int  Rarity = 1;       // tiers per GDD §6.1

    // Applied when the player picks this upgrade.
    void Apply(AHeroCharacter Hero) {}
}
```

```angelscript
// Script/Upgrades/Upgrade_ChainDetonation.as
class UUpgrade_ChainDetonation : UUpgradeEffect
{
    void Apply(AHeroCharacter Hero) override
    {
        // Component ::Get accessor is generated by the fork — verify the exact form.
        UBarrageAbilityComponent Barrage = UBarrageAbilityComponent::Get(Hero);
        if (Barrage == nullptr)
            return;

        Barrage.ClusterBonusMultiplier += 1.0;   // bigger payoff on clustered groups
        Barrage.Radius += 150.0;                  // easier to catch the knot the taunt made
    }
}
```

> Promote `UUpgradeEffect` to a `UDataAsset` once the pool grows, so designers can author upgrades as assets rather than classes. For the slice, plain UObject subclasses in a pool are enough.

---

## 8. How it reads at runtime (legibility check — GDD §5.3)

1. **Vanguard** presses Taunt → `Server_Activate` → `PullEnemiesToward` + `MarkClustered`. Enemies visibly converge; a "setup ready" cue plays (`PlayCosmetics`).
2. **Bombardier** sees the knot + the cue, aims, presses Barrage → `ApplyRadialDamage` with the cluster bonus → the bunched, Clustered enemies are deleted.
3. Mid-run, the team is offered **Chain Detonation** on the upgrade screen → Barrage's window widens → the same combo now clears even more.

The legibility requirement from the GDD is satisfied by three concrete things: the **pull is visible**, the **"Clustered" state has a distinct cue**, and the **bonus is large enough to feel**. If any of those is weak in playtest, the synergy fails its pillar — fix presentation before adding content.

---

## 9. Build / bring-up checklist

- [ ] Build UE 5.7 + the AngelScript fork from source; confirm a stable 5.7 branch (GDD §11.3 action item).
- [ ] Place a trivial `ASmokeTestActor` that `Print`s on `BeginPlay` — confirm hot-reload works before any gameplay.
- [ ] Stand up `UCombatSubsystem` (C++) with stub implementations that just `GetAllActorsOfClass` elites — get the *seam* working before Mass.
- [ ] Implement Vanguard + Bombardier + the two abilities against the stub subsystem; prove the combo on Actor-only enemies.
- [ ] Spike Mass fodder; route Mass agents through the same subsystem queries. Compare Mass-vs-Actor-pool against your enemy-count target.
- [ ] Add `Upgrade_ChainDetonation` + a minimal `UpgradeSelectWidget`.
- [ ] Wrap with the raid objective + basic extraction (`RaidObjective.as`), then host-authoritative 2-player test.

**Success criterion (unchanged from GDD §12):** is the taunt→barrage loop *fun* with this much and nothing more? If yes, expand. If no, stop and fix the feel.
