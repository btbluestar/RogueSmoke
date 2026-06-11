# Movement & Shooting Feel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deadlock-lean movement feel (heavy gravity, instant response, slide-hop momentum) and Destiny-grade shooting feedback (layered anim system, camera feel, damage numbers, VFX/audio), per the approved spec.

**Architecture:** All gameplay changes stay in AngelScript on stock CMC (server-authoritative, CMC-predicted); every feedback layer is owning-client/all-client cosmetic. The linked-body fix is a new AS `UAnimInstance` + a layered ABP the user builds in the GUI from a generated guide. A `MoveTune` console makes every feel number live-tunable in PIE.

**Tech Stack:** UE 5.7 AngelScript fork (`F:\UEAS`), GAS, CommonUI HUD (programmatic widgets), Niagara (assets optional/deferred), GASP retarget pipeline.

**References (read before executing any task):**
- Spec: `docs/superpowers/specs/2026-06-11-movement-shooting-feel-design.md`
- Research: `docs/superpowers/research/2026-06-11-feel-research-notes.md` (starting numbers + ABP node recipe — cited as "Research A/B/C" below)
- House rules: `CLAUDE.md`, `CODING_STANDARDS.md` §4 (networking), `startup.md` §3–4 (verification + gotchas)

**Execution rules (non-negotiable):**
- ONE editor. `run_code_test`, builds, headless boots, MCP/PIE = serialized through the orchestrating session. Subagents AUTHOR files; they do not touch the editor.
- Verify `.as` changes with the as-helper `run_code_test` MCP tool (NOT a C++ build, NOT launching the editor).
- `Tools\SmokeTest.ps1` must pass before every commit that touches gameplay. Run script files, not inline `Start-Process` (AMSI).
- Never stage `RogueSmoke/Content/Blueprints/GE_Upgrade_ChainDetonation.uasset` or `SUPERPOWERS_HANDOFF.md` (other workstreams).
- AS gotchas: RPCs default RELIABLE (mark cosmetics `Unreliable`); function params are const (copy to local); `Print()` is dev-only; struct UPROPERTY reads are value copies (read-modify-write).
- Commit messages: PowerShell 5.1 mangles embedded double quotes — use here-strings, no quotes in messages.

**Task → file map:**

| Task | Files | Executor |
|---|---|---|
| 1 Spike: AS anim instance | Create `Script/Player/HeroAnimInstance.as` (minimal) | subagent author, session verify |
| 2 Movement physics | Rewrite `Script/Player/LocomotionComponent.as`; edit `Script/Player/HeroCharacter.as` | subagent author, session verify |
| 3 MoveTune + MoveSmoke | Edit `Script/Player/RaidPlayerController.as` | subagent author, session verify |
| 4 Camera feel | Create `Script/Player/CameraFeelComponent.as`; edit `HeroCharacter.as` | subagent author, session verify |
| 5 Full anim instance | Rewrite `Script/Player/HeroAnimInstance.as` | subagent author, session verify |
| 6 GASP retarget bake | Content: staged UEFN anims → baked `/Game/Characters/Mannequins/Anims/GASP/` | main session (editor) |
| 7 Montages + ABP guide | Create montage assets; create `docs/guides/ABP_HERO_BUILD_GUIDE.md` | session (editor) + subagent (doc) |
| 8 ABP GUI build + hookup | USER builds `BS_Rifle_Strafe`, `ABP_Hero`; session assigns to hero BPs | USER + main session |
| 9 Montage wiring | Edit `HeroCharacter.as` (fire/reload montage plumbing) | subagent author, session verify |
| 10 Damage numbers | Edit `Weapons/Abilities/GA_WeaponFire.as`, `HeroCharacter.as`, `UI/RogueHUDWidget.as` | subagent author, session verify |
| 11 FX/audio fields + FireFX | Edit `Weapons/WeaponDefinition.as`, `HeroCharacter.as`, `GA_WeaponFire.as` | subagent author, session verify |
| 12 Kill confirm + fire tail | Edit `HeroCharacter.as` + investigate death path | subagent author, session verify |
| 13 Verification, tuning, docs | SmokeTest, 2P PIE, feel sessions, `DECISIONS.md`/`GLOSSARY.md`/`startup.md` | main session + USER |

Tasks 2→3→4 and 9→12 share `HeroCharacter.as` / `RaidPlayerController.as` — run them **sequentially**. Task 5 is independent of 3–4. Tasks 6–8 (editor/asset track) can interleave with 9–12 but never in parallel with another editor operation.

---

### Task 1: Spike — AS `UAnimInstance` subclass compiles

The one unproven architectural assumption. `UAnimInstance` is `Blueprintable` (verified), so an AS subclass should work; this proves it before anything depends on it.

**Files:**
- Create: `RogueSmoke/Script/Player/HeroAnimInstance.as`

- [ ] **Step 1: Verify the override surface.** Run as-helper `describe_type` on `UAnimInstance` and confirm the `BlueprintUpdateAnimation` event exists and `TryGetPawnOwner` is callable. Also `describe_type ACharacter` and note the AS override names for the landed/jumped events (expect `OnLanded(FHitResult)` / `OnJumped()` — K2_ prefixes are stripped by the fork). Record actual names for Tasks 2/5.

- [ ] **Step 2: Write the minimal anim instance**

```angelscript
// HeroAnimInstance.as
// The hero's anim-graph data source (Lyra's LyraAnimInstance role): AngelScript computes every
// variable the ABP graph reads, so all animation logic hot-reloads and the graph stays pure
// blending. Task 5 fills in the full variable set; this spike proves the subclass + override work.
class URogueHeroAnimInstance : UAnimInstance
{
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float GroundSpeed = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsFalling = false;

    UFUNCTION(BlueprintOverride)
    void BlueprintUpdateAnimation(float DeltaTimeX)
    {
        APawn Pawn = TryGetPawnOwner();
        if (Pawn == nullptr)
            return;
        FVector Vel = Pawn.GetVelocity();
        GroundSpeed = FVector(Vel.X, Vel.Y, 0.0).Size();
    }
}
```
(If `TryGetPawnOwner` or the override name differ per Step 1, use what `describe_type` reported.)

- [ ] **Step 3: Compile-verify.** Run as-helper `run_code_test`. Expected: exit 0, no script errors. If the subclass is rejected by the fork: STOP, report — the fallback (spec §2.5.4) is a thin C++ `URogueHeroAnimInstance`; do not improvise it without the user.

- [ ] **Step 4: Commit**
```
git add RogueSmoke/Script/Player/HeroAnimInstance.as
git commit -m "feat(anim): spike AS UAnimInstance subclass for the hero anim graph"
```

---

### Task 2: Movement physics overhaul (Deadlock-lean + slide-hop)

Numbers and rules from Research A. Everything becomes a tunable `UPROPERTY` so MoveTune (Task 3) and later meta-progression can move it.

**Files:**
- Rewrite: `RogueSmoke/Script/Player/LocomotionComponent.as`
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as` (jump/landed hooks; lines ~227–276 area)

- [ ] **Step 1: Rewrite `LocomotionComponent.as`**

Replace the whole file with (preserving the existing public API used by HeroCharacter: `Initialize`, `SetBaseSpeed`, `SetSprint`, `SetFocus`, `RequestCrouchOrSlide`, `ReleaseCrouch`, `TickLocomotion`, `IsSprinting`, `IsSliding`):

```angelscript
// LocomotionComponent.as
// The hero's mobility state machine (D-0015, reworked for the feel pass — see
// docs/superpowers/research/2026-06-11-feel-research-notes.md Report A):
// Deadlock-lean ground response, heavy gravity, and Apex-style slide-hop momentum
// (boost-arming threshold + hard cap + landing re-entry), all on stock CMC so movement
// stays client-predicted. Every knob is a UPROPERTY: MoveTune sets them live in PIE and
// meta-progression (D-0013) modifies them later.
class URogueLocomotionComponent : UActorComponent
{
    // --- Ground response (input feels direct) ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float MaxAcceleration = 6144.0;          // sv_accelerate 10 ~= full speed in ~0.15 s

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingDecelerationWalking = 3072.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float GroundFriction = 10.0;

    // Separate braking friction decouples stop-feel from turn-feel (PBCharacterMovement pattern).
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingFriction = 6.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingFrictionFactor = 1.0;

    // --- Gravity & air (the floatiness fix) ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float GravityScale = 1.8;                // Deadlock ~2.07, Apex ~1.46; lean Deadlock

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float JumpZVelocity = 750.0;             // similar height as before, faster arc

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float DoubleJumpZVelocity = 700.0;       // second jump reads better slightly weaker

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float JumpMaxHoldTime = 0.0;             // both refs use fixed impulse; tunable for PIE A/B

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float AirControl = 0.9;                  // Deadlock-style direct air steering

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float FallingLateralFriction = 0.0;      // preserve slide-hop momentum in air

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Jump")
    int MaxJumpCount = 2;

    // --- Sprint / crouch / focus ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Sprint")
    float SprintSpeedMultiplier = 1.6;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Crouch")
    float CrouchSpeed = 300.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Focus")
    float FocusMoveMultiplier = 0.8;

    // --- Slide (the fastest ground state; momentum tool) ---
    // Boost target / cap / arming threshold are fractions of the CURRENT sprint speed so
    // MoveSpeed upgrades scale the whole slide economy with them.
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostMultiplier = 1.3;        // boost target = sprint * this (Apex ~1.34 cap)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideSpeedCapMultiplier = 1.5;     // hard cap = sprint * this (Deadlock dash-jump 1.55x)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostArmMultiplier = 1.1;     // boost ONLY if speed < sprint * this (anti-bhop, Apex rule)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideEntryMinFraction = 0.85;      // entry needs speed >= sprint * this

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideExitSpeed = 540.0;            // ~0.9 * base walk: below this the slide ends

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideGroundFriction = 0.6;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBraking = 256.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideDownhillAccel = 2048.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideMaxDuration = 1.5;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostCooldown = 1.8;          // belt-and-suspenders on top of the arming rule

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideSustainMinSlope = 0.1;        // ~6 deg: descending refreshes the duration

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    bool bRequireSprintToSlide = true;

    // --- Runtime state (CMC replicates the actual motion; this mirrors on client+server) ---
    private ACharacter OwnerCharacter;
    private UCharacterMovementComponent Move;
    private float BaseWalkSpeed = 600.0;
    private bool bSprinting = false;
    private bool bFocusing = false;
    private bool bCrouchHeld = false;
    private bool bSliding = false;
    private bool bPostSlideHopAir = false;   // cap applies in air after a slide-hop
    private float SlideTimeRemaining = 0.0;
    private float BoostCooldownRemaining = 0.0;
    private float LastLandingFallSpeed = 0.0;  // |Z| at last landing; anim/camera read it

    void Initialize(ACharacter InOwner)
    {
        OwnerCharacter = InOwner;
        if (OwnerCharacter == nullptr)
            return;
        Move = OwnerCharacter.CharacterMovement;
        if (Move == nullptr)
            return;

        // Enable crouching (struct property is a value copy in AS: read-modify-write).
        FNavAgentProperties NavProps = Move.NavAgentProps;
        NavProps.bCanCrouch = true;
        Move.NavAgentProps = NavProps;

        ApplyMovementConfig();
    }

    // Push every tunable into the CMC/character. Initialize, MoveTune, and upgrades call this.
    void ApplyMovementConfig()
    {
        if (Move == nullptr || OwnerCharacter == nullptr)
            return;
        Move.MaxAcceleration = MaxAcceleration;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        Move.BrakingFriction = BrakingFriction;
        Move.BrakingFrictionFactor = BrakingFrictionFactor;
        Move.bUseSeparateBrakingFriction = true;
        Move.GravityScale = GravityScale;
        Move.JumpZVelocity = JumpZVelocity;
        Move.AirControl = AirControl;
        Move.FallingLateralFriction = FallingLateralFriction;
        OwnerCharacter.JumpMaxCount = MaxJumpCount;
        OwnerCharacter.JumpMaxHoldTime = JumpMaxHoldTime;
        // GroundFriction is stateful (the slide swaps it); only set when not mid-slide.
        if (!bSliding)
            Move.GroundFriction = GroundFriction;
        ApplyGroundSpeed();
    }

    void SetBaseSpeed(float NewBaseSpeed)
    {
        if (NewBaseSpeed > 0.0)
            BaseWalkSpeed = NewBaseSpeed;
        ApplyGroundSpeed();
    }

    void SetSprint(bool bWantsSprint)   { bSprinting = bWantsSprint; ApplyGroundSpeed(); }
    void SetFocus(bool bWantsFocus)     { bFocusing = bWantsFocus;   ApplyGroundSpeed(); }

    bool IsSprinting() const { return bSprinting; }
    bool IsSliding() const { return bSliding; }
    bool IsCrouchHeld() const { return bCrouchHeld; }
    float GetLastLandingFallSpeed() const { return LastLandingFallSpeed; }
    float SprintSpeed() const { return BaseWalkSpeed * SprintSpeedMultiplier; }
    float SlideCap() const { return SprintSpeed() * SlideSpeedCapMultiplier; }

    void RequestCrouchOrSlide()
    {
        bCrouchHeld = true;
        if (OwnerCharacter == nullptr || Move == nullptr)
            return;

        bool bGrounded = Move.IsMovingOnGround();
        bool bFastEnough = HorizontalSpeed() >= SprintSpeed() * SlideEntryMinFraction;
        bool bSprintOk = !bRequireSprintToSlide || bSprinting;

        if (bGrounded && bFastEnough && bSprintOk && !bSliding)
            StartSlide();
        else
            OwnerCharacter.Crouch();
    }

    void ReleaseCrouch()
    {
        bCrouchHeld = false;
        if (bSliding)
            EndSlide();
        else if (OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    // Jump pressed while sliding (owning client; the server converges via the airborne check in
    // TickLocomotion). Ends the slide WITHOUT bleeding speed: friction restore only matters on
    // ground, FallingLateralFriction 0 keeps the velocity in the air. Keeps crouch if held so
    // landing can chain straight into the next slide (the Deadlock loop).
    void NotifySlideJump()
    {
        if (!bSliding)
            return;
        bSliding = false;
        bPostSlideHopAir = true;
        Move.GroundFriction = GroundFriction;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        // No UnCrouch: airborne crouch is the slide-hop chain posture.
    }

    // Second-jump shaping: stock CMC reuses JumpZVelocity for every jump; rescale the second.
    // Runs on predicted client + server (OnJumped fires on both) and is idempotent (sets, not adds).
    void NotifyJumped(int JumpCount)
    {
        if (Move == nullptr || JumpCount < 2)
            return;
        FVector Vel = Move.Velocity;
        Move.Velocity = FVector(Vel.X, Vel.Y, DoubleJumpZVelocity);
    }

    // Landed (server + owning client). Re-enter the slide instantly if the chain is alive.
    void NotifyLanded(float FallSpeed)
    {
        LastLandingFallSpeed = FallSpeed;
        bPostSlideHopAir = false;
        if (OwnerCharacter == nullptr || Move == nullptr)
            return;
        bool bFastEnough = HorizontalSpeed() >= SprintSpeed() * SlideEntryMinFraction;
        bool bSprintOk = !bRequireSprintToSlide || bSprinting;
        if (bCrouchHeld && bFastEnough && bSprintOk && !bSliding)
            StartSlide();
    }

    private void StartSlide()
    {
        bSliding = true;
        SlideTimeRemaining = SlideMaxDuration;
        OwnerCharacter.Crouch();

        Move.GroundFriction = SlideGroundFriction;
        Move.BrakingDecelerationWalking = SlideBraking;

        FVector Vel = Move.Velocity;
        FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
        FVector Dir = Flat.GetSafeNormal();
        if (Dir.IsNearlyZero())
            Dir = OwnerCharacter.GetActorForwardVector();

        // Apex's anti-bhop governor: the boost only arms below the threshold; above it you keep
        // your speed but gain nothing. Idempotent on client+server (sets toward a target).
        float Speed = Flat.Size();
        float Target = Speed;
        if (Speed < SprintSpeed() * SlideBoostArmMultiplier && BoostCooldownRemaining <= 0.0)
        {
            Target = Math::Max(Speed, SprintSpeed() * SlideBoostMultiplier);
            BoostCooldownRemaining = SlideBoostCooldown;
        }
        Target = Math::Min(Target, SlideCap());
        FVector NewFlat = Dir * Target;
        Move.Velocity = FVector(NewFlat.X, NewFlat.Y, Vel.Z);
    }

    private void EndSlide()
    {
        bSliding = false;
        Move.GroundFriction = GroundFriction;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        if (!bCrouchHeld && OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    private void ApplyGroundSpeed()
    {
        if (Move == nullptr)
            return;
        float Speed = BaseWalkSpeed * (bSprinting ? SprintSpeedMultiplier : 1.0);
        if (bFocusing)
            Speed *= FocusMoveMultiplier;
        Move.MaxWalkSpeed = Speed;
        Move.MaxWalkSpeedCrouched = CrouchSpeed;
    }

    private float HorizontalSpeed() const
    {
        if (Move == nullptr)
            return 0.0;
        FVector Vel = Move.Velocity;
        return FVector(Vel.X, Vel.Y, 0.0).Size();
    }

    void TickLocomotion(float DeltaSeconds)
    {
        if (Move == nullptr || OwnerCharacter == nullptr)
            return;

        if (BoostCooldownRemaining > 0.0)
            BoostCooldownRemaining -= DeltaSeconds;

        // Post-slide-hop air: hold the hard cap so air control can't compound past it.
        if (bPostSlideHopAir && Move.IsFalling())
        {
            FVector Vel = Move.Velocity;
            FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
            if (Flat.Size() > SlideCap())
            {
                FVector Capped = Flat.GetSafeNormal() * SlideCap();
                Move.Velocity = FVector(Capped.X, Capped.Y, Vel.Z);
            }
        }

        if (!bSliding)
            return;

        // Left the ground mid-slide (jumped or rolled off a ledge): convert to the hop-air state.
        // This is also how the SERVER converges when the owning client called NotifySlideJump.
        if (!Move.IsMovingOnGround())
        {
            NotifySlideJump();
            return;
        }

        // Downhill: accelerate along the slope and sustain the slide (Apex-style).
        FVector FloorNormal = Move.CurrentFloor.HitResult.Normal;
        FVector Downhill = FVector(FloorNormal.X, FloorNormal.Y, 0.0);
        bool bDescending = false;
        if (!Downhill.IsNearlyZero())
        {
            FVector DownDir = Downhill.GetSafeNormal();
            Move.Velocity += DownDir * SlideDownhillAccel * DeltaSeconds;

            FVector VelFlat = FVector(Move.Velocity.X, Move.Velocity.Y, 0.0);
            bDescending = Downhill.Size() >= SlideSustainMinSlope
                && VelFlat.GetSafeNormal().DotProduct(DownDir) > 0.0;
        }

        // Cap while sliding too (downhill accel must not run away).
        FVector SVel = Move.Velocity;
        FVector SFlat = FVector(SVel.X, SVel.Y, 0.0);
        if (SFlat.Size() > SlideCap())
        {
            FVector Capped = SFlat.GetSafeNormal() * SlideCap();
            Move.Velocity = FVector(Capped.X, Capped.Y, SVel.Z);
        }

        if (bDescending)
            SlideTimeRemaining = SlideMaxDuration;
        else
            SlideTimeRemaining -= DeltaSeconds;

        if (SlideTimeRemaining <= 0.0 || HorizontalSpeed() < SlideExitSpeed)
            EndSlide();
    }
}
```

- [ ] **Step 2: Hook jump/landed events in `HeroCharacter.as`.** Replace `DoJump` and add the two event overrides (names per Task 1 Step 1 findings; expected as below):

```angelscript
    UFUNCTION(BlueprintCallable)
    void DoJump()
    {
        if (IsIncapacitated())
            return;
        // Slide-hop: jumping out of a slide keeps 100% of horizontal velocity (D-0015 rework).
        if (Locomotion.IsSliding())
            Locomotion.NotifySlideJump();
        Jump();
    }

    // Fires on the predicted client and the server; idempotent (sets the second jump's Z).
    UFUNCTION(BlueprintOverride)
    void OnJumped()
    {
        Locomotion.NotifyJumped(JumpCurrentCount);
    }

    // Fires on the server and the owning client. Slide re-entry + camera/anim get the fall speed.
    UFUNCTION(BlueprintOverride)
    void OnLanded(FHitResult Hit)
    {
        float FallSpeed = Math::Abs(CharacterMovement.Velocity.Z);
        Locomotion.NotifyLanded(FallSpeed);
    }
```
Note: stock `ACharacter` velocity at `OnLanded` time may already be ground-projected; if `FallSpeed` reads ~0 in testing, capture `Velocity.Z` each tick into a 2-frame history in the locomotion component instead — flag it in the task report rather than silently shipping a zero.

- [ ] **Step 3: Compile-verify.** as-helper `run_code_test`. Expected: exit 0. If `OnJumped`/`OnLanded`/`JumpCurrentCount`/`JumpMaxHoldTime` names fail to resolve, fix against `describe_type ACharacter` output (do NOT guess variants blindly).

- [ ] **Step 4: Headless boot sanity.** `Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Combat -Grep "error|Error"` — expect a clean boot, no script errors in log.

- [ ] **Step 5: Commit**
```
git add RogueSmoke/Script/Player/LocomotionComponent.as RogueSmoke/Script/Player/HeroCharacter.as
git commit -m "feat(movement): Deadlock-lean physics + Apex slide-hop rules (research Report A)"
```

---

### Task 3: MoveTune console + MoveSmoke battery

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (append to the debug exec section)

- [ ] **Step 1: Add the execs.** Append inside `ARaidPlayerController` (follow the existing exec style around `RaidDebugCam`):

```angelscript
    // --- Debug: live movement-feel tuning (the convergence tool for the feel pass). -------------
    // `MoveTune`                  — dump every knob as a paste-ready defaults block.
    // `MoveTune <Param> <Value>`  — set one knob live (host PIE; values are case-insensitive
    //                               partial matches against the names printed by the dump).
    // Host-only: on a listen server host = authority + local, so one apply covers prediction
    // and simulation. A remote client typing it would desync its prediction — refused.
    UFUNCTION(Exec)
    void MoveTune(FString Param = "", float Value = 0.0)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || Hero.Locomotion == nullptr)
        {
            Print("[MoveTune] needs a hero pawn", 4.0);
            return;
        }
        if (!HasAuthority())
        {
            Print("[MoveTune] host only (remote tuning would desync prediction)", 6.0);
            return;
        }
        URogueLocomotionComponent L = Hero.Locomotion;

        if (Param.IsEmpty() || Param.ToLower() == "dump")
        {
            Print("[MoveTune] --- paste-ready defaults (LocomotionComponent.as) ---", 20.0);
            Print(f"[MoveTune] MaxAcceleration = {L.MaxAcceleration}; BrakingDecelerationWalking = {L.BrakingDecelerationWalking};", 20.0);
            Print(f"[MoveTune] GroundFriction = {L.GroundFriction}; BrakingFriction = {L.BrakingFriction}; BrakingFrictionFactor = {L.BrakingFrictionFactor};", 20.0);
            Print(f"[MoveTune] GravityScale = {L.GravityScale}; JumpZVelocity = {L.JumpZVelocity}; DoubleJumpZVelocity = {L.DoubleJumpZVelocity};", 20.0);
            Print(f"[MoveTune] JumpMaxHoldTime = {L.JumpMaxHoldTime}; AirControl = {L.AirControl}; FallingLateralFriction = {L.FallingLateralFriction};", 20.0);
            Print(f"[MoveTune] SprintSpeedMultiplier = {L.SprintSpeedMultiplier}; CrouchSpeed = {L.CrouchSpeed}; FocusMoveMultiplier = {L.FocusMoveMultiplier};", 20.0);
            Print(f"[MoveTune] SlideBoostMultiplier = {L.SlideBoostMultiplier}; SlideSpeedCapMultiplier = {L.SlideSpeedCapMultiplier}; SlideBoostArmMultiplier = {L.SlideBoostArmMultiplier};", 20.0);
            Print(f"[MoveTune] SlideEntryMinFraction = {L.SlideEntryMinFraction}; SlideExitSpeed = {L.SlideExitSpeed}; SlideGroundFriction = {L.SlideGroundFriction};", 20.0);
            Print(f"[MoveTune] SlideBraking = {L.SlideBraking}; SlideDownhillAccel = {L.SlideDownhillAccel}; SlideMaxDuration = {L.SlideMaxDuration};", 20.0);
            Print(f"[MoveTune] SlideBoostCooldown = {L.SlideBoostCooldown}; SlideSustainMinSlope = {L.SlideSustainMinSlope};", 20.0);
            return;
        }

        FString P = Param.ToLower();
        bool bSet = true;
        if      (P == "maxacceleration")            L.MaxAcceleration = Value;
        else if (P == "brakingdecelerationwalking") L.BrakingDecelerationWalking = Value;
        else if (P == "groundfriction")             L.GroundFriction = Value;
        else if (P == "brakingfriction")            L.BrakingFriction = Value;
        else if (P == "brakingfrictionfactor")      L.BrakingFrictionFactor = Value;
        else if (P == "gravityscale")               L.GravityScale = Value;
        else if (P == "jumpzvelocity")              L.JumpZVelocity = Value;
        else if (P == "doublejumpzvelocity")        L.DoubleJumpZVelocity = Value;
        else if (P == "jumpmaxholdtime")            L.JumpMaxHoldTime = Value;
        else if (P == "aircontrol")                 L.AirControl = Value;
        else if (P == "fallinglateralfriction")     L.FallingLateralFriction = Value;
        else if (P == "sprintspeedmultiplier")      L.SprintSpeedMultiplier = Value;
        else if (P == "crouchspeed")                L.CrouchSpeed = Value;
        else if (P == "focusmovemultiplier")        L.FocusMoveMultiplier = Value;
        else if (P == "slideboostmultiplier")       L.SlideBoostMultiplier = Value;
        else if (P == "slidespeedcapmultiplier")    L.SlideSpeedCapMultiplier = Value;
        else if (P == "slideboostarmmultiplier")    L.SlideBoostArmMultiplier = Value;
        else if (P == "slideentryminfraction")      L.SlideEntryMinFraction = Value;
        else if (P == "slideexitspeed")             L.SlideExitSpeed = Value;
        else if (P == "slidegroundfriction")        L.SlideGroundFriction = Value;
        else if (P == "slidebraking")               L.SlideBraking = Value;
        else if (P == "slidedownhillaccel")         L.SlideDownhillAccel = Value;
        else if (P == "slidemaxduration")           L.SlideMaxDuration = Value;
        else if (P == "slideboostcooldown")         L.SlideBoostCooldown = Value;
        else if (P == "slidesustainminslope")       L.SlideSustainMinSlope = Value;
        else bSet = false;

        if (!bSet)
        {
            Print(f"[MoveTune] unknown param '{Param}' — run MoveTune with no args for the list", 8.0);
            return;
        }
        L.ApplyMovementConfig();
        Print(f"[MoveTune] {Param} = {Value} (applied)", 6.0);
    }

    // --- Debug: slide-hop rule battery (headless-safe; SmokeTest greps the RESULT line). ---------
    // Asserts the Research-A rules: (1) slide from sprint-speed boosts past sprint, (2) boost-armed
    // entry respects the cap, (3) an over-threshold entry does NOT re-boost (anti-bhop governor).
    private int MoveSmokeRetries = 0;

    UFUNCTION(Exec)
    void MoveSmoke()
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || Hero.Locomotion == nullptr || Hero.CharacterMovement == nullptr)
        {
            if (MoveSmokeRetries < 30)
            {
                MoveSmokeRetries++;
                System::SetTimer(this, n"MoveSmoke", 1.0, false);
                return;
            }
            Print("[MoveSmoke] gave up waiting for hero", 8.0);
            return;
        }
        URogueLocomotionComponent L = Hero.Locomotion;
        UCharacterMovementComponent M = Hero.CharacterMovement;
        FVector Fwd = Hero.GetActorForwardVector();
        int Pass = 0;
        const int Total = 3;

        // 1) Sprint-speed entry boosts to > sprint (and >= boost target * 0.95 for float slack).
        Hero.SetSprint(true);
        M.Velocity = Fwd * L.SprintSpeed();
        Hero.CrouchPressed();
        float S1 = FVector(M.Velocity.X, M.Velocity.Y, 0.0).Size();
        bool bC1 = L.IsSliding() && S1 > L.SprintSpeed() && S1 >= L.SprintSpeed() * L.SlideBoostMultiplier * 0.95;
        if (bC1) Pass++; else Print(f"[MoveSmoke] FAIL 1: entry speed {S1} (sprint {L.SprintSpeed()})", 10.0);
        Hero.CrouchReleased();

        // 2) Cap: an absurd entry speed is clamped to the slide cap.
        M.Velocity = Fwd * (L.SlideCap() * 2.0);
        Hero.CrouchPressed();
        float S2 = FVector(M.Velocity.X, M.Velocity.Y, 0.0).Size();
        bool bC2 = S2 <= L.SlideCap() * 1.01;
        if (bC2) Pass++; else Print(f"[MoveSmoke] FAIL 2: cap leak {S2} > {L.SlideCap()}", 10.0);
        Hero.CrouchReleased();

        // 3) Anti-bhop: above the arming threshold (and inside the boost cooldown from #1/#2),
        //    entering a slide must NOT add speed.
        float Over = L.SprintSpeed() * (L.SlideBoostArmMultiplier + 0.05);
        M.Velocity = Fwd * Over;
        Hero.CrouchPressed();
        float S3 = FVector(M.Velocity.X, M.Velocity.Y, 0.0).Size();
        bool bC3 = S3 <= Over * 1.01;
        if (bC3) Pass++; else Print(f"[MoveSmoke] FAIL 3: boosted over threshold {Over} -> {S3}", 10.0);
        Hero.CrouchReleased();
        Hero.SetSprint(false);

        Print(f"[MoveSmoke] RESULT {Pass}/{Total}", 15.0);
    }
```

- [ ] **Step 2: Compile-verify.** as-helper `run_code_test`. Expected: exit 0. (If reading `L.MaxAcceleration` etc. fails because the fields read as not-blueprint-accessible, change the `UPROPERTY()` specifiers in Task 2's file to `UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, ...)` — AS-to-AS access normally needs nothing, this is only a contingency.)

- [ ] **Step 3: Headless run.** `Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Combat -Exec "MoveSmoke" -Grep "MoveSmoke"`.
Expected: `[MoveSmoke] RESULT 3/3`.

- [ ] **Step 4: Add MoveSmoke to the smoke gate.** Edit `Tools\SmokeTest.ps1`: add `MoveSmoke` to the DL_Combat (RaidArena) case's exec list and assert `\[MoveSmoke\] RESULT 3/3` the same way existing cases assert their RESULT lines (mirror the UpgradeSmoke pattern exactly).

- [ ] **Step 5: Full gate + commit**
Run `Tools\SmokeTest.ps1` — all cases PASS (exit 0). Then:
```
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "feat(movement): MoveTune live-tuning console + MoveSmoke slide-rule battery"
```

---

### Task 4: Camera feel component

**Files:**
- Create: `RogueSmoke/Script/Player/CameraFeelComponent.as`
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as` (remove focus-camera code, hook the component)

- [ ] **Step 1: Write the component** (numbers from Research B; all owning-client cosmetic):

```angelscript
// CameraFeelComponent.as
// Owning-client camera juice (research Report B): landing dip, sprint/slide FOV kick, per-shot
// cosmetic camera kick with spring recovery, and the focus (light-ADS) zoom blend moved here from
// the hero so camera logic has one home. Strictly cosmetic — never touches control rotation, so
// the server-authoritative aim point is unaffected (the Destiny visual-recoil/aim split).
class URogueCameraFeelComponent : UActorComponent
{
    // --- Focus zoom (moved from HeroCharacter) ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Focus")
    float BaseCameraFOV = 90.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Focus")
    float BaseArmLength = 350.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Focus")
    float FocusBlendSpeed = 9.0;

    // --- Speed FOV kicks ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float SprintFOVBonus = 5.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float SlideFOVBonus = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float FOVBlendSpeed = 8.0;

    // --- Per-shot cosmetic kick (spring-recovered; camera-local, aim point untouched) ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickPitch = 0.4;             // degrees up per shot

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickYawRange = 0.15;         // +/- random yaw per shot

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickMaxOffset = 2.5;         // stacked-offset clamp under full-auto

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float KickSpringStiffness = 400.0;     // ~0.1 s recovery

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float KickSpringDamping = 30.0;

    // --- Landing dip ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float LandDipPerFallSpeed = 0.02;      // cm of dip per cm/s of fall speed
    
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float LandDipMax = 22.0;               // cm

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float DipSpringStiffness = 220.0;      // ~0.2 s recovery

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float DipSpringDamping = 22.0;

    // --- Runtime ---
    private AHeroCharacter Hero;
    private USpringArmComponent Boom;
    private UCameraComponent Camera;
    private float FocusAlpha = 0.0;
    private float SpeedFOVBonus = 0.0;
    private float KickPitch = 0.0;
    private float KickPitchVel = 0.0;
    private float KickYaw = 0.0;
    private float KickYawVel = 0.0;
    private float DipZ = 0.0;
    private float DipZVel = 0.0;
    private FVector CameraBaseRelLocation;

    void Initialize(AHeroCharacter InHero, USpringArmComponent InBoom, UCameraComponent InCamera)
    {
        Hero = InHero;
        Boom = InBoom;
        Camera = InCamera;
        if (Camera != nullptr)
            CameraBaseRelLocation = Camera.GetRelativeLocation();
    }

    void NotifyFired()
    {
        KickPitchVel += FireKickPitch / 0.016;   // impulse sized so one frame ~= the kick angle
        KickYawVel += Math::RandRange(-FireKickYawRange, FireKickYawRange) / 0.016;
    }

    void NotifyLanded(float FallSpeed)
    {
        float Dip = Math::Min(FallSpeed * LandDipPerFallSpeed, LandDipMax);
        DipZVel -= Dip / 0.016;
    }

    // Called from the hero Tick, owning client only. Composites every channel onto the camera.
    void TickCameraFeel(float DeltaSeconds)
    {
        if (Hero == nullptr || Boom == nullptr || Camera == nullptr)
            return;

        // Springs (semi-implicit Euler; stable at game framerates).
        KickPitchVel += (-KickSpringStiffness * KickPitch - KickSpringDamping * KickPitchVel) * DeltaSeconds;
        KickPitch = Math::Clamp(KickPitch + KickPitchVel * DeltaSeconds, -FireKickMaxOffset, FireKickMaxOffset);
        KickYawVel += (-KickSpringStiffness * KickYaw - KickSpringDamping * KickYawVel) * DeltaSeconds;
        KickYaw += KickYawVel * DeltaSeconds;
        DipZVel += (-DipSpringStiffness * DipZ - DipSpringDamping * DipZVel) * DeltaSeconds;
        DipZ += DipZVel * DeltaSeconds;

        // Focus zoom (same behavior the hero had; weapon overrides via the cosmetic def).
        float Target = Hero.bFocusing ? 1.0 : 0.0;
        FocusAlpha = Math::FInterpTo(FocusAlpha, Target, DeltaSeconds, FocusBlendSpeed);
        float TargetFOV = 70.0;
        float TargetArm = 220.0;
        URogueWeaponDefinition Def = Hero.GetCosmeticWeaponDef();
        if (Def != nullptr)
        {
            TargetFOV = Def.FocusFOV;
            TargetArm = Def.FocusArmLength;
        }

        // Speed FOV: sprint/slide widen, fast blend; focus zoom wins by composing after it.
        float WantSpeedBonus = 0.0;
        if (Hero.Locomotion != nullptr)
        {
            if (Hero.Locomotion.IsSliding())
                WantSpeedBonus = SlideFOVBonus;
            else if (Hero.Locomotion.IsSprinting())
                WantSpeedBonus = SprintFOVBonus;
        }
        SpeedFOVBonus = Math::FInterpTo(SpeedFOVBonus, WantSpeedBonus, DeltaSeconds, FOVBlendSpeed);

        Camera.FieldOfView = Math::Lerp(BaseCameraFOV + SpeedFOVBonus, TargetFOV, FocusAlpha);
        Boom.TargetArmLength = Math::Lerp(BaseArmLength, TargetArm, FocusAlpha);
        Camera.SetRelativeRotation(FRotator(KickPitch, KickYaw, 0.0));
        Camera.SetRelativeLocation(CameraBaseRelLocation + FVector(0.0, 0.0, DipZ));
    }
}
```

- [ ] **Step 2: Rewire `HeroCharacter.as`:**
  1. Add `default CameraBoom.bEnableCameraLag = false;` (replacing the `= true` line).
  2. Add the component: `UPROPERTY(DefaultComponent) URogueCameraFeelComponent CameraFeel;`
  3. Delete `FocusAlpha`, `BaseCameraFOV`, `BaseArmLength`, `FocusBlendSpeed` and the whole `UpdateFocusCamera` function; in `Tick`, replace the `UpdateFocusCamera(DeltaSeconds)` call with `CameraFeel.TickCameraFeel(DeltaSeconds)`.
  4. In `OnAbilitySystemReady`, after `Locomotion.Initialize(this)`: `CameraFeel.Initialize(this, CameraBoom, FollowCamera);`
  5. In `Multicast_FireFX`, inside the `IsLocallyControlled()` block, add `CameraFeel.NotifyFired();` and **halve the real recoil feel** by leaving `RecoilPitchPerShot` data as-is (designer dial) — the cosmetic kick stacks on top.
  6. In `OnLanded` (from Task 2), add: `if (IsLocallyControlled()) CameraFeel.NotifyLanded(FallSpeed);`
  7. Add the cosmetic-definition helper (clients can't see the server-only `Weapon.Definition`):
```angelscript
    // The weapon definition for cosmetic use on ANY machine: runtime definition where valid
    // (server/host), else the class-default DefaultWeapon (same fallback as WeaponMesh).
    URogueWeaponDefinition GetCosmeticWeaponDef() const
    {
        if (Weapon != nullptr && Weapon.Definition != nullptr)
            return Weapon.Definition;
        return DefaultWeapon;
    }
```

- [ ] **Step 3: Compile-verify.** as-helper `run_code_test` → exit 0.

- [ ] **Step 4: Append camera knobs to MoveTune.** In `RaidPlayerController.as` `MoveTune`, before the `if (!bSet)` line, add a camera block (get `URogueCameraFeelComponent C = Hero.CameraFeel;` at the top next to `L`):
```angelscript
        else if (P == "sprintfovbonus")        C.SprintFOVBonus = Value;
        else if (P == "slidefovbonus")         C.SlideFOVBonus = Value;
        else if (P == "firekickpitch")         C.FireKickPitch = Value;
        else if (P == "firekickyawrange")      C.FireKickYawRange = Value;
        else if (P == "firekickmaxoffset")     C.FireKickMaxOffset = Value;
        else if (P == "kickspringstiffness")   C.KickSpringStiffness = Value;
        else if (P == "kickspringdamping")     C.KickSpringDamping = Value;
        else if (P == "landdipperfallspeed")   C.LandDipPerFallSpeed = Value;
        else if (P == "landdipmax")            C.LandDipMax = Value;
        else if (P == "dipspringstiffness")    C.DipSpringStiffness = Value;
        else if (P == "dipspringdamping")      C.DipSpringDamping = Value;
```
And add matching lines to the dump block. Re-run `run_code_test` → exit 0.

- [ ] **Step 5: Gate + commit.** `Tools\SmokeTest.ps1` → all PASS.
```
git add RogueSmoke/Script/Player/CameraFeelComponent.as RogueSmoke/Script/Player/HeroCharacter.as RogueSmoke/Script/Player/RaidPlayerController.as
git commit -m "feat(camera): camera-feel component - lag off, landing dip, FOV kicks, cosmetic fire kick"
```

- [ ] **Step 6: USER CHECKPOINT — Feel session #1 (movement + camera).** User plays DL_Combat in PIE, drives `MoveTune` (gravity, accel, slide numbers, kick/dip magnitudes), runs `MoveTune dump`, pastes the chosen block back. Update the defaults in `LocomotionComponent.as`/`CameraFeelComponent.as` to the dumped values, `run_code_test`, commit as `tune(movement): bake feel session #1 numbers`.

---

### Task 5: Full hero anim instance

**Files:**
- Rewrite: `RogueSmoke/Script/Player/HeroAnimInstance.as`

- [ ] **Step 1: Full implementation** (variable set per Research C.3; landing detection is self-contained so simulated proxies animate correctly):

```angelscript
// HeroAnimInstance.as
// The hero's anim-graph data source (Lyra's LyraAnimInstance role). Computes every variable the
// layered ABP reads — see docs/guides/ABP_HERO_BUILD_GUIDE.md for the graph that consumes these.
// Works on ALL machines (owner, server, simulated proxies): everything derives from replicated
// state (velocity, BaseAimRotation, movement mode, locomotion component mirrors).
class URogueHeroAnimInstance : UAnimInstance
{
    // --- Locomotion (lower body) ---
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float GroundSpeed = 0.0;

    // Signed angle (-180..180) between velocity and actor facing; drives the strafe blendspace X.
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float Direction = 0.0;

    // Blendspace Y input in AUTHORED units: min(GroundSpeed, JogAuthoredSpeed).
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float StrafeSpeed = 0.0;

    // Anti-foot-slide: GroundSpeed / JogAuthoredSpeed above authored speed, clamped (Research C.3).
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float PlayRate = 1.0;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bShouldMove = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsFalling = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float VerticalVelocity = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsCrouching = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsSliding = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsSprinting = false;

    // --- Aim (upper body) ---
    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimPitch = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimYaw = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimOffsetAlpha = 1.0;

    // --- Landing recovery (additive alpha, decays after touchdown scaled by fall speed) ---
    UPROPERTY(BlueprintReadOnly, Category = "Landing")
    float LandRecoveryAlpha = 0.0;

    // Authored root speed of the jog row; measure once in Task 8 and correct here if needed.
    UPROPERTY(EditDefaultsOnly, Category = "Tuning")
    float JogAuthoredSpeed = 450.0;

    UPROPERTY(EditDefaultsOnly, Category = "Tuning")
    float LandRecoverySeconds = 0.4;

    private bool bWasFalling = false;
    private float PrevFallSpeed = 0.0;

    UFUNCTION(BlueprintOverride)
    void BlueprintUpdateAnimation(float DeltaTimeX)
    {
        AHeroCharacter Hero = Cast<AHeroCharacter>(TryGetPawnOwner());
        if (Hero == nullptr || Hero.CharacterMovement == nullptr)
            return;
        UCharacterMovementComponent Move = Hero.CharacterMovement;

        FVector Vel = Hero.GetVelocity();
        FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
        GroundSpeed = Flat.Size();
        VerticalVelocity = Vel.Z;
        bIsFalling = Move.IsFalling();
        bIsCrouching = Move.IsCrouching();
        bIsSliding = Hero.Locomotion != nullptr && Hero.Locomotion.IsSliding();
        bIsSprinting = Hero.Locomotion != nullptr && Hero.Locomotion.IsSprinting();

        // Acceleration check prevents the idle-pop while decelerating to a stop (Research C.3).
        bShouldMove = GroundSpeed > 3.0 && Move.GetCurrentAcceleration().SizeSquared() > 0.0;

        // Strafe direction: velocity in actor space (manual — no CalculateDirection binding bet).
        FRotator ActorRot = Hero.GetActorRotation();
        FVector Local = ActorRot.UnrotateVector(Flat);
        Direction = (GroundSpeed > 3.0) ? Math::RadiansToDegrees(Math::Atan2(Local.Y, Local.X)) : 0.0;

        StrafeSpeed = Math::Min(GroundSpeed, JogAuthoredSpeed);
        PlayRate = (GroundSpeed > JogAuthoredSpeed)
            ? Math::Clamp(GroundSpeed / JogAuthoredSpeed, 0.8, 1.35) : 1.0;

        // Aim deltas from BaseAimRotation: replicated (compressed) to simulated proxies, unlike
        // ControlRotation — this is what makes remote players' torsos track their crosshair.
        AimPitch = Math::Clamp(NormalizeAngle(Hero.GetBaseAimRotation().Pitch - ActorRot.Pitch), -90.0, 90.0);
        AimYaw = Math::Clamp(NormalizeAngle(Hero.GetBaseAimRotation().Yaw - ActorRot.Yaw), -90.0, 90.0);
        AimOffsetAlpha = Hero.IsIncapacitated() ? 0.0 : 1.0;

        // Self-contained landing detection (works on sim proxies, no event needed): falling last
        // frame + grounded now = landed; alpha scales with how hard we hit.
        if (bWasFalling && !bIsFalling)
            LandRecoveryAlpha = Math::Clamp(PrevFallSpeed / 750.0, 0.15, 1.0);
        else if (LandRecoveryAlpha > 0.0)
            LandRecoveryAlpha = Math::Max(0.0, LandRecoveryAlpha - DeltaTimeX / LandRecoverySeconds);
        bWasFalling = bIsFalling;
        if (bIsFalling)
            PrevFallSpeed = Math::Abs(Vel.Z);
    }

    private float NormalizeAngle(float Angle) const
    {
        float A = Angle;
        while (A > 180.0)  A -= 360.0;
        while (A < -180.0) A += 360.0;
        return A;
    }
}
```

- [ ] **Step 2: Compile-verify.** as-helper `run_code_test` → exit 0. (`GetCurrentAcceleration`, `GetBaseAimRotation`, `IsCrouching` — if any binding is missing, find the AS name with as-helper `find_binding` before substituting.)

- [ ] **Step 3: Commit**
```
git add RogueSmoke/Script/Player/HeroAnimInstance.as
git commit -m "feat(anim): full hero anim instance - strafe/aim/slide/landing graph inputs"
```

---

### Task 6: GASP sequence migration + retarget bake  *(main session; editor closed for saves)*

Migrate the needed UEFN-skeleton clips into the project, bake them onto our Manny via the proven pipeline (memory: `retarget-bake-via-python`), keep only the baked output.

**Files:**
- Staging (temporary): `RogueSmoke/Content/Characters/UEFN_Mannequin/**` (copied from `F:\UnrealSamples\GameAnimationSample\Content\Characters\UEFN_Mannequin\`)
- Output (committed): `RogueSmoke/Content/Characters/Mannequins/Anims/GASP/*.uasset`

- [ ] **Step 1: Confirm preconditions.** `editor_sessions_list` — no interactive editor running (commandlet saves need exclusive access). Verify `Slide_KneesOut_Loop.uasset` exists in our Anims (prior bake proof) and locate the rig/retargeter assets used last time: search our Content + the GASP project for `IK_*`/`RTG_*` assets (GASP's `Content\Characters\UE5_Mannequins\` + `Rigs\` folders normally carry `RTG_UEFN_to_UE5`-style retargeters — record exact names).

- [ ] **Step 2: Stage the source clips.** File-copy from GASP, PRESERVING relative Content paths (soft references resolve by path): `Characters\UEFN_Mannequin\Meshes\`, `Characters\UEFN_Mannequin\Rigs\`, and from `Characters\UEFN_Mannequin\Animations\`: the `Slide\`, `Jump\`, `Sprint\` folders. (Whole-folder copies are fine; staging is deleted after the bake.)

- [ ] **Step 3: Bake.** Headless commandlet python (pattern from the enemy retarget work; `unreal.log` + `-abslog` for output): `IKRetargetBatchOperation.duplicate_and_retarget` with source = the staged UEFN sequences, target = our `SKM_Manny_Simple`/`SK_Mannequin`, output path `/Game/Characters/Mannequins/Anims/GASP/`, name prefix none + explicit renames to project convention. Bake list (minimum):
  - `M_Neutral_Slide_FootOut_Into_Lfoot` → `Slide_Enter`
  - `M_Neutral_Slide_FootOut_Loop` → `Slide_Loop` (alongside the existing KneesOut loop — pick the better-looking one in Task 8)
  - `M_Neutral_Slide_FootOut_Out_Moving_Run` → `Slide_Exit_Run`
  - `M_Neutral_Jump_F_Land_Run_Light_Lfoot` → `Land_Run_Light`
  - `M_Neutral_Jump_F_Land_Run_Heavy_Lfoot` → `Land_Run_Heavy`
  - `M_Neutral_Jump_F_Land_Stand_Light_Lfoot` → `Land_Stand_Light`
  - `M_Neutral_Jump_Loop_Fall` → `Fall_Loop`
  - `M_Neutral_Sprint_Loop_F` → `Sprint_Loop`
  - `M_Neutral_Sprint_Stop_F_Lfoot` → `Sprint_Stop`

- [ ] **Step 4: Verify + clean.** Commandlet python: load each baked asset, assert its Skeleton == our `SK_Mannequin`. Delete the staged `Content/Characters/UEFN_Mannequin/` folder. Boot sanity: `Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Combat -Grep "error"` → clean.

- [ ] **Step 5: Commit** (baked assets only; staging must be gone)
```
git add RogueSmoke/Content/Characters/Mannequins/Anims/GASP
git commit -m "content(anim): bake GASP slide/land/sprint clips onto Manny (retarget pipeline)"
```

---

### Task 7: Fire/reload montages + the ABP build guide

**Files:**
- Create (editor): `/Game/Characters/Mannequins/Anims/Rifle/MTG_Rifle_Fire`, `MTG_Rifle_Reload`
- Create (doc): `docs/guides/ABP_HERO_BUILD_GUIDE.md`

- [ ] **Step 1: Create montages.** With the editor session up (`editor_session_start` `-skipcompile`), use the `anim_create_montage` MCP tool (or `python_exec` fallback: `unreal.AnimMontageFactory` + set the slot) to create `MTG_Rifle_Fire` from `MM_Rifle_Fire` and `MTG_Rifle_Reload` from `MM_Rifle_Reload`, **slot `DefaultGroup.UpperBody`** (the slot is registered on the skeleton in the GUI build — if slot assignment fails before Task 8, leave montages on DefaultSlot and re-slot them in Task 8's GUI pass; note it in the task report). Save both.

- [ ] **Step 2: Author the build guide.** Write `docs/guides/ABP_HERO_BUILD_GUIDE.md` by EXPANDING Research C.3 into a click-by-click checklist a non-animator can follow. Required content (all of it, no summarizing):
  1. Create `BS_Rifle_Strafe` (BlendSpace, skeleton `SK_Mannequin`): axis setup (Direction -180..180 grid 8 **Wrap Input ON**, Speed 0..450), the 9-sample placement table (Bwd at BOTH ±180) using the `MF_Rifle_Jog_*` + `MF_Rifle_Walk_*` sets, smoothing 0.1–0.15 Averaged.
  2. Anim Slot Manager: add `UpperBody` slot.
  3. Create `ABP_Hero` (Animation Blueprint, skeleton `SK_Mannequin`) → Class Settings → reparent to `URogueHeroAnimInstance`. Every graph variable comes from the parent class — bind pins via property access (no BP-side computation).
  4. The locomotion state machine (states, clips incl. the Task-6 GASP bakes for Slide/Land, transition conditions + blend times) exactly per Research C.3 — including the `ToAirborne` conduit, sync group `Locomotion` assignments, and the BS player pins (Direction→`Direction`, Speed→`StrafeSpeed`, PlayRate→`PlayRate`).
  5. The post-SM spine, node by node with pin wiring: Save/Use Cached Pose `LocomotionCache` → Layered blend per bone (`spine_01`, depth 4, Mesh Space Rotation Blend ON; UpperBody slot on Blend Poses 0) → AimOffset `AO_Rifle` (Pitch=`AimPitch`, Yaw=`AimYaw`, Alpha=`AimOffsetAlpha`) → Apply Additive (`MM_Rifle_Jump_RecoveryAdditive` × `LandRecoveryAlpha`) → Slot `DefaultSlot` → Output.
  6. Verify `AO_Rifle`'s additive settings (Mesh Space) and how to fix if not.
  7. A final checklist: compile ABP, assign preview mesh `SKM_Manny_Simple`, scrub each variable in the anim preview.

- [ ] **Step 3: Commit**
```
git add docs/guides/ABP_HERO_BUILD_GUIDE.md
git commit -m "docs(anim): click-by-click ABP_Hero + strafe blendspace build guide"
```
(Montage assets get committed in Task 8 with the rest of the anim content.)

---

### Task 8: USER builds the ABP; hook it to the heroes  *(USER + main session)*

- [ ] **Step 1: USER follows `docs/guides/ABP_HERO_BUILD_GUIDE.md`** in the editor GUI (session can stay up; answer questions as they come). Output assets: `/Game/Characters/Mannequins/Anims/BS_Rifle_Strafe`, `/Game/Characters/Mannequins/Anims/ABP_Hero`.

- [ ] **Step 2: Measure authored jog speed.** In the editor, open `MF_Rifle_Jog_Fwd`, read root motion distance/duration (or eyeball via preview speed). If it differs from 450 by >10%, update `JogAuthoredSpeed` in `HeroAnimInstance.as` and the BS speed-axis max to match; `run_code_test`.

- [ ] **Step 3: Assign to heroes.** MCP `python_exec`: for `BP_Vanguard` + `BP_Bombardier`, set the mesh component's `anim_class` to `ABP_Hero_C`, then **`compile_blueprint` + save** (memory: `mcp-bp-cdo-needs-compile` — skipping the compile means PIE wipes it). Verify with `blueprint_outline` that the parent/mesh settings stuck.

- [ ] **Step 4: PIE verification (2-player listen server).** Checks, in order: no T-pose; legs strafe in 8 directions while the torso stays on the crosshair; aim pitch tracks on BOTH the local player and the remote proxy (view from the second client); slide plays the slide pose; jump→fall→land sequence with visible land recovery; no foot-sliding at walk and sprint (if sliding at sprint: confirm PlayRate pin is wired).

- [ ] **Step 5: SmokeTest + commit** (`Tools\SmokeTest.ps1` first):
```
git add RogueSmoke/Content/Characters/Mannequins/Anims RogueSmoke/Content/Blueprints/BP_Vanguard.uasset RogueSmoke/Content/Blueprints/BP_Bombardier.uasset
git commit -m "content(anim): layered ABP_Hero + strafe blendspace wired to heroes (linked-body fix)"
```

---

### Task 9: Fire/reload montage wiring

**Files:**
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as`

- [ ] **Step 1: Add montage fields + reload edge detection + multicast.** In `AHeroCharacter`:

```angelscript
    // Upper-body feedback montages (assigned on the hero BPs; UpperBody slot in ABP_Hero).
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage FireMontage;

    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage ReloadMontage;

    // Server-only edge detector: WeaponComponent starts reloads internally (auto-reload on empty),
    // so the hero polls the transition in Tick rather than hooking every call site.
    private bool bWasReloading = false;
```

In `Tick`, inside the `HasAuthority()` + `Weapon != nullptr` block, after `Weapon.TickWeapon(DeltaSeconds)`:
```angelscript
                // Reload started this frame (manual or auto): cosmetic montage everywhere.
                bool bReloadingNow = Weapon.IsReloading();
                if (bReloadingNow && !bWasReloading)
                    Multicast_ReloadFX();
                bWasReloading = bReloadingNow;
```

New multicast + a montage helper (placed near `Multicast_FireFX`):
```angelscript
    // Cosmetic, fire-and-forget: reload montage on the upper body on all machines.
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_ReloadFX()
    {
        PlayUpperBodyMontage(ReloadMontage);
    }

    private void PlayUpperBodyMontage(UAnimMontage Montage)
    {
        if (Montage == nullptr || Mesh == nullptr)
            return;
        UAnimInstance AnimInst = Mesh.GetAnimInstance();
        if (AnimInst != nullptr)
            AnimInst.Montage_Play(Montage);
    }
```
And in `Multicast_FireFX`, first line of the body: `PlayUpperBodyMontage(FireMontage);`
(If `Montage_Play` isn't the AS binding name, resolve with as-helper `find_binding UAnimInstance::Montage_Play` — do not guess.)

- [ ] **Step 2: Compile-verify.** `run_code_test` → exit 0.

- [ ] **Step 3: Assign montages on hero BPs.** MCP `python_exec`: set `FireMontage`/`ReloadMontage` class defaults on `BP_Vanguard`/`BP_Bombardier` to `MTG_Rifle_Fire`/`MTG_Rifle_Reload` + `compile_blueprint` + save.

- [ ] **Step 4: PIE check.** Fire: upper body plays the shot anim while strafing legs continue; reload (R / empty mag): reload montage on both players' views. SmokeTest → PASS.

- [ ] **Step 5: Commit**
```
git add RogueSmoke/Script/Player/HeroCharacter.as RogueSmoke/Content/Blueprints/BP_Vanguard.uasset RogueSmoke/Content/Blueprints/BP_Bombardier.uasset
git commit -m "feat(anim): fire/reload upper-body montages wired through cosmetic multicasts"
```

---

### Task 10: Damage numbers

**Files:**
- Modify: `RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as`
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as`
- Modify: `RogueSmoke/Script/UI/RogueHUDWidget.as`

- [ ] **Step 1: Server → owning-client hit events.** In `GA_WeaponFire.FireOneCartridge`, extend the pellet loop to collect damage hits, and send them after `Weapon.NotifyFired()`:

```angelscript
        TArray<FVector> Impacts;
        TArray<FVector> DamageLocs;
        TArray<float> DamageAmounts;
        bool bHitEnemy = false;
        for (int i = 0; i < Def.BulletsPerCartridge; i++)
        {
            FVector Dir = (HalfAngleRad > 0.0) ? Math::VRandCone(BaseDir, HalfAngleRad) : BaseDir;
            FVector End = MuzzleLoc + Dir * Def.MaxRange;
            FHitscanResult Result = Combat.FireWeaponShot(MuzzleLoc, End, Shot, Avatar);
            Impacts.Add(Result.ImpactPoint);
            if (Result.bHitEnemy)
            {
                bHitEnemy = true;
                if (Result.DamageDealt > 0.0)
                {
                    DamageLocs.Add(Result.ImpactPoint);
                    DamageAmounts.Add(Result.DamageDealt);
                }
            }
        }

        Weapon.NotifyFired();
        Hero.Multicast_FireFX(MuzzleLoc, Impacts, bHitEnemy);
        if (DamageLocs.Num() > 0)
            Hero.Client_DamageNumbers(DamageLocs, DamageAmounts);
```

- [ ] **Step 2: Hero-side event buffer.** In `AHeroCharacter` (script struct + RPC + accessor):

```angelscript
    // One floating damage number waiting for the HUD to spawn it (owning client only).
    private TArray<FVector> PendingDamageLocs;
    private TArray<float> PendingDamageAmounts;

    // Cosmetic, per-hit damage feedback to the shooter only. Unreliable: a lost number is noise.
    UFUNCTION(Client, Unreliable)
    void Client_DamageNumbers(TArray<FVector> Locations, TArray<float> Amounts)
    {
        int Count = Math::Min(Locations.Num(), Amounts.Num());
        for (int i = 0; i < Count; i++)
        {
            PendingDamageLocs.Add(Locations[i]);
            PendingDamageAmounts.Add(Amounts[i]);
        }
    }

    // The HUD drains the buffer once per frame.
    void TakePendingDamageNumbers(TArray<FVector>& OutLocs, TArray<float>& OutAmounts)
    {
        OutLocs = PendingDamageLocs;
        OutAmounts = PendingDamageAmounts;
        PendingDamageLocs.Empty();
        PendingDamageAmounts.Empty();
    }
```

- [ ] **Step 3: HUD pooled numbers.** In `RogueHUDWidget.as`, add a pooled damage-number layer (mirror the edge-indicator pool's style; Research B parameters):

```angelscript
    // --- Floating damage numbers (Research B: pooled, world-anchored, priority-evicted). ---
    const int DamageNumberPoolSize = 32;
    const float DamageNumberLife = 0.7;        // seconds
    const float DamageNumberRise = 90.0;       // cm of world-space rise over the lifetime
    private TArray<UTextBlock> DamageNumberPool;
    private TArray<FVector> DamageNumberWorld;  // world anchor per pool slot
    private TArray<float> DamageNumberBorn;     // spawn time; <0 = slot free
    private TArray<float> DamageNumberValue;
```

Build the pool in the existing widget-construction path (same `ConstructWidget`/`AddChild` pattern as `HitMarker`, anchors (0,0), `Collapsed`, font via `UITheme` defaults), then add to the per-frame refresh chain:

```angelscript
    private void RefreshDamageNumbers()
    {
        if (Hero == nullptr)
            return;
        float Now = Gameplay::GetTimeSeconds();

        // Spawn: take this frame's hits; evict the OLDEST live slot when the pool is full
        // (priority eviction, Warframe-style — new info beats stale info).
        TArray<FVector> NewLocs;
        TArray<float> NewAmounts;
        Hero.TakePendingDamageNumbers(NewLocs, NewAmounts);
        for (int n = 0; n < NewLocs.Num(); n++)
        {
            int Slot = -1;
            float OldestBorn = 1.0e18;
            for (int i = 0; i < DamageNumberPoolSize; i++)
            {
                if (DamageNumberBorn[i] < 0.0) { Slot = i; break; }
                if (DamageNumberBorn[i] < OldestBorn) { OldestBorn = DamageNumberBorn[i]; Slot = i; }
            }
            // Slight XY jitter so stacked hits on one enemy stay readable.
            DamageNumberWorld[Slot] = NewLocs[n]
                + FVector(Math::RandRange(-18.0, 18.0), Math::RandRange(-18.0, 18.0), 0.0);
            DamageNumberBorn[Slot] = Now;
            DamageNumberValue[Slot] = NewAmounts[n];
            UTextBlock T = DamageNumberPool[Slot];
            int Shown = Math::RoundToInt(NewAmounts[n]);
            FString Text = Shown >= 10000 ? f"{float(Shown) / 1000.0:.1f}k" : f"{Shown}";
            T.SetText(FText::FromString(Text));
        }

        // Animate: project, rise, fade, expire.
        APlayerController PC = GetOwningPlayer();
        for (int i = 0; i < DamageNumberPoolSize; i++)
        {
            UTextBlock T = DamageNumberPool[i];
            if (DamageNumberBorn[i] < 0.0)
                continue;
            float Age = Now - DamageNumberBorn[i];
            if (Age >= DamageNumberLife)
            {
                DamageNumberBorn[i] = -1.0;
                T.SetVisibility(ESlateVisibility::Collapsed);
                continue;
            }
            FVector World = DamageNumberWorld[i] + FVector(0.0, 0.0, DamageNumberRise * (Age / DamageNumberLife));
            FVector2D ScreenPos;
            if (!WidgetLayout::ProjectWorldLocationToWidgetPosition(PC, World, ScreenPos, false))
            {
                T.SetVisibility(ESlateVisibility::Collapsed);
                continue;
            }
            T.SetVisibility(ESlateVisibility::HitTestInvisible);
            T.SetRenderOpacity(1.0 - Math::Pow(Age / DamageNumberLife, 2.0));   // hold, then fade
            UCanvasPanelSlot Slot = WidgetLayout::SlotAsCanvasSlot(T);
            if (Slot != nullptr)
                Slot.SetPosition(ScreenPos);
        }
    }
```
Wire `RefreshDamageNumbers()` into the same per-frame refresh list as `RefreshHitMarker()`, and initialize the four arrays (pool widgets + `DamageNumberBorn[i] = -1.0`) where the other widgets are built.

- [ ] **Step 4: Compile + verify.** `run_code_test` → exit 0. PIE on `DL_Upgrades` (target dummies): shoot the SOLO dummy — numbers rise and fade at the impact point; hold full-auto on the CLUSTER formation — readable, no hitches, pool caps at 32. Numbers appear ONLY on the shooting player's screen (verify from client 2).

- [ ] **Step 5: SmokeTest + commit**
```
git add RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as RogueSmoke/Script/Player/HeroCharacter.as RogueSmoke/Script/UI/RogueHUDWidget.as
git commit -m "feat(ui): pooled floating damage numbers (owning-client, priority eviction)"
```

---

### Task 11: Weapon FX/audio slots + FireFX payload extension

All asset slots are OPTIONAL: null slot = current debug-draw/silent behavior, so this lands before any assets exist. Lyra-content migration (user download) and/or sound-pack drops then become pure designer assignment, no code.

**Files:**
- Modify: `RogueSmoke/Script/Weapons/WeaponDefinition.as`
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as` (`Multicast_FireFX`)
- Modify: `RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as` (payload)

- [ ] **Step 1: Add the slots to `URogueWeaponDefinition`:**

```angelscript
    // --- Feel: VFX (all optional; null = debug-line fallback) ---
    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem MuzzleFlashFX;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem TracerFX;           // expects a user vector param "TracerEnd"

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem ImpactWorldFX;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem ImpactEnemyFX;

    // --- Feel: audio (all optional; null = silent) ---
    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase FireSound;              // short per-shot (transient+body); 4-6 round-robins inside the cue

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase FireTailSound;          // played once on trigger release (Research B: the tail sells power)

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase ReloadSound;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase HitTickSound;           // owning client, confirmed hit

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase KillConfirmSound;       // owning client, killing blow (Task 12)
```

- [ ] **Step 2: Extend the FireFX payload with per-impact surface info.** Change the multicast signature (caller updated in step 3):

```angelscript
    // Cosmetic, fire-and-forget: per-shot FX on all machines + recoil/hit feedback on the owner.
    // ImpactIsEnemy parallels Impacts (pellet counts are tiny — shotgun max ~8).
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireFX(FVector MuzzleLocation, TArray<FVector> Impacts, TArray<bool> ImpactIsEnemy, bool bHitEnemy)
    {
        PlayUpperBodyMontage(FireMontage);

        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        for (int i = 0; i < Impacts.Num(); i++)
        {
            FVector Impact = Impacts[i];
            if (Def != nullptr && Def.TracerFX != nullptr)
            {
                UNiagaraComponent Tracer = Niagara::SpawnSystemAtLocation(Def.TracerFX, MuzzleLocation,
                    (Impact - MuzzleLocation).Rotation());
                if (Tracer != nullptr)
                    Tracer.SetVectorParameter(n"TracerEnd", Impact);
            }
            else
            {
                System::DrawDebugLine(MuzzleLocation, Impact, FLinearColor(1.0, 1.0, 0.0), 0.05, 2.0);
            }

            bool bEnemy = i < ImpactIsEnemy.Num() && ImpactIsEnemy[i];
            UNiagaraSystem ImpactFX = Def != nullptr ? (bEnemy ? Def.ImpactEnemyFX : Def.ImpactWorldFX) : nullptr;
            if (ImpactFX != nullptr)
                Niagara::SpawnSystemAtLocation(ImpactFX, Impact);
        }

        if (Def != nullptr && Def.MuzzleFlashFX != nullptr)
            Niagara::SpawnSystemAttached(Def.MuzzleFlashFX, WeaponMesh, Def.MuzzleSocket,
                FVector(), FRotator(), EAttachLocation::SnapToTarget, true);
        if (Def != nullptr && Def.FireSound != nullptr)
            Gameplay::SpawnSoundAtLocation(Def.FireSound, MuzzleLocation);

        if (IsLocallyControlled())
        {
            CameraFeel.NotifyFired();
            if (Weapon != nullptr && Weapon.Definition != nullptr)
            {
                AddControllerPitchInput(-Weapon.Definition.RecoilPitchPerShot);
                AddControllerYawInput(Math::RandRange(-Weapon.Definition.RecoilYawRange, Weapon.Definition.RecoilYawRange));
            }
            if (bHitEnemy)
            {
                LastHitConfirmTime = Gameplay::GetTimeSeconds();
                if (Def != nullptr && Def.HitTickSound != nullptr)
                    Gameplay::SpawnSound2D(Def.HitTickSound);
            }
        }
    }
```
Also: in `Multicast_ReloadFX` (Task 9), add the reload sound after the montage:
```angelscript
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && Def.ReloadSound != nullptr)
            Gameplay::SpawnSoundAtLocation(Def.ReloadSound, GetActorLocation());
```
Binding caution: verify `Niagara::SpawnSystemAtLocation` / `SpawnSystemAttached` / `Gameplay::SpawnSoundAtLocation` / `SpawnSound2D` AS names with as-helper `find_binding` (UNiagaraFunctionLibrary / UGameplayStatics — WorldContext params are dropped by the fork). `UNiagaraSystem`/`UNiagaraComponent` types must resolve; if the Niagara plugin isn't script-exposed, report — don't hack around it.

- [ ] **Step 3: Update the caller.** In `GA_WeaponFire.FireOneCartridge`, build `TArray<bool> ImpactIsEnemy;` in the pellet loop (`ImpactIsEnemy.Add(Result.bHitEnemy);`) and call `Hero.Multicast_FireFX(MuzzleLoc, Impacts, ImpactIsEnemy, bHitEnemy);`.

- [ ] **Step 4: Compile + gate.** `run_code_test` → exit 0; `Tools\SmokeTest.ps1` → PASS (WeaponSmoke exercises the fire path). PIE: firing still draws debug lines (null slots), no errors.

- [ ] **Step 5: Commit**
```
git add RogueSmoke/Script/Weapons/WeaponDefinition.as RogueSmoke/Script/Player/HeroCharacter.as RogueSmoke/Script/Weapons/Abilities/GA_WeaponFire.as
git commit -m "feat(weapons): optional VFX/audio slots + surface-aware FireFX payload"
```

- [ ] **Step 6: USER ASSET DROP (parallel, non-blocking).** Hand the user the acquisition list; wiring already works, so this is assignment-only when assets arrive (a later session or Task 13):
  - **Lyra content** via Epic Games Launcher ("Lyra Starter Game", any 5.x slot) → migrate `NS_WeaponFire`-family Niagara + `sfx_Weapons_*` cues into `/Game/Effects/Weapons/` + `/Game/Audio/Weapons/`.
  - Failing that: free packs — Sonniss GDC bundles (sonniss.com/gameaudiogdc), freesound.org CC0 gunshots; any rifle shot + tail + click works for v1.
  - Assign on `DA_Weapon_*` assets (the `Feel|VFX` / `Feel|Audio` categories).

---

### Task 12: Kill confirm + fire-stop tail

**Files:**
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as`
- Modify: server death path (exact file determined in Step 1 — likely `Source/RogueSmoke` C++ or `Script/Core/RaidGameMode.as`)

- [ ] **Step 1: Find the killing-blow attribution.** Investigate with ue-cpp (`grab_function USpawnDirector::OnEnemyKilled` / `UHealthComponent` death broadcast) and Grep `Script/Core/RaidGameMode.as` for the kill-XP path: find where a kill knows its instigator. Expected: `UHealthComponent::ApplyDamage(Damage, Instigator)` records the instigator and the death event carries it. Write down the actual seam in the task report.

- [ ] **Step 2: Route the confirm.** At the server-side kill site (where XP is awarded), if the instigator is an `AHeroCharacter`, call a new RPC on it:

```angelscript
    // Cosmetic kill confirmation to the killing player only (Research B: third distinct sound,
    // the loudest of the three confirm layers).
    UFUNCTION(Client, Unreliable)
    void Client_KillConfirm()
    {
        LastKillConfirmTime = Gameplay::GetTimeSeconds();
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && Def.KillConfirmSound != nullptr)
            Gameplay::SpawnSound2D(Def.KillConfirmSound);
    }

    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    float LastKillConfirmTime = -100.0;
```
(If the death path is C++-only with no instigator exposed, the minimal change is exposing the instigator on the existing kill broadcast — prefer the smallest C++ touch, then `ue-cpp build` with `bounce_editor=true` if an editor is up.)

- [ ] **Step 3: Fire-stop tail.** In `Server_SetWantsToFire`, detect the stop edge and multicast:
```angelscript
        bool bWas = bWantsToFire;
        bWantsToFire = bWants;
        if (bWas && !bWants)
            Multicast_FireStopped();
        if (bWants)
            ActivateGrantedAbility(FireInputTag);
```
```angelscript
    // Cosmetic: the gun tail rings out once when the trigger releases (full-auto fatigue fix).
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireStopped()
    {
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && Def.FireTailSound != nullptr)
            Gameplay::SpawnSoundAtLocation(Def.FireTailSound, GetMuzzleFallbackLocation());
    }
```
(`GetMuzzleFallbackLocation()` = the existing `GetMuzzleLocation()` is server-only safe; on clients use `WeaponMesh.GetSocketLocation(...)` when the mesh exists else actor location — add a small client-safe variant.)

- [ ] **Step 4: HUD kill marker variant.** In `RogueHUDWidget`, extend `RefreshHitMarker`: if `Hero.LastKillConfirmTime` is within 0.25 s, render the marker scaled 2.0 (kill pop) instead of 1.4.

- [ ] **Step 5: Compile + gate + commit.** `run_code_test` → exit 0; `Tools\SmokeTest.ps1` → PASS; PIE: killing a dummy pops the marker (sound slots may still be null).
```
git add RogueSmoke/Script RogueSmoke/Source
git commit -m "feat(feel): kill confirm to the killer + fire-stop tail hook"
```

---

### Task 13: Final verification, feel sessions, defaults bake, docs

- [ ] **Step 1: Full regression.** `Tools\SmokeTest.ps1` → ALL PASS (now including MoveSmoke).

- [ ] **Step 2: 2-player listen-server PIE checklist** (user hosts, session observes via MCP where useful):
  - Movement: slide-hop chain (sprint→slide→jump→land→slide) carries speed on host AND remote client; double jump caps at 2; no rubber-banding on the remote during slides.
  - Anim: remote proxy strafes/aims/fires/reloads correctly (BaseAimRotation pitch visible).
  - Shooting: damage numbers only on the shooter; hitmarker/kill pop only on the shooter; tracers/montages on both.
  - Telegraph interaction: jump arc change (gravity 1.8) — confirm the Lunger lunge and Bloater blast are still dodgeable by slide/jump; if not, flag enemy timing for a follow-up balance pass (do NOT retune enemies in this plan).

- [ ] **Step 3: USER feel session #2 (full stack).** DL_Combat + DL_Upgrades with everything on; MoveTune any remaining knobs; `MoveTune dump` → bake final defaults into the two components; `run_code_test`; commit `tune(feel): bake feel session #2 numbers`.

- [ ] **Step 4: Docs.**
  - `DECISIONS.md`: add **D-0021 — Movement/shooting feel pass**: chosen numbers + slide-hop rule set + the cosmetic/authoritative recoil split + layered-ABP architecture (AS anim instance + GUI graph) + MoveTune; supersedes the D-0015 slide tunables list.
  - `GLOSSARY.md`: add **Slide-hop** (jumping out of a slide preserving momentum; boost-arming threshold), **MoveTune**, **Anim instance** (URogueHeroAnimInstance role), update **Slide** entry numbers.
  - `startup.md` §5: movement/shooting feel state + new exec names (`MoveTune`, `MoveSmoke`); note ABP_Hero replaces the template ABP_TP_Rifle.
  - `CLAUDE.md`: no change expected (house rules untouched).

- [ ] **Step 5: Final commit + branch decision.**
```
git add DECISIONS.md GLOSSARY.md startup.md docs/superpowers/plans/2026-06-11-movement-shooting-feel.md
git commit -m "docs(D-0021): movement+shooting feel pass - decisions, glossary, startup state"
```
Then invoke superpowers:finishing-a-development-branch (we are on `main` per repo practice — verify nothing is left unstaged except the two protected files).

---

## Self-review notes

- **Spec coverage:** §2.1 → Tasks 2–3; §2.2 → Task 4; §2.3 → Tasks 1, 5–9; §2.4.1 → Task 11; §2.4.2 → Task 10; §2.4.3 → Task 4; §2.4.4 → Tasks 11–12 + asset drop; §2.5 → execution rules + Task 13. Variable jump height: research reversed the spec's 0.12 s suggestion → shipped as a tunable defaulting to 0 (user A/Bs it in feel session #1).
- **Known deliberate deferrals (match spec):** turn-in-place, DoT/chain damage-number itemization (seam doesn't expose per-extra-enemy damage — `ExtraEnemiesHit` is a count), crit styling (no crit stat exists), footstep audio wiring (foley notify receiver needs asset-side inspection — folded into the asset-drop follow-up), Niagara authoring (slots null-safe until assets exist), hitstop.
- **Binding-name risk is concentrated** in Tasks 1, 2, 9, 11 — each has an explicit "verify with describe_type/find_binding, don't guess" step.
