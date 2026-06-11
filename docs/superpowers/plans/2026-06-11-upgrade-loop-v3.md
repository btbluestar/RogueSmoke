# Upgrade Loop v3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Behavior evolutions (chain ignites, poison death-burst, cluster bonus arcs, clustered-kill shields), full Vanguard/Bombardier ability card tracks (stats → milestone → evolution: Event Horizon vortex, Carpet Bombing), and a team-level-scaled wave director.

**Architecture:** Hybrid execution per spec `docs/superpowers/specs/2026-06-11-upgrade-loop-v3-design.md` — hit-path behaviors are compiled branches in `UCombatSubsystem::ProcOnHitEffects` switched by new `URogueCombatSet` attributes; death-path behaviors run in AngelScript in `ARaidGameMode.HandleEnemyKilled` (verified: `USpawnDirector::HandleEliteDeath` broadcasts `OnEnemyKilled` BEFORE `Deactivate()`, so the corpse's DoT/Clustered state is readable); ability behaviors are timers inside `GA_Taunt`/`GA_Barrage`. Wave director is a pure AngelScript function.

**Tech Stack:** UE 5.7 AngelScript fork (`F:\UEAS`, read-only), GAS via AngelscriptGAS, headless verification via `as-helper run_code_test`, `Tools\BootLevel.ps1`, `Tools\SmokeTest.ps1`, editor-python commandlet for content.

## Hard worker constraints (read first)

- **ONE editor.** `mcp__ue-cpp__build`, `run_code_test`, commandlets, and BootLevel/SmokeTest all drive editor binaries — strictly sequential, never parallel. Kill any interactive editor before building C++ (module DLL lock).
- **Never `git add -A`.** Stage files explicitly. NEVER stage or touch `RogueSmoke/Content/Blueprints/GE_Upgrade_ChainDetonation.uasset` (pre-existing dirty file, other workstream) or `SUPERPOWERS_HANDOFF.md`.
- **`F:\UEAS` is read-only.** Never push to remote.
- Verify `.as` ONLY via `mcp__plugin_ue-as_as-helper__run_code_test` (call `set_project_root` to `C:\Users\btblu\Documents\RogueSmoke\RogueSmoke` once per session first). Same for `mcp__ue-cpp__set_project_root`.
- AngelScript gotchas: parameters are implicitly const; no ternary inside f-strings (assign to a local first); `Gameplay::GetGameState()` takes no WorldContext; subsystems use no-arg `::Get()`; RPCs default RELIABLE; `Print()` is dev-only and must not call non-const methods inside the f-string.
- Content commandlet (Task 4) requires the editor CLOSED and runs AFTER Task 1's build (the new attributes must exist in the binaries).
- File paths below are relative to the repo root `C:\Users\btblu\Documents\RogueSmoke`.

---

### Task 1: C++ pass — attributes, shot params, seam methods, objective-target setter

**Files:**
- Modify: `RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueCombatSet.h`
- Modify: `RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueCombatSet.cpp`
- Modify: `RogueSmoke/Source/RogueSmoke/Combat/CombatSubsystem.h`
- Modify: `RogueSmoke/Source/RogueSmoke/Combat/CombatSubsystem.cpp`
- Modify: `RogueSmoke/Source/RogueSmoke/Enemies/EliteEnemyBase.h`

- [ ] **Step 1.1: Add 11 attributes to `RogueCombatSet.h`**

After `ATTRIBUTE_ACCESSORS(URogueCombatSet, ReloadSpeedBonus);` (line 44) add:

```cpp
	ATTRIBUTE_ACCESSORS(URogueCombatSet, ChainIgniteFraction);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, ClusterChainBonusArcs);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, PoisonBurstDps);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, ClusterKillShieldAmount);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, TauntRadiusBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, TauntClusterDurationBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, TauntDamage);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, TauntVortex);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BarrageDamageBonus);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BarrageSalvoCount);
	ATTRIBUTE_ACCESSORS(URogueCombatSet, BarrageCarpet);
```

After the `OnRep_ReloadSpeedBonus` trampoline (line 63) add 11 trampolines in the same style:

```cpp
	UFUNCTION() void OnRep_ChainIgniteFraction(const FAngelscriptGameplayAttributeData& Old)     { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_ClusterChainBonusArcs(const FAngelscriptGameplayAttributeData& Old)   { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_PoisonBurstDps(const FAngelscriptGameplayAttributeData& Old)          { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_ClusterKillShieldAmount(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_TauntRadiusBonus(const FAngelscriptGameplayAttributeData& Old)        { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_TauntClusterDurationBonus(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_TauntDamage(const FAngelscriptGameplayAttributeData& Old)             { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_TauntVortex(const FAngelscriptGameplayAttributeData& Old)             { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageDamageBonus(const FAngelscriptGameplayAttributeData& Old)      { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageSalvoCount(const FAngelscriptGameplayAttributeData& Old)       { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_BarrageCarpet(const FAngelscriptGameplayAttributeData& Old)           { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
```

At the bottom of the private section (after `ReloadSpeedBonus`, line 118) add:

```cpp
	// --- v3 behavior evolutions + hero ability tracks (D-0020). Same contract as the weapon
	// track: all default 0 = behavior off; instant ADD_BASE GEs stack on repeat picks.
	// Flags (TauntVortex/BarrageCarpet) are "treat >= 1 as on".

	// Chain arcs also ignite: arc targets get Burn at ArcDamage * fraction / BurnDuration.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ChainIgniteFraction, Category = "Evolution", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData ChainIgniteFraction;

	// Extra chain arcs when the directly-hit victim is Clustered (Overwhelm evolution).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ClusterChainBonusArcs, Category = "Evolution", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData ClusterChainBonusArcs;

	// Enemies dying while Poisoned burst: poison DoT at this DPS in a sphere (RaidGameMode death path).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PoisonBurstDps, Category = "Evolution", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData PoisonBurstDps;

	// Flat Shield granted to the squad per Clustered kill (Iron Bulwark).
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ClusterKillShieldAmount, Category = "Evolution", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData ClusterKillShieldAmount;

	// Taunt track: additive radius (uu) / Clustered duration (s); TauntDamage > 0 makes the
	// taunt hit (Concussive Taunt); TauntVortex >= 1 turns it into a re-pulling vortex.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_TauntRadiusBonus, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData TauntRadiusBonus;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_TauntClusterDurationBonus, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData TauntClusterDurationBonus;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_TauntDamage, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData TauntDamage;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_TauntVortex, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData TauntVortex;

	// Barrage track: multiplicative damage bonus; extra echo strikes; carpet flag.
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BarrageDamageBonus, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BarrageDamageBonus;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BarrageSalvoCount, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BarrageSalvoCount;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BarrageCarpet, Category = "Ability", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData BarrageCarpet;
```

- [ ] **Step 1.2: Init + clamp in `RogueCombatSet.cpp`**

In the constructor after `InitReloadSpeedBonus(0.0f);`:

```cpp
	InitChainIgniteFraction(0.0f);
	InitClusterChainBonusArcs(0.0f);
	InitPoisonBurstDps(0.0f);
	InitClusterKillShieldAmount(0.0f);
	InitTauntRadiusBonus(0.0f);
	InitTauntClusterDurationBonus(0.0f);
	InitTauntDamage(0.0f);
	InitTauntVortex(0.0f);
	InitBarrageDamageBonus(0.0f);
	InitBarrageSalvoCount(0.0f);
	InitBarrageCarpet(0.0f);
```

In `PreAttributeChange`, extend the final non-negative clamp `else if` condition with the 11 new getters (same `FMath::Max(NewValue, 0.0f)` branch):

```cpp
	else if (Attribute == GetWeaponDamageBonusAttribute() || Attribute == GetFireRateBonusAttribute() ||
	         Attribute == GetPierceCountAttribute() || Attribute == GetChainCountAttribute() ||
	         Attribute == GetMagazineBonusAttribute() || Attribute == GetReloadSpeedBonusAttribute() ||
	         Attribute == GetChainIgniteFractionAttribute() || Attribute == GetClusterChainBonusArcsAttribute() ||
	         Attribute == GetPoisonBurstDpsAttribute() || Attribute == GetClusterKillShieldAmountAttribute() ||
	         Attribute == GetTauntRadiusBonusAttribute() || Attribute == GetTauntClusterDurationBonusAttribute() ||
	         Attribute == GetTauntDamageAttribute() || Attribute == GetTauntVortexAttribute() ||
	         Attribute == GetBarrageDamageBonusAttribute() || Attribute == GetBarrageSalvoCountAttribute() ||
	         Attribute == GetBarrageCarpetAttribute())
```

- [ ] **Step 1.3: `CombatSubsystem.h` — shot params + two seam methods**

In `FWeaponShotParams`, after `PoisonDuration` (line 101):

```cpp
	/** v3 evolutions (D-0020): > 0 means chain arcs also Burn their target at
	 *  ArcDamage * ChainIgniteFraction / BurnDuration DPS for BurnDuration (Searing Arcs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Shot")
	float ChainIgniteFraction = 0.f;

	/** Extra chain arcs when the directly-hit victim is Clustered (Overwhelm evolution). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Shot")
	int32 ClusterChainBonusArcs = 0;
```

In `UCombatSubsystem`, after `ApplyRadialDamageToPlayers` (line 184):

```cpp
	/** Apply a DoT to every LIVE registered enemy within Radius (Toxic Burst death cloud).
	 *  DoT only — no instant damage — so cascades are time-gated by dot ticks (bounded by
	 *  design, like chains). Returns how many enemies were dotted. Server-only. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	int32 ApplyDotInSphere(FVector Center, float Radius, ERogueDotType Type,
	                       float DamagePerSecond, float Duration, AActor* DamageInstigator);

	/** Grant Shield to every player pawn, clamped to each pawn's MaxShield (Iron Bulwark).
	 *  Goes through a transient instant GE so RogueHealthSet stays the single write path. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void GrantShieldToSquad(float Amount);
```

- [ ] **Step 1.4: `CombatSubsystem.cpp` — implement the two methods + evolve `ProcOnHitEffects`**

Append after `ApplyRadialDamageToPlayers`:

```cpp
int32 UCombatSubsystem::ApplyDotInSphere(FVector Center, float Radius, ERogueDotType Type,
                                         float DamagePerSecond, float Duration, AActor* DamageInstigator)
{
	if (!IsServer() || Radius <= 0.f || DamagePerSecond <= 0.f || Duration <= 0.f)
	{
		return 0;
	}

	// Snapshot for the same reason ApplyRadialDamage does: registry mutation safety.
	const TArray<TWeakObjectPtr<AEliteEnemyBase>> Snapshot = Elites;
	const float RadiusSq = Radius * Radius;
	int32 Applied = 0;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Snapshot)
	{
		AEliteEnemyBase* Elite = Ptr.Get();
		if (Elite == nullptr || Elite->Health == nullptr || Elite->Health->IsDead())
		{
			continue;   // the bursting corpse itself is dead -> skipped
		}
		if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
		{
			Elite->Health->ApplyDot(Type, DamagePerSecond, Duration, DamageInstigator);
			++Applied;
		}
	}
	return Applied;
}

void UCombatSubsystem::GrantShieldToSquad(float Amount)
{
	UWorld* World = GetWorld();
	if (!IsServer() || World == nullptr || Amount <= 0.f)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			continue;
		}
		UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn);
		if (ASC == nullptr)
		{
			continue;
		}

		// Clamp in C++ so an evolution can never overfill past MaxShield, mirroring the
		// transient-GE pattern of ApplyDamageToPlayer (no direct attribute pokes).
		const float Shield = ASC->GetNumericAttribute(URogueHealthSet::GetShieldAttribute());
		const float MaxShield = ASC->GetNumericAttribute(URogueHealthSet::GetMaxShieldAttribute());
		const float Delta = FMath::Min(Amount, FMath::Max(0.f, MaxShield - Shield));
		if (Delta <= 0.f)
		{
			continue;
		}

		UGameplayEffect* ShieldGE = NewObject<UGameplayEffect>(GetTransientPackage());
		ShieldGE->DurationPolicy = EGameplayEffectDurationType::Instant;
		FGameplayModifierInfo Mod;
		Mod.Attribute = URogueHealthSet::GetShieldAttribute();
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Delta));
		ShieldGE->Modifiers.Add(Mod);
		ASC->ApplyGameplayEffectToSelf(ShieldGE, 1.0f, ASC->MakeEffectContext());
	}
}
```

In `ProcOnHitEffects`, replace the chain section (from the `if (Params.ChainCount <= 0 ...)` gate at line 360 to the end of the function) with:

```cpp
	// Chain arcs (RoR2-ukulele shape): nearest arcs within ChainRadius of the victim take a
	// damage fraction. The Overwhelm evolution adds arcs when the victim is Clustered — the
	// taunt->gun loop. Arcs never re-chain or re-pierce; Searing Arcs may add a Burn DoT to
	// arc targets (still no recursive procs), so the cascade stays bounded.
	const int32 EffectiveChainCount = Params.ChainCount +
		(Victim->IsClustered() ? FMath::Max(0, Params.ClusterChainBonusArcs) : 0);
	if (EffectiveChainCount <= 0 || Params.ChainDamageFraction <= 0.f)
	{
		return;
	}

	const FVector Origin = Victim->GetActorLocation();
	const float RadiusSq = Params.ChainRadius * Params.ChainRadius;

	TArray<AEliteEnemyBase*> Candidates;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		AEliteEnemyBase* Other = Ptr.Get();
		if (Other == nullptr || Other == Victim || Other->Health == nullptr || Other->Health->IsDead())
		{
			continue;
		}
		if (FVector::DistSquared(Other->GetActorLocation(), Origin) <= RadiusSq)
		{
			Candidates.Add(Other);
		}
	}

	Candidates.Sort([&Origin](const AEliteEnemyBase& A, const AEliteEnemyBase& B)
	{
		return FVector::DistSquared(A.GetActorLocation(), Origin) <
		       FVector::DistSquared(B.GetActorLocation(), Origin);
	});

	const int32 Arcs = FMath::Min(EffectiveChainCount, Candidates.Num());
	const float ChainDamage = Params.Damage * Params.ChainDamageFraction;
	for (int32 i = 0; i < Arcs; ++i)
	{
		Result.DamageDealt += Candidates[i]->Health->ApplyDamage(ChainDamage, DamageInstigator);
		Result.ExtraEnemiesHit++;

		// Searing Arcs: the arc ignites its target (magnitude derives from the arc's damage,
		// same Gunfire-Reborn model as the on-hit procs above).
		if (Params.ChainIgniteFraction > 0.f && Params.BurnDuration > 0.f && !Candidates[i]->Health->IsDead())
		{
			Candidates[i]->Health->ApplyDot(ERogueDotType::Burn,
			                                ChainDamage * Params.ChainIgniteFraction / Params.BurnDuration,
			                                Params.BurnDuration, DamageInstigator);
		}

		// MVP readability: debug arc on the host. The Niagara beam lands with the cue pass (#39).
		DrawDebugLine(GetWorld(), Origin + FVector(0, 0, 50.f),
		              Candidates[i]->GetActorLocation() + FVector(0, 0, 50.f),
		              FColor::Cyan, false, 0.2f, 0, 2.f);
	}
```

- [ ] **Step 1.5: `EliteEnemyBase.h` — objective-target setter**

After `CountsAsObjectiveTarget()` (line 39):

```cpp
	/** Director-injected wave elites are pressure, not clear-gates: the raid objective flags
	 *  them false at spawn. Pooled actors keep the last value, so spawn sites set it explicitly. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void SetCountsAsObjectiveTarget(bool bCounts) { bCountsAsObjectiveTarget = bCounts; }
```

- [ ] **Step 1.6: Build**

Ensure no interactive editor is running (`mcp__ue-cpp__editor_sessions_list`, end any). Run `mcp__ue-cpp__build`. Expected: 0 errors. Fix any compile errors before proceeding.

- [ ] **Step 1.7: Commit**

```bash
git add RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueCombatSet.h RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueCombatSet.cpp RogueSmoke/Source/RogueSmoke/Combat/CombatSubsystem.h RogueSmoke/Source/RogueSmoke/Combat/CombatSubsystem.cpp RogueSmoke/Source/RogueSmoke/Enemies/EliteEnemyBase.h
git commit -m "feat(combat): v3 evolution attributes + seam methods (chain ignite, cluster arcs, dot sphere, squad shield) (D-0020)"
```

---

### Task 2: AngelScript gameplay pass — abilities, death-path evolutions, hero gating, wave director

**Files:**
- Modify: `RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as` (2 lines)
- Modify: `RogueSmoke/Script/AbilitySystem/Abilities/GA_Taunt.as` (rewrite)
- Modify: `RogueSmoke/Script/AbilitySystem/Abilities/GA_Barrage.as` (rewrite)
- Modify: `RogueSmoke/Script/Upgrades/RogueUpgradeDef.as` (1 field)
- Modify: `RogueSmoke/Script/Core/RaidGameMode.as` (death path + eligibility)
- Create: `RogueSmoke/Script/Objective/RaidWaveDirector.as`
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as` (director consumption)

- [ ] **Step 2.1: `GA_WeaponFire.as` — carry the two new shot params**

After `Shot.PoisonChance = GetCombatAttribute(n"PoisonChance");` (line 71) add:

```angelscript
        Shot.ChainIgniteFraction = GetCombatAttribute(n"ChainIgniteFraction");
        Shot.ClusterChainBonusArcs = int(GetCombatAttribute(n"ClusterChainBonusArcs"));
```

- [ ] **Step 2.2: `GA_Taunt.as` — attribute-driven tunables, Concussive damage, Event Horizon vortex**

Full replacement file content:

```angelscript
// GA_Taunt.as
// SETUP half of the signature synergy (D-0008), now a GAS ability. Pulls nearby enemies into a
// knot around the caster and flags them "Clustered" so payoff abilities reward density.
// v3 (D-0020): radius/duration grow via URogueCombatSet attributes; TauntDamage makes the taunt
// hit (Concussive Taunt); TauntVortex turns it into a re-pulling vortex (Event Horizon).
class UGA_Taunt : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float Radius = 800.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float PullStrength = 1200.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float ClusterDuration = 3.0;

    // Event Horizon: re-pull + refresh Clustered every VortexInterval for VortexDuration.
    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexDuration = 3.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexInterval = 0.5;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexStrengthFraction = 0.6;

    private FVector VortexCenter;
    private int VortexPulsesRemaining = 0;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        // Commit pays cost + starts cooldown (via the assigned Cooldown GE). Bail if it fails.
        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        // Server-authoritative seam work only (clients predict the cosmetic, server does the pull).
        if (HasAuthority())
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
            {
                FVector Location = GetActivationLocation();
                TauntPulse(Combat, Location, PullStrength);

                // Concussive Taunt: the taunt now hits. AbilityPower-scaled; cluster multiplier
                // 1.0 so it never double-dips with Barrage's cluster payoff.
                float TauntDmg = GetCombatAttribute(n"TauntDamage");
                if (TauntDmg > 0.0)
                {
                    Combat.ApplyRadialDamage(Location, EffectiveRadius(),
                        TauntDmg * GetCombatAttribute(n"AbilityPower", 1.0), 1.0,
                        GetAvatarActorFromActorInfo());
                }

                // Event Horizon: keep pulsing on a timer; EndAbility deferred to the last pulse.
                if (GetCombatAttribute(n"TauntVortex") >= 1.0)
                {
                    VortexCenter = Location;
                    VortexPulsesRemaining = Math::Max(int(VortexDuration / VortexInterval), 1);
                    System::SetTimer(this, n"VortexPulse", VortexInterval, true);
                    Print("TAUNT: EVENT HORIZON — vortex active", 2.0);
                    return;
                }
            }
        }

        // Cosmetic cue (replace Print with a GameplayCue asset). Predicted on the local client.
        Print("TAUNT: enemies pulled + marked Clustered", 2.0);

        EndAbility();
    }

    private float EffectiveRadius() const
    {
        return Radius + GetCombatAttribute(n"TauntRadiusBonus");
    }

    private float EffectiveClusterDuration() const
    {
        return ClusterDuration + GetCombatAttribute(n"TauntClusterDurationBonus");
    }

    private void TauntPulse(UCombatSubsystem Combat, FVector Location, float Strength)
    {
        Combat.PullEnemiesToward(Location, EffectiveRadius(), Strength, EffectiveClusterDuration());  // SETUP, half 1
        Combat.MarkClustered(Location, EffectiveRadius(), EffectiveClusterDuration());                // SETUP, half 2
    }

    UFUNCTION()
    private void VortexPulse()
    {
        VortexPulsesRemaining -= 1;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
            TauntPulse(Combat, VortexCenter, PullStrength * VortexStrengthFraction);
        if (VortexPulsesRemaining <= 0)
        {
            System::ClearTimer(this, n"VortexPulse");
            EndAbility();
        }
    }
}
```

- [ ] **Step 2.3: `GA_Barrage.as` — damage bonus, Twin Salvo echoes, Carpet Bombing strip**

Full replacement file content:

```angelscript
// GA_Barrage.as
// PAYOFF half of the signature synergy (D-0008), now a GAS ability. Radial damage that rewards
// density: Clustered enemies (set by Taunt) eat a bonus multiplier. v3 (D-0020): damage scales
// via BarrageDamageBonus; BarrageSalvoCount adds echo strikes (Twin Salvo); BarrageCarpet turns
// the strike into a telegraphed strip marching along the caster's facing (Carpet Bombing).
class UGA_Barrage : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float Radius = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float BaseDamage = 40.0;

    // The synergy payoff: clustered enemies take this multiple of damage.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float ClusterBonusMultiplier = 2.0;

    // Twin Salvo: each extra strike echoes at the same center for a damage fraction.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Salvo")
    float SalvoDelay = 0.4;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Salvo")
    float EchoDamageFraction = 0.6;

    // Carpet Bombing: pads march along the caster's facing, each telegraphed before impact.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    int CarpetSteps = 5;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetStepDistance = 300.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetStepInterval = 0.25;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetDamageFraction = 0.6;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetRadiusFraction = 0.6;

    // Telegraph lead in step-timer ticks: rings and blasts share one clock (GDD §10 readability).
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    int CarpetTelegraphLeadTicks = 2;

    private FVector StrikeCenter;
    private FVector CarpetDir;
    private int SalvoStrikesRemaining = 0;
    private int CarpetTick = 0;
    private float EffRadius = 0.0;
    private float EffDamage = 0.0;
    private float EffCluster = 0.0;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        if (HasAuthority())
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
            {
                // Upgrades add to these via URogueCombatSet (Chain Detonation, High Explosives...).
                StrikeCenter = GetActivationLocation();
                EffRadius = Radius + GetCombatAttribute(n"BarrageRadiusBonus");
                EffDamage = BaseDamage * (1.0 + GetCombatAttribute(n"BarrageDamageBonus"));
                EffCluster = ClusterBonusMultiplier + GetCombatAttribute(n"BarrageClusterBonus");

                // Carpet Bombing replaces the single strike (and any salvo) with the strip.
                if (GetCombatAttribute(n"BarrageCarpet") >= 1.0)
                {
                    AActor Avatar = GetAvatarActorFromActorInfo();
                    CarpetDir = Avatar != nullptr ? Avatar.GetActorForwardVector() : FVector(1.0, 0.0, 0.0);
                    CarpetDir.Z = 0.0;
                    CarpetDir = CarpetDir.GetSafeNormal();
                    CarpetTick = 0;
                    System::SetTimer(this, n"CarpetStep", CarpetStepInterval, true);
                    Print("BARRAGE: CARPET BOMBING — strip inbound", 2.0);
                    return;   // EndAbility deferred to the last pad
                }

                Strike(Combat, StrikeCenter, EffRadius, EffDamage);

                SalvoStrikesRemaining = int(GetCombatAttribute(n"BarrageSalvoCount"));
                if (SalvoStrikesRemaining > 0)
                {
                    System::SetTimer(this, n"SalvoStrike", SalvoDelay, true);
                    return;   // EndAbility deferred to the last echo
                }
            }
        }

        EndAbility();
    }

    private void Strike(UCombatSubsystem Combat, FVector Center, float StrikeRadius, float Damage)
    {
        int HitCount = Combat.CountEnemiesInSphere(Center, StrikeRadius);
        float Dealt = Combat.ApplyRadialDamage(Center, StrikeRadius, Damage, EffCluster,
                                               GetAvatarActorFromActorInfo());
        Print(f"BARRAGE hit {HitCount} enemies for {Dealt} total", 2.0);
    }

    UFUNCTION()
    private void SalvoStrike()
    {
        SalvoStrikesRemaining -= 1;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
            Strike(Combat, StrikeCenter, EffRadius, EffDamage * EchoDamageFraction);
        if (SalvoStrikesRemaining <= 0)
        {
            System::ClearTimer(this, n"SalvoStrike");
            EndAbility();
        }
    }

    // One clock: tick T telegraphs pad T and detonates pad T - CarpetTelegraphLeadTicks, so the
    // ring's fill reaches the edge exactly at impact (the telegraph contract, GDD §10).
    UFUNCTION()
    private void CarpetStep()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
        {
            System::ClearTimer(this, n"CarpetStep");
            EndAbility();
            return;
        }

        float PadRadius = EffRadius * CarpetRadiusFraction;
        if (CarpetTick < CarpetSteps)
            Combat.ShowTelegraphZone(PadCenter(CarpetTick), PadRadius, CarpetStepInterval * float(CarpetTelegraphLeadTicks));

        int BlastIdx = CarpetTick - CarpetTelegraphLeadTicks;
        if (BlastIdx >= 0 && BlastIdx < CarpetSteps)
            Strike(Combat, PadCenter(BlastIdx), PadRadius, EffDamage * CarpetDamageFraction);

        CarpetTick += 1;
        if (CarpetTick >= CarpetSteps + CarpetTelegraphLeadTicks)
        {
            System::ClearTimer(this, n"CarpetStep");
            EndAbility();
        }
    }

    private FVector PadCenter(int StepIdx) const
    {
        return StrikeCenter + CarpetDir * CarpetStepDistance * float(StepIdx + 1);
    }
}
```

- [ ] **Step 2.4: `RogueUpgradeDef.as` — hero gating field**

After `bool bSynergyUpgrade = false;` (line 33) add:

```angelscript
    // Only offer to players piloting this hero class (null = any hero). Checked in
    // ARaidGameMode::IsEligible; a dead/missing pawn counts as ineligible (safe default).
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    TSubclassOf<APawn> RequiredHeroClass;
```

- [ ] **Step 2.5: `RaidGameMode.as` — hero gate in `IsEligible` + death-path evolutions**

In `IsEligible`, after the MaxStacks check (line 291-292) insert:

```angelscript
        // Hero-gated cards (taunt/barrage tracks) never appear in the wrong hero's hand.
        if (Def.RequiredHeroClass.Get() != nullptr)
        {
            APawn HeroPawn = ForPlayer != nullptr ? ForPlayer.GetPawn() : nullptr;
            if (HeroPawn == nullptr || !HeroPawn.IsA(Def.RequiredHeroClass.Get()))
                return false;
        }
```

(If `APlayerState.GetPawn()` is not bound, check with `mcp__plugin_ue-as_as-helper__find_binding` for `GetPawn` on `APlayerState`; the fork binds it as `GetPawn()`.)

Add two tunables next to the XP tunables (after `XPGrowthPerLevel`, line 192):

```angelscript
    // --- v3 death-path evolutions (D-0020): Toxic Burst cloud + Iron Bulwark shield. ---
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|Evolutions")
    float PoisonBurstRadius = 350.0;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|Evolutions")
    float PoisonBurstDuration = 6.0;
```

Add this private helper after `GetPlayerSalt` (line 598):

```angelscript
    // Highest value across hero ASCs. Evolution GEs are bApplyToSquad, so any living hero
    // carries the value; max() behaves sanely if a hero spawned after the pick. ~2 heroes,
    // so the GetAllActorsOfClass per kill is cheap; cache if fodder rates ever make it hot.
    private float GetSquadAttribute(FName AttrName) const
    {
        float Best = 0.0;
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter Hero : Heroes)
        {
            if (Hero == nullptr)
                continue;
            UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
            if (ASC != nullptr)
                Best = Math::Max(Best, ASC.GetAttributeCurrentValue(URogueCombatSet, AttrName, 0.0));
        }
        return Best;
    }
```

In `HandleEnemyKilled`, after the phase guard (line 339) and BEFORE the XP block, insert:

```angelscript
        // v3 behavior evolutions read the corpse's pre-recycle state — OnEnemyKilled fires
        // before Deactivate/ResetHealth (SpawnDirector::HandleEliteDeath), so DoT/Clustered
        // flags are still live here.
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
        {
            float BurstDps = GetSquadAttribute(n"PoisonBurstDps");
            if (BurstDps > 0.0 && Enemy.Health != nullptr
                && Enemy.Health.HasActiveDot(ERogueDotType::Poison))
            {
                int Spread = Combat.ApplyDotInSphere(Enemy.GetActorLocation(), PoisonBurstRadius,
                    ERogueDotType::Poison, BurstDps, PoisonBurstDuration, nullptr);
                if (Spread > 0)
                    Print(f"[Evo] toxic burst -> {Spread} enemies", 2.0);
            }

            float ShieldAmount = GetSquadAttribute(n"ClusterKillShieldAmount");
            if (ShieldAmount > 0.0 && Enemy.IsClustered())
                Combat.GrantShieldToSquad(ShieldAmount);
        }
```

- [ ] **Step 2.6: Create `RogueSmoke/Script/Objective/RaidWaveDirector.as`**

```angelscript
// RaidWaveDirector.as
// The wave director's brain (D-0020): a PURE function of (team level, wave index, player count,
// tunables) — no world reads, no RNG, no clocks — so the same inputs give the same plan on every
// machine and in the DirectorReport battery. ARaidObjective owns the tunables and consumes plans.

struct FDirectorTunables
{
    UPROPERTY()
    float BaseInterval = 7.0;

    UPROPERTY()
    int BasePerWave = 8;

    UPROPERTY()
    float EscalationPerWave = 0.5;

    UPROPERTY()
    int MaxPerWave = 32;

    // v3 scaling: pressure follows the team's power curve (D-0018 levels).
    UPROPERTY()
    float FodderPerTeamLevel = 0.8;

    UPROPERTY()
    float IntervalReductionPerLevel = 0.35;

    UPROPERTY()
    float MinInterval = 3.5;

    UPROPERTY()
    int EliteInjectStartLevel = 4;

    UPROPERTY()
    int EliteInjectFastLevel = 8;

    UPROPERTY()
    float PlayerCountWaveScale = 0.5;
}

struct FWavePlan
{
    UPROPERTY()
    int FodderCount = 0;

    UPROPERTY()
    float Interval = 7.0;

    // Index into the objective's InjectRoster (rotated deterministically); -1 = no injection.
    UPROPERTY()
    int EliteInjectIndex = -1;
}

namespace RaidDirector
{
    FWavePlan ComputeWavePlan(int TeamLevel, int WaveIndex, int NumPlayers, const FDirectorTunables& T)
    {
        FWavePlan Plan;

        float Size = float(T.BasePerWave)
                   + float(WaveIndex) * T.EscalationPerWave
                   + float(TeamLevel) * T.FodderPerTeamLevel;
        Size *= 1.0 + T.PlayerCountWaveScale * float(Math::Max(NumPlayers - 1, 0));
        Plan.FodderCount = Math::Clamp(int(Size), 1, T.MaxPerWave);

        Plan.Interval = Math::Max(T.MinInterval,
                                  T.BaseInterval - float(TeamLevel) * T.IntervalReductionPerLevel);

        // Spice, not just volume: from the start level every 3rd wave carries an elite, every
        // 2nd from the fast level. Keyed to WaveIndex only — deterministic, no RNG to perturb.
        if (TeamLevel >= T.EliteInjectStartLevel)
        {
            int Cadence = (TeamLevel >= T.EliteInjectFastLevel) ? 2 : 3;
            if (WaveIndex % Cadence == 0)
                Plan.EliteInjectIndex = (WaveIndex / Cadence) % 2;
        }
        return Plan;
    }
}
```

- [ ] **Step 2.7: `RaidObjective.as` — consume the plan**

Change `FodderMaxPerWave` default from `20` to `32` (line 85). After `MaxConcurrentEnemies` (line 93) add:

```angelscript
    // --- v3 wave director (D-0020): pressure scales with team level + squad size. ---
    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float FodderPerTeamLevel = 0.8;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float WaveIntervalReductionPerLevel = 0.35;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float MinWaveInterval = 3.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int EliteInjectStartLevel = 4;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int EliteInjectFastLevel = 8;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float PlayerCountWaveScale = 0.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int MaxConcurrentPerExtraPlayer = 5;

    // Injected wave elites rotate through this roster (defaulted in BeginPlay). They are
    // pressure, NOT clear-gates: SetCountsAsObjectiveTarget(false) at spawn.
    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    TArray<TSubclassOf<AEliteEnemyBase>> InjectRoster;
```

In `BeginPlay`, after the `BossClass` default (line 130) add:

```angelscript
        if (InjectRoster.Num() == 0)
        {
            InjectRoster.Add(ASpitter);
            InjectRoster.Add(ALunger);
        }
```

Replace `TickFodderWaves` (lines 161-182) with:

```angelscript
    // Spawn a director-planned fodder wave; the plan scales with team level, wave index, and
    // squad size (RaidWaveDirector.as), soft-capped by live enemy count.
    private void TickFodderWaves(float DeltaSeconds)
    {
        if (!bSpawnFodderWaves || Elapsed < StartGraceSeconds)
            return;

        int TeamLevel = GetTeamLevel();
        int NumPlayers = GetNumPlayers();
        FWavePlan Plan = RaidDirector::ComputeWavePlan(TeamLevel, WaveIndex, NumPlayers, MakeTunables());

        WaveTimer += DeltaSeconds;
        if (WaveTimer < Plan.Interval)
            return;
        WaveTimer = 0.0;

        int ConcurrentCap = MaxConcurrentEnemies + MaxConcurrentPerExtraPlayer * Math::Max(NumPlayers - 1, 0);
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr && Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) >= ConcurrentCap)
            return;     // already enough on the field

        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr)
            return;

        FVector Center = PickWaveCenter(WaveIndex);
        Director.SpawnFodderWave(Center, 300.0, Plan.FodderCount);

        if (Plan.EliteInjectIndex >= 0 && InjectRoster.Num() > 0)
        {
            TSubclassOf<AEliteEnemyBase> Cls = InjectRoster[Plan.EliteInjectIndex % InjectRoster.Num()];
            if (Cls.Get() != nullptr)
            {
                AEliteEnemyBase Injected = Director.SpawnElite(Cls, Center + FVector(0.0, 0.0, 40.0), FRotator());
                if (Injected != nullptr)
                {
                    Injected.SetCountsAsObjectiveTarget(false);   // pressure, not a clear-gate
                    Print(f"[Director] wave {WaveIndex}: injected elite (L{TeamLevel})", 3.0);
                }
            }
        }
        WaveIndex += 1;
    }

    private FDirectorTunables MakeTunables() const
    {
        FDirectorTunables T;
        T.BaseInterval = FodderWaveInterval;
        T.BasePerWave = FodderPerWave;
        T.EscalationPerWave = FodderEscalationPerWave;
        T.MaxPerWave = FodderMaxPerWave;
        T.FodderPerTeamLevel = FodderPerTeamLevel;
        T.IntervalReductionPerLevel = WaveIntervalReductionPerLevel;
        T.MinInterval = MinWaveInterval;
        T.EliteInjectStartLevel = EliteInjectStartLevel;
        T.EliteInjectFastLevel = EliteInjectFastLevel;
        T.PlayerCountWaveScale = PlayerCountWaveScale;
        return T;
    }

    private int GetTeamLevel() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        return GameState != nullptr ? GameState.TeamLevel : 1;
    }

    private int GetNumPlayers() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        return GameState != nullptr ? Math::Max(GameState.PlayerArray.Num(), 1) : 1;
    }
```

In `SpawnInitialElites`, capture spawn returns and pin the clear-gate flag (pooled actors keep
the last value, so set it explicitly — an injected Spitter recycled into a ring elite must gate):

```angelscript
        if (BossClass.Get() != nullptr)
        {
            AEliteEnemyBase Boss = Director.SpawnElite(BossClass, Center, FRotator());
            if (Boss != nullptr)
                Boss.SetCountsAsObjectiveTarget(true);
            bBoss = true;
        }
```

and in the ring loop:

```angelscript
                AEliteEnemyBase Ring = Director.SpawnElite(Cls, Center + Offset, FRotator());
                if (Ring != nullptr)
                    Ring.SetCountsAsObjectiveTarget(true);
                Spawned += 1;
```

- [ ] **Step 2.8: Verify + commit**

Run `mcp__plugin_ue-as_as-helper__run_code_test`. Expected: 0 errors, 0 warnings.

```bash
git add RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as RogueSmoke/Script/AbilitySystem/Abilities/GA_Taunt.as RogueSmoke/Script/AbilitySystem/Abilities/GA_Barrage.as RogueSmoke/Script/Upgrades/RogueUpgradeDef.as RogueSmoke/Script/Core/RaidGameMode.as RogueSmoke/Script/Objective/RaidWaveDirector.as RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(upgrades): v3 gameplay - ability evolutions, death-path evolutions, hero gating, wave director (D-0020)"
```

---

### Task 3: Exec batteries — EvoSmoke, DirectorReport, FlowSmoke check 7, UpgradeSmoke tracking

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as`

- [ ] **Step 3.1: Extend `GetTrackedAttributes`** (line 668) — after the `ReloadSpeedBonus` row add:

```angelscript
        OutNames.Add(n"ChainIgniteFraction");      OutIsHealthSet.Add(false);
        OutNames.Add(n"ClusterChainBonusArcs");    OutIsHealthSet.Add(false);
        OutNames.Add(n"PoisonBurstDps");           OutIsHealthSet.Add(false);
        OutNames.Add(n"ClusterKillShieldAmount");  OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntRadiusBonus");         OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntClusterDurationBonus");OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntDamage");              OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntVortex");              OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageDamageBonus");       OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageSalvoCount");        OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageCarpet");            OutIsHealthSet.Add(false);
```

- [ ] **Step 3.2: FlowSmoke check 7 — hero gating**

Read `UpgradeFlowSmoke` (line 840) and follow its existing per-check report pattern exactly.
Append a 7th check before the RESULT print and bump every `/6` total to `/7`:

```angelscript
        // Check 7: hero gating — RequiredHeroClass must match the candidate player's pawn.
        // Host pawn is a hero, so an AHeroCharacter gate passes and a spectator gate fails.
        URogueUpgradeDef GatedOk = NewObject(this, URogueUpgradeDef);
        GatedOk.RequiredHeroClass = AHeroCharacter;
        URogueUpgradeDef GatedWrong = NewObject(this, URogueUpgradeDef);
        GatedWrong.RequiredHeroClass = ASpectatorPawn;
        bool bCheck7 = GameMode.IsEligible(GatedOk, PlayerState)
                    && !GameMode.IsEligible(GatedWrong, PlayerState);
```

(Use the same PlayerState variable the other checks use; report via the same pass/fail print.)

- [ ] **Step 3.3: Add the `EvoSmoke` battery**

Append after `RaidXPReport`. NOTE: dummies MUST spawn through `USpawnDirector.SpawnElite`
(it binds `HandleEliteDeath`, so deaths reach `ARaidGameMode.HandleEnemyKilled` — a plain
`SpawnActor` dummy never fires the death path). Confirm the dummy class name with
`mcp__plugin_ue-as_as-helper__find_subclasses` on `AEliteEnemyBase` (expected: `ATargetDummy`
from the DL_Upgrades range); adapt the class literal if it differs. Confirm
`TryActivateAbilityByClass` / ability-granting bindings with
`mcp__plugin_ue-as_as-helper__list_type_methods` on `UAngelscriptAbilitySystemComponent`
(expected: `TryActivateAbilityByClass(TSubclassOf<UGameplayAbility>, bool)` and a grant method
such as `K2_GiveAbility` / `GiveAbility`); adapt the two call sites if named differently.

```angelscript
    // --- Debug: behavior-evolution battery (D-0020). Spawns its own dummies through the
    // SpawnDirector (so deaths reach the GameMode death path), drives the seam + abilities,
    // and asserts each evolution behavior. `[EvoSmoke] RESULT n/7` is the SmokeTest assertion.
    // Run WITHOUT UpgradeSmoke in the same session: UpgradeSmoke applies every pool GE, which
    // would pre-set the evolution flags this battery toggles deliberately. Host-only; polls at
    // boot like the other batteries.
    private int EvoRetries = 0;
    private int EvoPassed = 0;
    private AEliteEnemyBase EvoVortexDummy;
    private AEliteEnemyBase EvoSalvoDummy;
    private float EvoSalvoStartHP = 0.0;
    private AEliteEnemyBase EvoCarpetDummy;
    private float EvoCarpetStartHP = 0.0;

    UFUNCTION(Exec)
    void EvoSmoke()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        USpawnDirector Director = USpawnDirector::Get();
        if (GameMode == nullptr || ASC == nullptr || Combat == nullptr || Director == nullptr)
        {
            if (EvoRetries < 30)
            {
                EvoRetries++;
                System::SetTimer(this, n"EvoSmoke", 1.0, false);
                return;
            }
            Print("[EvoSmoke] gave up waiting for hero/gamemode", 8.0);
            return;
        }

        FVector H = Hero.GetActorLocation();
        FVector F = Hero.GetActorForwardVector();
        F.Z = 0.0;
        F = F.GetSafeNormal();
        FVector R = FVector(-F.Y, F.X, 0.0);

        // Check 1: Searing Arcs — chain arcs ignite their targets (per-shot params, no GE needed).
        AEliteEnemyBase A1 = Director.SpawnElite(ATargetDummy, H + F * 400.0, FRotator());
        AEliteEnemyBase B1 = Director.SpawnElite(ATargetDummy, H + F * 400.0 + R * 250.0, FRotator());
        bool bCheck1 = false;
        if (A1 != nullptr && B1 != nullptr && B1.Health != nullptr)
        {
            FWeaponShotParams Shot;
            Shot.Damage = 10.0;
            Shot.ChainCount = 5;        // generous: B1 must be among the nearest arcs even if
            Shot.ChainRadius = 300.0;   // placed range targets share the lane
            Shot.ChainIgniteFraction = 0.5;
            FireAt(Combat, Hero, A1, Shot);
            bCheck1 = B1.Health.HasActiveDot(ERogueDotType::Burn);
        }
        EvoCheck("searing arcs (ignite-on-arc)", bCheck1);

        // Check 2: Overwhelm — bonus arcs on a Clustered victim with base ChainCount 0.
        AEliteEnemyBase A2 = Director.SpawnElite(ATargetDummy, H - F * 500.0, FRotator());
        AEliteEnemyBase B2 = Director.SpawnElite(ATargetDummy, H - F * 500.0 + R * 200.0, FRotator());
        AEliteEnemyBase C2 = Director.SpawnElite(ATargetDummy, H - F * 500.0 - R * 200.0, FRotator());
        bool bCheck2 = false;
        if (A2 != nullptr && B2 != nullptr && C2 != nullptr)
        {
            Combat.MarkClustered(A2.GetActorLocation(), 50.0, 30.0);   // only the victim
            FWeaponShotParams Shot;
            Shot.Damage = 10.0;
            Shot.ChainCount = 0;
            Shot.ClusterChainBonusArcs = 2;
            Shot.ChainRadius = 300.0;
            FHitscanResult Res = FireAt(Combat, Hero, A2, Shot);
            bCheck2 = Res.ExtraEnemiesHit >= 2;
        }
        EvoCheck("overwhelm (cluster bonus arcs)", bCheck2);

        // Check 3: Toxic Burst — a poisoned victim's death dots its neighbors (GE + death path).
        bool bCheck3 = false;
        URogueUpgradeDef ToxicBurst = FindPoolDef(GameMode, "ToxicBurst");
        AEliteEnemyBase A3 = Director.SpawnElite(ATargetDummy, H + R * 700.0, FRotator());
        AEliteEnemyBase B3 = Director.SpawnElite(ATargetDummy, H + R * 700.0 + F * 150.0, FRotator());
        if (ToxicBurst != nullptr && ToxicBurst.Effect.Get() != nullptr
            && A3 != nullptr && B3 != nullptr && A3.Health != nullptr && B3.Health != nullptr)
        {
            ASC.ApplyGameplayEffectToTarget(ToxicBurst.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            A3.Health.ApplyDot(ERogueDotType::Poison, 1.0, 30.0, Hero);
            A3.Health.ApplyDamage(999999.0, Hero);   // death fires HandleEnemyKilled inline
            bCheck3 = B3.Health.HasActiveDot(ERogueDotType::Poison);
        }
        EvoCheck("toxic burst (poison death cloud)", bCheck3);

        // Check 4: Iron Bulwark — a Clustered kill grants squad Shield (its GE also adds MaxShield).
        bool bCheck4 = false;
        URogueUpgradeDef Bulwark = FindPoolDef(GameMode, "IronBulwark");
        AEliteEnemyBase A4 = Director.SpawnElite(ATargetDummy, H - R * 700.0, FRotator());
        if (Bulwark != nullptr && Bulwark.Effect.Get() != nullptr && A4 != nullptr && A4.Health != nullptr)
        {
            ASC.ApplyGameplayEffectToTarget(Bulwark.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            float ShieldBefore = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Shield", 0.0);
            Combat.MarkClustered(A4.GetActorLocation(), 50.0, 30.0);
            A4.Health.ApplyDamage(999999.0, Hero);
            float ShieldAfter = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Shield", 0.0);
            bCheck4 = ShieldAfter > ShieldBefore + 0.5;
        }
        EvoCheck("iron bulwark (clustered-kill shield)", bCheck4);

        // Checks 5-7 need real time (vortex pulses / salvo echo / carpet march) — phase chain.
        System::SetTimer(this, n"EvoPhaseVortex", 0.5, false);
    }

    UFUNCTION()
    private void EvoPhaseVortex()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("vortex phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        EvoVortexDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 600.0, FRotator());

        URogueUpgradeDef Horizon = FindPoolDef(GameMode, "EventHorizon");
        if (Horizon != nullptr && Horizon.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Horizon.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        ASC.K2_GiveAbility(UGA_Taunt, 1, -1);
        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Taunt, false);
        Print(f"[EvoSmoke] taunt activated={bActivated}", 6.0);

        // Base Clustered expires 3.0s after activation; the vortex refreshes through 3.0s, so at
        // +4.0s "still clustered" can only mean pulses fired.
        System::SetTimer(this, n"EvoCheckVortex", 4.0, false);
    }

    UFUNCTION()
    private void EvoCheckVortex()
    {
        bool bStill = EvoVortexDummy != nullptr && EvoVortexDummy.IsClustered();
        EvoCheck("event horizon (vortex refresh)", bStill);
        System::SetTimer(this, n"EvoPhaseSalvo", 0.5, false);
    }

    UFUNCTION()
    private void EvoPhaseSalvo()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("salvo phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        EvoSalvoDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 300.0, FRotator());
        EvoSalvoStartHP = (EvoSalvoDummy != nullptr && EvoSalvoDummy.Health != nullptr)
            ? EvoSalvoDummy.Health.Health : 0.0;

        URogueUpgradeDef Salvo = FindPoolDef(GameMode, "TwinSalvo");
        if (Salvo != nullptr && Salvo.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Salvo.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        ASC.K2_GiveAbility(UGA_Barrage, 1, -1);
        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Barrage, false);
        Print(f"[EvoSmoke] barrage(salvo) activated={bActivated}", 6.0);

        System::SetTimer(this, n"EvoCheckSalvo", 1.5, false);
    }

    UFUNCTION()
    private void EvoCheckSalvo()
    {
        // Base strike alone = 40 (unclustered). Strike + 60% echo = 64. Assert past the single.
        bool bEcho = false;
        if (EvoSalvoDummy != nullptr && EvoSalvoDummy.Health != nullptr)
            bEcho = (EvoSalvoStartHP - EvoSalvoDummy.Health.Health) >= 50.0;
        EvoCheck("twin salvo (echo strike)", bEcho);

        // Wait out the barrage cooldown before the carpet activation.
        System::SetTimer(this, n"EvoPhaseCarpet", 12.0, false);
    }

    UFUNCTION()
    private void EvoPhaseCarpet()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("carpet phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        // 900uu out: beyond the 600 base radius, covered by carpet pad 2 (3 * 300uu).
        EvoCarpetDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 900.0, FRotator());
        EvoCarpetStartHP = (EvoCarpetDummy != nullptr && EvoCarpetDummy.Health != nullptr)
            ? EvoCarpetDummy.Health.Health : 0.0;

        URogueUpgradeDef Carpet = FindPoolDef(GameMode, "CarpetBombing");
        if (Carpet != nullptr && Carpet.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Carpet.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Barrage, false);
        Print(f"[EvoSmoke] barrage(carpet) activated={bActivated}", 6.0);

        System::SetTimer(this, n"EvoCheckCarpet", 3.0, false);
    }

    UFUNCTION()
    private void EvoCheckCarpet()
    {
        bool bMarched = false;
        if (EvoCarpetDummy != nullptr && EvoCarpetDummy.Health != nullptr)
            bMarched = EvoCarpetDummy.Health.Health < EvoCarpetStartHP - 0.5;
        EvoCheck("carpet bombing (marching strip)", bMarched);
        Print(f"[EvoSmoke] RESULT {EvoPassed}/7", 20.0);
    }

    private void EvoFinishEarly(FString Why)
    {
        Print(f"[EvoSmoke] aborted: {Why}", 10.0);
        Print(f"[EvoSmoke] RESULT {EvoPassed}/7", 20.0);
    }

    private FHitscanResult FireAt(UCombatSubsystem Combat, AHeroCharacter Hero,
                                  AEliteEnemyBase Target, FWeaponShotParams Shot)
    {
        FVector From = Hero.GetMuzzleLocation();
        FVector Dir = (Target.GetActorLocation() - From).GetSafeNormal();
        return Combat.FireWeaponShot(From, From + Dir * 20000.0, Shot, Hero);
    }

    private URogueUpgradeDef FindPoolDef(ARaidGameMode GameMode, FString NamePart)
    {
        FString Filter = NamePart.ToLower();
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def == nullptr)
                continue;
            FString AssetName = FString(f"{Def.GetName()}").ToLower();
            if (AssetName.Contains(Filter))
                return Def;
        }
        Print(f"[EvoSmoke] pool card not found: {NamePart}", 8.0);
        return nullptr;
    }

    private void EvoCheck(FString Name, bool bPass)
    {
        if (bPass)
            EvoPassed++;
        FString Tag = bPass ? "PASS" : "FAIL";
        Print(f"[EvoSmoke] {Tag}: {Name}", 10.0);
    }
```

- [ ] **Step 3.4: Add the `DirectorReport` battery**

Append after the EvoSmoke block:

```angelscript
    // --- Debug: wave-director pure-function battery (D-0020). No spawning — asserts the plan
    // math. `[DirectorSmoke] RESULT 6/6` is the SmokeTest assertion. Safe anywhere, any time.
    UFUNCTION(Exec)
    void DirectorReport()
    {
        FDirectorTunables T;   // struct defaults mirror ARaidObjective's class defaults
        int Passed = 0;

        // The curve for human eyeballs: levels 1..12, representative wave 6, solo vs duo.
        for (int L = 1; L <= 12; L++)
        {
            FWavePlan P1 = RaidDirector::ComputeWavePlan(L, 6, 1, T);
            FWavePlan P2 = RaidDirector::ComputeWavePlan(L, 6, 2, T);
            Print(f"[Director] L{L}: solo={P1.FodderCount}@{P1.Interval}s duo={P2.FodderCount}@{P2.Interval}s inject={P1.EliteInjectIndex}", 12.0);
        }

        // 1: wave size never shrinks as level rises.
        bool bMonotonic = true;
        for (int L = 1; L < 12; L++)
        {
            if (RaidDirector::ComputeWavePlan(L + 1, 6, 1, T).FodderCount
                < RaidDirector::ComputeWavePlan(L, 6, 1, T).FodderCount)
                bMonotonic = false;
        }
        Passed += DirCheck("size monotonic in level", bMonotonic);

        // 2: interval respects the floor and never grows with level.
        bool bTempo = true;
        for (int L = 1; L <= 20; L++)
        {
            FWavePlan P = RaidDirector::ComputeWavePlan(L, 6, 1, T);
            if (P.Interval < T.MinInterval - 0.001)
                bTempo = false;
            if (L > 1 && P.Interval > RaidDirector::ComputeWavePlan(L - 1, 6, 1, T).Interval + 0.001)
                bTempo = false;
        }
        Passed += DirCheck("tempo floor + monotonic", bTempo);

        // 3: no injection below the start level.
        bool bNoEarly = true;
        for (int W = 0; W < 12; W++)
        {
            if (RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel - 1, W, 1, T).EliteInjectIndex >= 0)
                bNoEarly = false;
        }
        Passed += DirCheck("no injection before start level", bNoEarly);

        // 4: cadence — every 3rd wave at the start level, every 2nd at the fast level.
        bool bCadence = RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel, 3, 1, T).EliteInjectIndex >= 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel, 4, 1, T).EliteInjectIndex < 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectFastLevel, 4, 1, T).EliteInjectIndex >= 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectFastLevel, 3, 1, T).EliteInjectIndex < 0;
        Passed += DirCheck("injection cadence 3rd->2nd", bCadence);

        // 5: clamp holds under absurd inputs.
        bool bClamp = RaidDirector::ComputeWavePlan(99, 999, 4, T).FodderCount == T.MaxPerWave;
        Passed += DirCheck("size clamp", bClamp);

        // 6: pure determinism + a duo is never lighter than solo.
        FWavePlan X = RaidDirector::ComputeWavePlan(7, 9, 2, T);
        FWavePlan Y = RaidDirector::ComputeWavePlan(7, 9, 2, T);
        bool bDet = X.FodderCount == Y.FodderCount && X.Interval == Y.Interval
                 && X.EliteInjectIndex == Y.EliteInjectIndex
                 && X.FodderCount >= RaidDirector::ComputeWavePlan(7, 9, 1, T).FodderCount;
        Passed += DirCheck("determinism + player scaling", bDet);

        Print(f"[DirectorSmoke] RESULT {Passed}/6", 12.0);
    }

    private int DirCheck(FString Name, bool bPass)
    {
        FString Tag = bPass ? "PASS" : "FAIL";
        Print(f"[DirectorSmoke] {Tag}: {Name}", 10.0);
        return bPass ? 1 : 0;
    }
```

- [ ] **Step 3.5: Verify + commit**

Run `run_code_test`. Expected: 0 errors. Fix binding-name mismatches (`K2_GiveAbility`,
`TryActivateAbilityByClass`, `ATargetDummy`) via as-helper lookups if the compile complains.

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as
git commit -m "feat(debug): EvoSmoke + DirectorReport batteries; FlowSmoke hero-gating check (D-0020)"
```

---

### Task 4: Content — 11 GEs + 11 DAs + pool wiring (editor python commandlet)

**Files:**
- Create: `Tools/py_make_loop_v3_content.py`

- [ ] **Step 4.1: Write the script** (same proven pattern as `Tools/py_make_loop_v2_content.py`:
template export_text/import_text, idempotent, `L3C-FATAL` raises, `L3C-DONE` marker):

```python
# Create the v3 cards (D-0020): 4 behavior evolutions (chest), the Vanguard taunt track, and
# the Bombardier barrage track. Sets RequiredHeroClass on hero-track cards from the GameMode's
# HeroPawnClasses (0 = Vanguard, 1 = Bombardier). Appends all 11 to BP_RaidGamemode.UpgradePool.
#
# Headless commandlet; run with -run=pythonscript AFTER the Task-1 C++ build (the new attributes
# must exist). Editor must be CLOSED.
#
# FORBIDDEN: do not load or save GE_Upgrade_ChainDetonation (blueprint) — another workstream owns it.
#
# Success marker: unreal.log('L3C-DONE ge=11 da=11 pool=<n>')
# Raises RuntimeError on any failure.

import re
import unreal

GE_DIR = '/Game/Blueprints/GameplayEffects/LoopV3'
DA_DIR = '/Game/Upgrades'
GAMEMODE_BP = '/Game/Blueprints/BP_RaidGamemode'
TEMPLATE_COMBAT = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MoveSpeed_T1'
TEMPLATE_HEALTH = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MaxHealth_T1'

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def load_template_mod(ge_path, token):
    cls = unreal.load_object(None, ge_path + '.' + ge_path.split('/')[-1] + '_C')
    if cls is None:
        raise RuntimeError(f'L3C-FATAL: template GE class not found: {ge_path}')
    cdo = unreal.get_default_object(cls)
    mods = cdo.get_editor_property('modifiers')
    if len(mods) == 0:
        raise RuntimeError(f'L3C-FATAL: template GE has no modifiers: {ge_path}')
    txt = mods[0].export_text()
    if token not in txt or 'AddBase' not in txt:
        raise RuntimeError(f'L3C-FATAL: template missing "{token}"/"AddBase": {txt}')
    return txt


tmpl_combat = load_template_mod(TEMPLATE_COMBAT, 'MoveSpeed')
tmpl_health = load_template_mod(TEMPLATE_HEALTH, 'MaxHealth')


def make_mod(template_text, from_token, to_attr, magnitude):
    txt = template_text.replace(from_token, to_attr)
    txt = re.sub(r'ScalableFloatMagnitude=\(Value=[-0-9.]+',
                 f'ScalableFloatMagnitude=(Value={magnitude:.6f}', txt)
    mod = unreal.GameplayModifierInfo()
    mod.import_text(txt)
    check = mod.export_text()
    if to_attr not in check or 'AddBase' not in check:
        raise RuntimeError(f'L3C-FATAL: modifier round-trip failed for {to_attr}: {check}')
    unreal.log(f'L3C: mod OK {to_attr} mag={magnitude}')
    return mod


def make_ge(ge_name, mods_list):
    ge_path = f'{GE_DIR}/{ge_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(ge_path):
        bp = unreal.EditorAssetLibrary.load_asset(ge_path)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.GameplayEffect)
        bp = asset_tools.create_asset(ge_name, GE_DIR, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError(f'L3C-FATAL: failed to create GE {ge_path}')
    cdo = unreal.get_default_object(bp.generated_class())
    cdo.set_editor_property('duration_policy', unreal.GameplayEffectDurationType.INSTANT)
    cdo.set_editor_property('modifiers', mods_list)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    if not unreal.EditorAssetLibrary.save_asset(ge_path, only_if_is_dirty=False):
        raise RuntimeError(f'L3C-FATAL: save failed {ge_path}')
    unreal.log(f'L3C: GE OK {ge_path}')
    return bp


ref_da = unreal.EditorAssetLibrary.load_asset(f'{DA_DIR}/DA_Upgrade_Swift')
if ref_da is None:
    raise RuntimeError('L3C-FATAL: DA_Upgrade_Swift not found')
upgrade_def_class = ref_da.get_class()


def make_da(da_name, ge_bp, display, rarity, value_text, desc,
            bMilestone=False, bPrereqSelf=False, bApplyToSquad=False, bSynergyUpgrade=False,
            MaxStacks=5, PrereqA=None, PrereqAStacks=1, PrereqB=None, PrereqBStacks=1,
            RequiredHeroClass=None):
    da_path = f'{DA_DIR}/{da_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(da_path):
        da = unreal.EditorAssetLibrary.load_asset(da_path)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property('data_asset_class', upgrade_def_class)
        da = asset_tools.create_asset(da_name, DA_DIR, None, f)
    if da is None:
        raise RuntimeError(f'L3C-FATAL: failed to create DA {da_path}')
    da.set_editor_property('DisplayName', display)
    da.set_editor_property('Description', desc)
    da.set_editor_property('ValueText', value_text)
    da.set_editor_property('Rarity', rarity)
    da.set_editor_property('Effect', ge_bp.generated_class())
    da.set_editor_property('MaxStacks', MaxStacks)
    da.set_editor_property('bMilestone', bMilestone)
    da.set_editor_property('bApplyToSquad', bApplyToSquad)
    da.set_editor_property('bSynergyUpgrade', bSynergyUpgrade)
    da.set_editor_property('bPrereqSelf', bPrereqSelf)
    if PrereqA is not None:
        da.set_editor_property('PrereqA', PrereqA)
        da.set_editor_property('PrereqAStacks', PrereqAStacks)
    if PrereqB is not None:
        da.set_editor_property('PrereqB', PrereqB)
        da.set_editor_property('PrereqBStacks', PrereqBStacks)
    if RequiredHeroClass is not None:
        da.set_editor_property('RequiredHeroClass', RequiredHeroClass)
    if not unreal.EditorAssetLibrary.save_asset(da_path, only_if_is_dirty=False):
        raise RuntimeError(f'L3C-FATAL: save failed {da_path}')
    unreal.log(f'L3C: DA OK {da_path} ("{display}" r{rarity})')
    return da


def load_existing_da(name):
    da = unreal.EditorAssetLibrary.load_asset(f'{DA_DIR}/{name}')
    if da is None:
        raise RuntimeError(f'L3C-FATAL: prereq DA not found: {name}')
    return da


# Prereq references
da_Wildfire = load_existing_da('DA_Synergy_Wildfire')
da_VenomCascade = load_existing_da('DA_Synergy_VenomCascade')
da_SynOverwhelm = load_existing_da('DA_Synergy_Overwhelm')
da_IronVanguard = load_existing_da('DA_Synergy_IronVanguard')
da_WideBarrage = load_existing_da('DA_Upgrade_WideBarrage')

# Hero classes from the GameMode (0 = Vanguard, 1 = Bombardier)
gm_bp = unreal.EditorAssetLibrary.load_asset(GAMEMODE_BP)
gm_class = unreal.load_object(None, GAMEMODE_BP + '.BP_RaidGamemode_C')
if gm_bp is None or gm_class is None:
    raise RuntimeError('L3C-FATAL: BP_RaidGamemode not found')
gm_cdo = unreal.get_default_object(gm_class)
hero_classes = list(gm_cdo.get_editor_property('HeroPawnClasses'))
if len(hero_classes) < 2 or hero_classes[0] is None or hero_classes[1] is None:
    raise RuntimeError(f'L3C-FATAL: HeroPawnClasses needs Vanguard+Bombardier, got {hero_classes}')
vanguard_cls, bombardier_cls = hero_classes[0], hero_classes[1]
unreal.log(f'L3C: heroes vanguard={vanguard_cls.get_name()} bombardier={bombardier_cls.get_name()}')

pool_das = []

# ---- 4 behavior evolutions (chest: synergy-class, squad-wide, 1 stack, base card prereq) ----
ge = make_ge('GE_Evo_SearingArcs', [make_mod(tmpl_combat, 'MoveSpeed', 'ChainIgniteFraction', 0.5)])
pool_das.append(make_da('DA_Evo_SearingArcs', ge, 'Searing Arcs', 3,
    'Chain arcs ignite their targets (squad)',
    'The arc carries heat. Everything it touches burns.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_Wildfire))

ge = make_ge('GE_Evo_ToxicBurst', [make_mod(tmpl_combat, 'MoveSpeed', 'PoisonBurstDps', 6.0)])
pool_das.append(make_da('DA_Evo_ToxicBurst', ge, 'Toxic Burst', 3,
    'Poisoned enemies burst a poison cloud on death (squad)',
    'The toxin does not die with the host. It looks for the next one.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_VenomCascade))

ge = make_ge('GE_Evo_Overwhelm', [make_mod(tmpl_combat, 'MoveSpeed', 'ClusterChainBonusArcs', 2.0)])
pool_das.append(make_da('DA_Evo_Overwhelm', ge, 'Critical Mass', 3,
    'Hits on Clustered enemies arc to +2 extra targets (squad)',
    'Pack them tight enough and the lightning does the rest.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_SynOverwhelm))

ge = make_ge('GE_Evo_IronBulwark', [
    make_mod(tmpl_combat, 'MoveSpeed', 'ClusterKillShieldAmount', 3.0),
    make_mod(tmpl_health, 'MaxHealth', 'MaxShield', 25.0),
])
pool_das.append(make_da('DA_Evo_IronBulwark', ge, 'Iron Bulwark', 3,
    '+25 Max Shield; Clustered kills restore 3 Shield (squad)',
    'Hold the line together and the line holds you.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_IronVanguard))

# ---- Vanguard taunt track ----
ge = make_ge('GE_Upgrade_MagneticPull', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntRadiusBonus', 150.0)])
da_MagneticPull = make_da('DA_Upgrade_MagneticPull', ge, 'Magnetic Pull', 1,
    '+150 Taunt radius', 'The pull reaches further. Nothing escapes the gravity of a good taunt.',
    MaxStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_MagneticPull)

ge = make_ge('GE_Upgrade_IronGrip', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntClusterDurationBonus', 1.0)])
da_IronGrip = make_da('DA_Upgrade_IronGrip', ge, 'Iron Grip', 1,
    '+1s Clustered duration', 'They stay where you put them.',
    MaxStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_IronGrip)

ge = make_ge('GE_Upgrade_ConcussiveTaunt', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntDamage', 30.0)])
da_ConcussiveTaunt = make_da('DA_Upgrade_ConcussiveTaunt', ge, 'Concussive Taunt', 2,
    'Taunt deals 30 damage (Ability Power scaled)',
    'The shout hits like a shockwave now.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=2,
    PrereqA=da_MagneticPull, PrereqAStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_ConcussiveTaunt)

ge = make_ge('GE_Evo_EventHorizon', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntVortex', 1.0)])
pool_das.append(make_da('DA_Evo_EventHorizon', ge, 'Event Horizon', 3,
    'Taunt becomes a 3s vortex: re-pulls and refreshes Clustered',
    'Past this point, nothing leaves.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_ConcussiveTaunt, PrereqAStacks=1,
    PrereqB=da_IronGrip, PrereqBStacks=2, RequiredHeroClass=vanguard_cls))

# ---- Bombardier barrage track ----
ge = make_ge('GE_Upgrade_HighExplosives', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageDamageBonus', 0.25)])
da_HighExplosives = make_da('DA_Upgrade_HighExplosives', ge, 'High Explosives', 1,
    '+25% Barrage damage', 'More filler, more killer.',
    MaxStacks=3, RequiredHeroClass=bombardier_cls)
pool_das.append(da_HighExplosives)

ge = make_ge('GE_Upgrade_TwinSalvo', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageSalvoCount', 1.0)])
da_TwinSalvo = make_da('DA_Upgrade_TwinSalvo', ge, 'Twin Salvo', 2,
    'Barrage strikes a second time (60% damage)',
    'The first shell is the question. The second is the answer.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_HighExplosives, PrereqAStacks=3, RequiredHeroClass=bombardier_cls)
pool_das.append(da_TwinSalvo)

ge = make_ge('GE_Evo_CarpetBombing', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageCarpet', 1.0)])
pool_das.append(make_da('DA_Evo_CarpetBombing', ge, 'Carpet Bombing', 3,
    'Barrage becomes a telegraphed strip of 5 blasts marching forward',
    'Walk the line. Or rather — make them try.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_TwinSalvo, PrereqAStacks=1,
    PrereqB=da_WideBarrage, PrereqBStacks=2, RequiredHeroClass=bombardier_cls))

# ---- Pool wiring ----
pool = list(gm_cdo.get_editor_property('UpgradePool'))
before = len(pool)
existing = {p.get_name() for p in pool if p is not None}
for da in pool_das:
    if da.get_name() not in existing:
        pool.append(da)
        existing.add(da.get_name())
gm_cdo.set_editor_property('UpgradePool', pool)
unreal.BlueprintEditorLibrary.compile_blueprint(gm_bp)
if not unreal.EditorAssetLibrary.save_asset(GAMEMODE_BP, only_if_is_dirty=False):
    raise RuntimeError('L3C-FATAL: BP_RaidGamemode save failed')
unreal.log(f'L3C: UpgradePool {before} -> {len(pool)}')

unreal.log(f'L3C-DONE ge=11 da=11 pool={len(pool)}')
```

- [ ] **Step 4.2: Run the commandlet** (editor closed):

```powershell
& "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject" -run=pythonscript -script="C:\Users\btblu\Documents\RogueSmoke\Tools\py_make_loop_v3_content.py" -unattended -nullrhi -abslog="$env:TEMP\rs_l3c.log"
```

Then check `Select-String -Path "$env:TEMP\rs_l3c.log" -Pattern "L3C-DONE|L3C-FATAL"`.
Expected: `L3C-DONE ge=11 da=11 pool=35`. On `L3C-FATAL`, read the log, fix, re-run (idempotent).

- [ ] **Step 4.3: Commit** (script + the new/changed assets — list them explicitly from `git status`;
NEVER include `GE_Upgrade_ChainDetonation.uasset` or `SUPERPOWERS_HANDOFF.md`):

```bash
git add Tools/py_make_loop_v3_content.py "RogueSmoke/Content/Blueprints/GameplayEffects/LoopV3" "RogueSmoke/Content/Upgrades" "RogueSmoke/Content/Blueprints/BP_RaidGamemode.uasset"
git commit -m "feat(content): v3 cards - 4 behavior evolutions + taunt/barrage tracks, pool 24->35 (D-0020)"
```

---

### Task 5: Verification battery, SmokeTest gate, docs

**Files:**
- Modify: `Tools/SmokeTest.ps1`
- Modify: `DECISIONS.md`, `GLOSSARY.md`, `startup.md`

- [ ] **Step 5.1: Single-level batteries first** (debug cheaply before the full gate):

```powershell
powershell -File Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "EvoSmoke, DirectorReport" -WindowSec 75 -Grep "EvoSmoke|DirectorSmoke|Director\]"
```

Expected: `[EvoSmoke] RESULT 7/7` and `[DirectorSmoke] RESULT 6/6`. If EvoSmoke checks fail,
read `$env:TEMP\rs_boot.log` for the per-check PASS/FAIL lines and the `activated=` prints
(a false `activated=` on the carpet phase means the barrage cooldown outlasts the 12s gap —
raise the `EvoPhaseCarpet` delay). Then:

```powershell
powershell -File Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeSmoke, UpgradeFlowSmoke" -WindowSec 60 -Grep "RESULT"
```

Expected: `[UpgradeSmoke] RESULT 35/35` and `[FlowSmoke] RESULT 7/7`.

- [ ] **Step 5.2: Update `Tools/SmokeTest.ps1`** — replace the Upgrades case (lines 43-45) with:

```powershell
    # Upgrade firing range + the GE->attribute battery: every pool upgrade must move an attribute.
    @{ Name = "Upgrades";           Map = "/Game/Levels/DebuggingLevels/DL_Upgrades";
       Expect = @("[UpgradeTest] range ready: solo=1 line=4 cluster=5", "[UpgradeSmoke] RESULT 35/35", "[FlowSmoke] RESULT 7/7")
       Exec = "UpgradeSmoke, UpgradeFlowSmoke"; Window = 60 }
    # Behavior evolutions + wave director (D-0020). Separate boot: UpgradeSmoke pre-sets every
    # evolution flag, so EvoSmoke needs a clean ASC.
    @{ Name = "UpgradesEvo";        Map = "/Game/Levels/DebuggingLevels/DL_Upgrades";
       Expect = @("[EvoSmoke] RESULT 7/7", "[DirectorSmoke] RESULT 6/6")
       Exec = "EvoSmoke, DirectorReport"; Window = 75 }
```

- [ ] **Step 5.3: Full gate**: `powershell -File Tools\SmokeTest.ps1` — expected: all 9 levels PASS, exit 0.
(RaidArena now runs the director — watch its case for fatals; team level stays 1 there, so wave
sizes barely change and the breadcrumb expectation is unchanged.)

- [ ] **Step 5.4: Docs.** Append to `DECISIONS.md` after D-0019:

```markdown
## D-0020 — Behavior evolutions, hero ability tracks, wave director (2026-06-11)

**Decision.** (a) Behavior evolutions execute hybrid: hit-path branches compile into
`UCombatSubsystem::ProcOnHitEffects` switched by `URogueCombatSet` attributes
(ChainIgniteFraction, ClusterChainBonusArcs); death-path behaviors (PoisonBurstDps,
ClusterKillShieldAmount) run in AngelScript on `USpawnDirector::OnEnemyKilled` (fires before
pool recycle, so corpse DoT/Clustered state is readable). No generic on-hit event — that would
script the hot path. (b) Hero ability cards are gated by `URogueUpgradeDef.RequiredHeroClass`
(checked in IsEligible against the player's pawn; null pawn = ineligible). Tracks: stats →
milestone → evolution using D-0019 self-prereqs (Taunt: Magnetic Pull/Iron Grip → Concussive
Taunt → Event Horizon vortex; Barrage: High Explosives → Twin Salvo → Carpet Bombing).
Ability behaviors (vortex/salvo/carpet) are server-side timers in the instanced GAS abilities,
switched by flag attributes (TauntVortex/BarrageCarpet, >= 1 = on). (c) The wave director is a
pure function `RaidDirector::ComputeWavePlan(level, waveIndex, players, tunables)` — no RNG, no
world reads — consumed by `ARaidObjective.TickFodderWaves`: wave size +0.8 fodder/level,
interval −0.35s/level (floor 3.5s), elite injections every 3rd wave from level 4 / every 2nd
from level 8 (rotation keyed to wave index), wave size ×1.5 per extra player. Injected elites
are flagged `SetCountsAsObjectiveTarget(false)` — pressure, not clear-gates.

**Why.** Stat-combo synergy cards (D-0019) don't change how the game plays; evolutions do —
and the spec's research (Swarm evolutions, RoR2 proc chains) says transformed behavior is the
roguelike retention hook. Compiled hit-path keeps the per-bullet cost flat; scripted death-path
keeps iteration hot-reloadable where frequency is low.

**Verification.** `EvoSmoke` (7 behavior checks vs director-spawned dummies) and
`DirectorReport` (6 pure-function checks) in a separate SmokeTest boot from `UpgradeSmoke`
(which pre-sets every flag GE); FlowSmoke check 7 covers hero gating.
```

In `GLOSSARY.md`, add under "Roguelike systems" (after the "Squad reroll" bullet):

```markdown
- **Behavior evolution** — a chest/milestone card that changes a *mechanic*, not a number
  (chain arcs ignite, poisoned enemies burst, taunt becomes a vortex). Hit-path evolutions
  compile into the seam; death-path evolutions script on the kill event. (D-0020)
- **Ability track** — a hero-gated card line: stat cards → milestone → behavior evolution
  (Taunt: Magnetic Pull → Concussive Taunt → Event Horizon; Barrage: High Explosives →
  Twin Salvo → Carpet Bombing). Gated by `RequiredHeroClass` so only the right hero sees it.
- **Wave director** — the pure function scaling fodder-wave pressure with team level and squad
  size (size, tempo, deterministic elite injections). Injected elites never gate the clear.
```

In `startup.md`: add `EvoSmoke`, `DirectorReport` to the exec list; note pool is 35 cards and
the upgrade loop includes behavior evolutions + hero tracks + the wave director (D-0020).

- [ ] **Step 5.5: Final commit**

```bash
git add Tools/SmokeTest.ps1 DECISIONS.md GLOSSARY.md startup.md
git commit -m "docs+test(D-0020): SmokeTest evo/director gate; DECISIONS, GLOSSARY, startup"
```

---

## Self-review notes

- **Spec coverage:** evolutions A1-A4 → Tasks 1/2/4; tracks B → Tasks 1/2/4; director C → Tasks 2/3; hero gating + verification D → Tasks 2/3/5. Spec said `HasActiveDot` was a new C++ add — it already exists (HealthComponent.h:60); dropped. Spec's "0.4s telegraph lead" became 2 ticks × 0.25s = 0.5s so rings and blasts share one timer clock (noted in code).
- **Type consistency:** `PoisonBurstDps` everywhere (attribute, GE token, GetSquadAttribute read, EvoSmoke FindPoolDef("ToxicBurst") matches DA_Evo_ToxicBurst). `FDirectorTunables`/`FWavePlan`/`RaidDirector::ComputeWavePlan` consistent across Task 2.6/2.7/3.4. `SetCountsAsObjectiveTarget` defined Task 1.5, used Task 2.7.
- **Known soft spots for the implementer:** exact binding names `K2_GiveAbility` / `TryActivateAbilityByClass` / `ATargetDummy` / `APlayerState.GetPawn()` — verify with as-helper before assuming; the barrage cooldown duration (12s gap in EvoSmoke may need raising); FlowSmoke's internal check-report helper name (follow its existing pattern).

## Risk register

1. **Barrage/taunt Cooldown GE blocks the second EvoSmoke activation** → diagnostic `activated=` prints + adjustable phase gap; worst case the check fails visibly, not silently.
2. **DL_Upgrades placed range targets inside EvoSmoke arc radii** → checks use generous arc counts (ChainCount 5) and assert on the spawned dummy / on `ExtraEnemiesHit`, not on which actor got hit.
3. **`GS.Phase != InProgress` in DL_Upgrades would mute the death path** → v2 verified the run boots InProgress there (RaidGiveXP round trip); if check 3/4 fail with no `[Evo]` prints, print `GS.Phase` first.
4. **Changed `FodderMaxPerWave` default 20→32** — placed objectives that serialized an explicit override keep their value; DL levels were authored with class defaults, so they pick up 32.
5. **RaidArena smoke now exercises the director at team level 1** — injections can't fire below level 4, so the existing expectation line stays valid.
