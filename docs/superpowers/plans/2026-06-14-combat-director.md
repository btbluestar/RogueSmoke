# Combat Director (Attack Tokens) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A server-side combat director that grants a limited number of per-player attack tokens, so only a few "Engaged" elites run the full telegraph→attack loop at once while the rest hold at a ring as "Background" — the DOOM/Arkham combat-circle feel on the existing pooled Actors.

**Architecture:** Two halves split "decision vs simulation". (1) `AAttackingElite` (C++) gains an `EEngageState` + per-enemy token timers + a `bUsesAttackToken` flag; when a token-using elite is `Background` it approaches a *ring standoff* and never telegraphs. (2) `RaidCombatDirector` (AngelScript, a **stateless namespace** like `RaidDirector`) is ticked by `RaidObjective` every ~0.25s on the server: it gathers living elites + players, releases finished tokens, and promotes the best `Background` elites into free slots. All token state lives on the elite, so the director needs no persistent storage.

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); C++ `AActor`-based enemies; verification via `mcp__ue-cpp__build`, `as-helper run_code_test`, and `Tools/SmokeTest.ps1` (headless `-ExecCmds`).

---

## Design notes the engineer must know

- **The enemy loop you're extending** (`AAttackingElite::Tick`, server section): `AcquireTarget` (nearest living player) → dash handling → resolve in-flight telegraph (`PerformAttack` at the end of the wind-up) → if `IsTargetInAttackRange()` start a telegraph (when `AttackCooldown<=0`) → else `ApproachTarget` (closes to `PreferredRange`). You are gating the *start a telegraph* branch behind "is Engaged", and making `Background` token-users approach only to a further *ring standoff*.
- **Fodder are elites too:** `AFodderEnemy : AAttackingElite`. They must be **exempt** from token-gating (cheap contact melee) — they set `bUsesAttackToken = false`, and the director skips them.
- **All token bookkeeping lives on the elite** (so the AS director stays stateless): `TimeInEngageState` (seconds since the last state change — doubles as the Engaged *timeout* clock and the Background *re-eligibility cooldown* clock) and `bAttackedThisEngagement`. The director reads these and decides.
- **Server-only.** The director and all state mutation are gated on `HasAuthority()`. No new replication: a Background elite simply moves to the ring and doesn't telegraph; movement + the already-replicated `bTelegraphing` give clients the look for free.
- **AngelScript ↔ C++:** the director calls C++ `UFUNCTION`s on `AAttackingElite`. The enum is `UENUM(BlueprintType)`; getters are `BlueprintPure`, the setter `BlueprintCallable`. `GetAllActorsOfClass(TArray<AAttackingElite>&)` works on the C++ class. AS can read `Elite.Class` for archetype-variety comparison.
- **AS idioms:** `namespace`, `GetAllActorsOfClass(OutArray)`, `Cast<T>(x)`, `(A-B).Size()`, `Math::Abs/Max/Min/Acos/Cos`, `f"text {x}"`, `Print(msg, secs)`. No `import` lines (symbols are global).
- **Verification:** `mcp__ue-cpp__build` for C++ (returns parsed errors); `as-helper run_code_test` for AS compile; `SmokeTest.ps1` boots a level headless with `-ExecCmds` and greps a `RESULT n/n` breadcrumb. Execs **retry-poll** so they work as `-ExecCmds` (the spawn/sample isn't ready on frame 0).
- **Branch:** `combat-director` (already checked out). Commit after every task. Never commit broken C++ or `.as`.

## File structure

- **Modify** `RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.h` — `EEngageState`, token fields, `UFUNCTION`s, `RingStandoffMult`, `ApproachTarget` signature.
- **Modify** `RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.cpp` — Tick timing + gate + ring approach + attack flag.
- **Modify** `RogueSmoke/Source/RogueSmoke/Enemies/FodderEnemy.cpp` — set `bUsesAttackToken = false`.
- **Create** `RogueSmoke/Script/Objective/RaidCombatDirector.as` — the stateless director namespace.
- **Modify** `RogueSmoke/Script/Objective/RaidObjective.as` — tunables + tick the director behind `bEnableCombatDirector`.
- **Modify** `RogueSmoke/Script/Player/RaidPlayerController.as` — `CombatDirectorSmoke` exec.
- **Modify** `Tools/SmokeTest.ps1` — add the `CombatDirector` case.

---

### Task 1: `AAttackingElite` — engage-state, token fields, accessors

**Files:**
- Modify: `RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.h`
- Modify: `RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.cpp`

- [ ] **Step 1: Add the enum + fields + accessors to the header**

In `AttackingElite.h`, immediately ABOVE `UCLASS()` (after the includes), add the enum:

```cpp
UENUM(BlueprintType)
enum class EEngageState : uint8
{
	// Holds at a ring standoff, no telegraph/attack — until the combat director grants a token.
	Background,
	// Holds a token: runs the full approach→telegraph→attack loop.
	Engaged
};
```

Inside the class `public:` section (e.g. after `IsDashing()`), add the token API:

```cpp
	/** Combat-director scheduling. Background = circle/hold; Engaged = token-holder, full attack loop. */
	UFUNCTION(BlueprintCallable, Category="Combat|Director")
	void SetEngageState(EEngageState NewState);

	UFUNCTION(BlueprintPure, Category="Combat|Director")
	EEngageState GetEngageState() const { return EngageState; }

	/** Seconds since the last engage-state change (Engaged timeout clock AND Background cooldown clock). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	float GetTimeInEngageState() const { return TimeInEngageState; }

	/** True once this elite has committed an attack during its current Engaged stint (release signal). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool HasAttackedThisEngagement() const { return bAttackedThisEngagement; }

	/** Token-gated? False for fodder (AFodderEnemy) so the director ignores them. */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool GetUsesAttackToken() const { return bUsesAttackToken; }

	/** Alive per its HealthComponent (no health component = treat as alive). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	bool IsAlive() const;

	/** Ring standoff a Background token-user holds at (further than AttackRange). */
	UFUNCTION(BlueprintPure, Category="Combat|Director")
	float GetRingStandoff() const { return AttackRange * RingStandoffMult; }
```

In the same `public:` area add the tunable + flag (with the other UPROPERTYs):

```cpp
	/** Background token-users hold at AttackRange * this (the combat circle radius). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Combat|Director")
	float RingStandoffMult = 1.6f;

	/** Token-gated by the combat director. AFodderEnemy clears this (fodder attack freely). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Combat|Director")
	bool bUsesAttackToken = true;
```

In the `protected:` section (with the other transient state near `AttackCooldown`), add:

```cpp
	EEngageState EngageState = EEngageState::Background;
	float TimeInEngageState = 0.f;
	bool bAttackedThisEngagement = false;
```

Change the `ApproachTarget` declaration to take a stop range:

```cpp
	void ApproachTarget(float DeltaSeconds, float StopRange);
```

- [ ] **Step 2: Implement `SetEngageState` + `IsAlive` and update `ApproachTarget` in the .cpp**

In `AttackingElite.cpp`, change `ApproachTarget` to honor the passed stop range — replace the whole function:

```cpp
void AAttackingElite::ApproachTarget(float DeltaSeconds, float StopRange)
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector Mine = GetActorLocation();
	FVector ToTarget = Target->GetActorLocation() - Mine;
	ToTarget.Z = 0.f;
	const float Dist = ToTarget.Size();
	if (Dist <= StopRange)
	{
		return;
	}
	const FVector Dir = ToTarget / Dist;
	SetActorLocation(Mine + Dir * MoveSpeed * DeltaSeconds, /*bSweep=*/false);
	SetActorRotation(Dir.Rotation());
}
```

Add the two new methods (e.g. after `ApproachTarget`):

```cpp
void AAttackingElite::SetEngageState(EEngageState NewState)
{
	if (NewState == EngageState)
	{
		return;
	}
	EngageState = NewState;
	TimeInEngageState = 0.f;
	if (NewState == EEngageState::Engaged)
	{
		bAttackedThisEngagement = false;
	}
}

bool AAttackingElite::IsAlive() const
{
	return Health == nullptr || Health->Health > 0.f;
}
```

Reset the new state in `ClearTransientState` (so a pooled recycle starts clean) — add after the existing resets:

```cpp
	EngageState = EEngageState::Background;
	TimeInEngageState = 0.f;
	bAttackedThisEngagement = false;
```

- [ ] **Step 3: Build (the gate/timing come in Task 2; this must compile first)**

Run: `mcp__ue-cpp__build`
Expected: `Build SUCCEEDED`, 0 errors. (`ApproachTarget`'s old 1-arg call site is updated in Task 2 — to keep THIS task compiling, also do Step 4 before building.)

- [ ] **Step 4: Fix the one existing `ApproachTarget` call site** in `Tick` so the build passes now

In `AttackingElite.cpp::Tick`, the `else { ApproachTarget(DeltaSeconds); }` branch — change it to pass `PreferredRange` for now (Task 2 refines it):

```cpp
	else
	{
		ApproachTarget(DeltaSeconds, PreferredRange);
	}
```

Run: `mcp__ue-cpp__build`
Expected: `Build SUCCEEDED`, 0 errors.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.h RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.cpp
git commit -m "feat(combat): AAttackingElite engage-state + token fields + accessors"
```

---

### Task 2: `AAttackingElite` — gate the attack on Engaged + ring standoff in Background

**Files:**
- Modify: `RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.cpp` (`Tick`)

- [ ] **Step 1: Advance the engage-state clock each server tick**

In `Tick`, right after the `if (AttackCooldown > 0.f) { AttackCooldown -= DeltaSeconds; }` block, add:

```cpp
	TimeInEngageState += DeltaSeconds;
```

- [ ] **Step 2: Flag the attack-commit (release signal) in the telegraph resolve**

In `Tick`, the telegraph-resolve block — set the flag where the attack commits:

```cpp
	if (bTelegraphing)
	{
		TelegraphRemaining -= DeltaSeconds;
		FaceTarget();
		if (TelegraphRemaining <= 0.f)
		{
			bTelegraphing = false;
			if (IsTargetInAttackRange())
			{
				PerformAttack();
			}
			bAttackedThisEngagement = true; // committed this engagement (even a whiff) -> director can release
			AttackCooldown = AttackInterval;
		}
		return;
	}
```

- [ ] **Step 3: Gate the telegraph-start on Engaged + ring standoff in Background**

Replace the final `if (IsTargetInAttackRange()) { ... } else { ApproachTarget(...); }` block at the end of `Tick` with:

```cpp
	// Token-gating: a token-using elite only attacks while Engaged; in Background it holds at the ring.
	const bool bMayAttack = !bUsesAttackToken || EngageState == EEngageState::Engaged;
	if (bMayAttack && IsTargetInAttackRange())
	{
		FaceTarget();
		if (AttackCooldown <= 0.f)
		{
			bTelegraphing = true;
			TelegraphRemaining = TelegraphSeconds;
			OnTelegraphStarted(); // subclass hook: lock targets / ring attack-specific zones
		}
	}
	else
	{
		const float StopRange = (bUsesAttackToken && EngageState == EEngageState::Background)
			? GetRingStandoff() : PreferredRange;
		ApproachTarget(DeltaSeconds, StopRange);
	}
```

- [ ] **Step 4: Build**

Run: `mcp__ue-cpp__build`
Expected: `Build SUCCEEDED`, 0 errors.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Source/RogueSmoke/Enemies/AttackingElite.cpp
git commit -m "feat(combat): gate elite attack on Engaged + ring standoff in Background"
```

---

### Task 3: Fodder are exempt

**Files:**
- Modify: `RogueSmoke/Source/RogueSmoke/Enemies/FodderEnemy.cpp` (constructor)

- [ ] **Step 1: Clear the token flag for fodder**

In `AFodderEnemy`'s constructor (`FodderEnemy.cpp`), add (the field is inherited from `AAttackingElite`):

```cpp
	bUsesAttackToken = false; // fodder attack freely (cheap contact melee); the director ignores them
```

If `FodderEnemy.cpp` has no constructor body, add the assignment to the existing constructor; if there is no constructor at all, add one (declare `AFodderEnemy();` in `FodderEnemy.h` and define it). Verify by reading the file first.

- [ ] **Step 2: Build**

Run: `mcp__ue-cpp__build`
Expected: `Build SUCCEEDED`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Source/RogueSmoke/Enemies/FodderEnemy.cpp RogueSmoke/Source/RogueSmoke/Enemies/FodderEnemy.h
git commit -m "feat(combat): fodder exempt from attack tokens (bUsesAttackToken=false)"
```

---

### Task 4: `RaidCombatDirector` — the stateless director

**Files:**
- Create: `RogueSmoke/Script/Objective/RaidCombatDirector.as`

- [ ] **Step 1: Create the file**

```angelscript
// RaidCombatDirector.as
// The combat director (attack tokens). STATELESS namespace — all per-enemy token state lives on the
// elite (AAttackingElite). Server-only; called from RaidObjective on a ~0.25s throttle. Grants a limited
// number of "tokens" per player: only Engaged token-holders run the full attack loop; the rest hold at a
// ring as Background. Promotion = nearest + spread + archetype variety; release on attack/death/lost/timeout.

namespace RaidCombatDirector
{
    // One scheduling pass. TokensPerPlayer active attackers per living player; an Engaged elite is released
    // when it has attacked / died / lost its target / been Engaged longer than TimeoutSeconds; a Background
    // elite is eligible again only after CooldownSeconds and within EngagementRange of its target.
    void Tick(int TokensPerPlayer, float TimeoutSeconds, float CooldownSeconds, float EngagementRange)
    {
        TArray<AAttackingElite> Elites;
        GetAllActorsOfClass(Elites);

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);

        // 1) RELEASE: hand finished tokens back.
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken())
                continue;
            if (E.GetEngageState() != EEngageState::Engaged)
                continue;
            bool bRelease = !E.IsAlive()
                || E.HasAttackedThisEngagement()
                || E.GetCurrentTarget() == nullptr
                || E.GetTimeInEngageState() >= TimeoutSeconds;
            if (bRelease)
                E.SetEngageState(EEngageState::Background);
        }

        // 2) PROMOTE: per living player, fill free token slots with the best eligible Background elites.
        for (AHeroCharacter H : Heroes)
        {
            if (H == nullptr || H.IsIncapacitated())
                continue;
            APawn HeroPawn = Cast<APawn>(H);
            if (HeroPawn == nullptr)
                continue;

            int Used = CountEngagedTargeting(Elites, HeroPawn);
            int Free = TokensPerPlayer - Used;
            // Guard against an unbounded loop if nothing eligible remains.
            while (Free > 0)
            {
                AAttackingElite Best = PickBest(Elites, HeroPawn, H.GetActorLocation(), CooldownSeconds, EngagementRange);
                if (Best == nullptr)
                    break;
                Best.SetEngageState(EEngageState::Engaged);
                Free -= 1;
            }
        }
    }

    // Engaged token-users currently targeting HeroPawn.
    int CountEngagedTargeting(const TArray<AAttackingElite>& Elites, APawn HeroPawn)
    {
        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() == EEngageState::Engaged && E.GetCurrentTarget() == HeroPawn)
                N += 1;
        }
        return N;
    }

    // Highest-scoring eligible Background elite targeting HeroPawn, or null. Score: nearer is better,
    // minus a penalty per already-Engaged elite of the same archetype (variety) and per one on the same
    // side of the player (spread, ~within 60 degrees).
    AAttackingElite PickBest(const TArray<AAttackingElite>& Elites, APawn HeroPawn, FVector HeroLoc,
                             float CooldownSeconds, float EngagementRange)
    {
        AAttackingElite Best = nullptr;
        float BestScore = -100000.0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() != EEngageState::Background)
                continue;
            if (E.GetCurrentTarget() != HeroPawn)
                continue;
            if (E.GetTimeInEngageState() < CooldownSeconds)   // still on re-eligibility cooldown
                continue;
            float Dist = (E.GetActorLocation() - HeroLoc).Size();
            if (Dist > EngagementRange)                        // too far to bother promoting yet
                continue;

            float Score = -Dist;                               // nearer better
            Score -= 600.0 * float(SameArchetypeEngaged(Elites, HeroPawn, E.Class));   // variety
            Score -= 400.0 * float(SameSideEngaged(Elites, HeroPawn, HeroLoc, E.GetActorLocation()));  // spread

            if (Score > BestScore)
            {
                BestScore = Score;
                Best = E;
            }
        }
        return Best;
    }

    // Count of Engaged token-users targeting HeroPawn whose class == Cls (variety penalty).
    int SameArchetypeEngaged(const TArray<AAttackingElite>& Elites, APawn HeroPawn, UClass Cls)
    {
        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() == EEngageState::Engaged && E.GetCurrentTarget() == HeroPawn && E.Class == Cls)
                N += 1;
        }
        return N;
    }

    // Count of Engaged token-users targeting HeroPawn within ~60 degrees of CandidateLoc, as seen from the
    // hero (spread penalty so attackers surround instead of stacking one side).
    int SameSideEngaged(const TArray<AAttackingElite>& Elites, APawn HeroPawn, FVector HeroLoc, FVector CandidateLoc)
    {
        FVector CandDir = CandidateLoc - HeroLoc;
        CandDir.Z = 0.0;
        if (CandDir.SizeSquared() < 1.0)
            return 0;
        CandDir = CandDir.GetSafeNormal();

        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() != EEngageState::Engaged || E.GetCurrentTarget() != HeroPawn)
                continue;
            FVector D = E.GetActorLocation() - HeroLoc;
            D.Z = 0.0;
            if (D.SizeSquared() < 1.0)
                continue;
            D = D.GetSafeNormal();
            if (CandDir.DotProduct(D) > 0.5)   // cos(60deg) — same side
                N += 1;
        }
        return N;
    }
}
```

- [ ] **Step 2: Compile-check** (needs Task 1/2 built first so the C++ `UFUNCTION`s exist)

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors. If `AHeroCharacter.IsIncapacitated()` or `E.Class`/`UClass` don't resolve, report the exact error (these are used elsewhere in the project — `IsIncapacitated` in `RaidObjective.as`, `.Class` is the standard AS actor class accessor — but confirm).

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidCombatDirector.as
git commit -m "feat(combat): RaidCombatDirector stateless attack-token namespace"
```

---

### Task 5: `RaidObjective` ticks the director

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as`

- [ ] **Step 1: Add tunables + the flag** — with the other `UPROPERTY(EditAnywhere)` blocks (e.g. after the Director section), add:

```angelscript
    // --- Combat director (attack tokens): only N elites attack each player at once. ---
    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    bool bEnableCombatDirector = true;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    int AttackTokensPerPlayer = 3;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    float TokenTimeoutSeconds = 6.0;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    float TokenCooldownSeconds = 2.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    float TokenEngagementRange = 1100.0;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat Director")
    float CombatDirectorInterval = 0.25;
```

Add the throttle accumulator with the other private fields (near `WaveTimer`):

```angelscript
    private float CombatDirectorTimer = 0.0;
```

- [ ] **Step 2: Tick the director from `Tick`** — in `ARaidObjective::Tick`, inside the `if (!HasAuthority()) return;` server region (e.g. right after `Elapsed += DeltaSeconds;`), add:

```angelscript
        if (bEnableCombatDirector)
        {
            CombatDirectorTimer += DeltaSeconds;
            if (CombatDirectorTimer >= CombatDirectorInterval)
            {
                CombatDirectorTimer = 0.0;
                RaidCombatDirector::Tick(AttackTokensPerPlayer, TokenTimeoutSeconds,
                                         TokenCooldownSeconds, TokenEngagementRange);
            }
        }
```

- [ ] **Step 3: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(combat): RaidObjective ticks the combat director (throttled, gated)"
```

---

### Task 6: `CombatDirectorSmoke` headless test

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (new exec)
- Modify: `Tools/SmokeTest.ps1` (new case)

- [ ] **Step 1: Read an existing spawn-and-poll exec to match the pattern** — grep `RaidPlayerController.as` for an existing `UFUNCTION(Exec)` that spawns actors and samples over time (e.g. `GenShots`/`WeaponSmoke`), and for the `SpawnActor` + `System::SetTimer` retry-poll idiom. Match it.

- [ ] **Step 2: Add the `CombatDirectorSmoke` exec** — append in the controller class:

```angelscript
    // --- Combat-director smoke: spawn many token-using elites around the hero and assert only the
    // per-player budget are Engaged at once, and that tokens rotate over time. Boot RaidArena with
    // -ExecCmds="CombatDirectorSmoke". ---
    private int CDSpawned = 0;
    private int CDMaxEngagedSeen = 0;
    private int CDSamples = 0;
    private TArray<AAttackingElite> CDFirstEngaged;

    UFUNCTION(Exec)
    void CombatDirectorSmoke()
    {
        // Spawn 8 Carapace (token-using elites) in a tight ring around the local hero.
        APawn Hero = GetControlledPawn();
        if (Hero == nullptr)
        {
            System::SetTimer(this, n"CombatDirectorSmoke", 0.5, false);   // retry until possessed
            return;
        }
        FVector C = Hero.GetActorLocation();
        for (int i = 0; i < 8; i++)
        {
            float A = (2.0 * 3.14159265 * float(i)) / 8.0;
            FVector P = C + FVector(Math::Cos(A), Math::Sin(A), 0.0) * 250.0;
            ACarapace E = Cast<ACarapace>(SpawnActor(ACarapace, P, FRotator()));
            if (E != nullptr)
                CDSpawned += 1;
        }
        Print(f"[CombatDirectorSmoke] spawned {CDSpawned} elites", 6.0);
        CDSamples = 0;
        System::SetTimer(this, n"CombatDirectorSample", 0.8, true);   // sample repeatedly
    }

    UFUNCTION()
    void CombatDirectorSample()
    {
        ARaidObjective Obj = FindRaidObjective();
        int Budget = (Obj != nullptr) ? Obj.AttackTokensPerPlayer : 3;

        TArray<AAttackingElite> Elites;
        GetAllActorsOfClass(Elites);
        int Engaged = 0;
        TArray<AAttackingElite> NowEngaged;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() == EEngageState::Engaged)
            {
                Engaged += 1;
                NowEngaged.Add(E);
            }
        }
        if (Engaged > CDMaxEngagedSeen)
            CDMaxEngagedSeen = Engaged;
        if (CDFirstEngaged.Num() == 0 && NowEngaged.Num() > 0)
            CDFirstEngaged = NowEngaged;

        CDSamples += 1;
        if (CDSamples >= 8)
        {
            System::ClearAndInvalidateTimerHandle(...);   // see note below — use the stored handle / stop pattern
            // Rotation: at least one currently-Engaged elite differs from the first set we saw.
            bool bRotated = false;
            for (AAttackingElite E : NowEngaged)
            {
                bool bWasFirst = false;
                for (AAttackingElite F : CDFirstEngaged)
                    if (E == F) { bWasFirst = true; break; }
                if (!bWasFirst) { bRotated = true; break; }
            }
            int Pass = 0;
            int Total = 2;
            if (CDMaxEngagedSeen <= Budget && CDMaxEngagedSeen > 0) Pass += 1;
            else Print(f"[CombatDirectorSmoke] FAIL cap (max {CDMaxEngagedSeen} > budget {Budget})", 8.0);
            if (bRotated) Pass += 1;
            else Print("[CombatDirectorSmoke] FAIL no rotation", 8.0);
            Print(f"[CombatDirectorSmoke] RESULT {Pass}/{Total}", 15.0);
        }
    }
```

> Implementation notes for Step 2 (resolve against the live code in Step 1):
> - Use the controller's existing objective lookup (Plan C added `FindRaidObjective()` — reuse it) and its existing possessed-pawn accessor (whatever `GetControlledPawn()`/`GetPawn` form the file already uses).
> - Replace the `System::ClearAndInvalidateTimerHandle(...)` line with the project's actual repeating-timer stop idiom: capture the handle from `System::SetTimer(...)` into a member and clear it, OR switch `bIsLooping` to a guard `bool` checked at the top of `CombatDirectorSample` (simplest — set a `bCDDone` flag and early-return once printed; do NOT keep a dangling timer). Pick the form already used elsewhere in this file.
> - `GetControlledPawn`: use the same call the other execs use to get the local hero pawn.

- [ ] **Step 3: Add the SmokeTest case** — in `Tools/SmokeTest.ps1`, add to `$Cases` (after the `RaidArena` case):

```powershell
    @{ Name = "CombatDirector";     Map = "/Game/Levels/RaidArena";                       Expect = "[CombatDirectorSmoke] RESULT 2/2"; Exec = "CombatDirectorSmoke"; Window = 30 }
```

- [ ] **Step 4: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(combat): CombatDirectorSmoke asserts token cap + rotation (2/2)"
```

---

### Task 7: Verification gate (run from the controller session)

**Files:** none (verification only)

- [ ] **Step 1: C++ build + AS compile clean**

Run: `mcp__ue-cpp__build` (expect SUCCEEDED, 0 errors) and `mcp__plugin_ue-as_as-helper__run_code_test` (expect 0/0).

- [ ] **Step 2: CombatDirectorSmoke headless**

Boot `/Game/Levels/RaidArena` with `-ExecCmds="CombatDirectorSmoke"` (~30s window), grep `[CombatDirectorSmoke] RESULT 2/2`, no `Fatal error` / `Script call stack` / `LogScript: Error`.

- [ ] **Step 3: Full suite stays green (the key regression risk)**

Run `Tools/SmokeTest.ps1`. The director now runs on `RaidArena`'s elites too, so confirm `RaidArena`, `RaidLoopVictory`, `RaidLoopDefeat`, and the enemy cases still PASS. If `RaidLoopSmoke` regresses because it assumed all elites attack, set `bEnableCombatDirector = false` on the `RaidArena` placed objective (or default it off and turn it on only for `L_GenRaid`), and re-run.
Expected: all cases PASS.

- [ ] **Step 4: Optional PIE visual proof**

Launch the editor, 1-client PIE on `RaidArena` or `L_GenRaid`, advance a few seconds, and confirm via `actor_list` that only ≤ budget `AAttackingElite` report `GetEngageState()==Engaged`/`IsTelegraphing()` at once while the rest hold at the ring. (Screenshots are unreliable under background-throttle — use actor introspection.)

- [ ] **Step 5: Final commit (if any tuning was needed)**

```bash
git add -A
git commit -m "tune(combat): director defaults / gating after verification"
```

---

## Self-review against the spec

- **Spec §3 architecture (RaidCombatDirector AS stateless + AAttackingElite engage-state C++; RaidObjective ticks it):** Tasks 1-2 (C++), Task 4 (director), Task 5 (wiring). ✓
- **Spec §3 fodder exempt via bUsesAttackToken:** Task 3. ✓
- **Spec §4 token rules (per-player budget, nearest+spread+variety promotion, release on attack/death/lost/timeout, cooldown, 0.25s tick):** Task 4 (`Tick`/`PickBest`/`SameArchetypeEngaged`/`SameSideEngaged`), Task 5 (interval). ✓
- **Spec §5 authority/replication (server-only, no new replication, bEnableCombatDirector flag):** Task 5 (`HasAuthority` region + flag); no replication added. ✓
- **Spec §6 MVP scope (single budget; split/per-zone/intensity deferred):** single `AttackTokensPerPlayer`; nothing deferred is built. ✓
- **Spec §7 testing (build, run_code_test, CombatDirectorSmoke cap+rotation, full suite):** Tasks 6-7. ✓
- **Type consistency:** `EEngageState{Background,Engaged}` defined Task 1, used Tasks 2/4/6; accessors `GetEngageState`/`GetTimeInEngageState`/`HasAttackedThisEngagement`/`GetUsesAttackToken`/`IsAlive`/`GetRingStandoff`/`SetEngageState` defined Task 1, called Tasks 2/4/6; `ApproachTarget(float,float)` redefined Task 1, called Task 2; `RaidCombatDirector::Tick(int,float,float,float)` defined Task 4, called Task 5; objective fields `AttackTokensPerPlayer` etc. defined Task 5, read Task 6. ✓
- **Known live-code dependencies the engineer must confirm (flagged in-task):** `AFodderEnemy` constructor (Task 3 Step 1), the controller's pawn accessor + `FindRaidObjective` + repeating-timer stop idiom (Task 6 Step 2), `Health->Health` member name on `AEliteEnemyBase` (Task 1 Step 2 — used already in `AttackingElite.cpp`, so it exists). These are the only spots requiring a read of the live code rather than verbatim paste.
