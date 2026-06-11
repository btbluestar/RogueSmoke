# Lyra Animation Stack Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the v1 hand-built anim stack with Lyra's production animation architecture (linked anim layers, distance matching, turn-in-place), plus Lyra gun models, weapon audio/VFX, ContextEffects footsteps, heat-based recoil, and a Deadlock-pip stamina system.

**Architecture:** Direct file copy of Lyra 5.7 content preserving `/Game` paths; `ABP_Mannequin_Base` re-parented onto our AngelScript `URogueHeroAnimInstance` via CoreRedirects (Phase-1 spike gates this); slide grafted as a montage driven by our locomotion events (no anim-graph surgery); ContextEffects is the only C++ port; stamina is a new GAS AttributeSet. Spec: `docs/superpowers/specs/2026-06-12-lyra-anim-migration-design.md`.

**Tech Stack:** UE 5.7 AngelScript fork (`F:\UEAS`), AngelScript + C++ (RogueSmoke module), MCP editor tools (`unreal-test-mcp`, `ue-cpp`, `as-helper`), python asset automation.

---

## Execution rules (read first)

- **ONE editor.** Every build / `run_code_test` / PIE / python step runs serialized through the main session. Subagents may only author `.as`/`.cpp`/doc files.
- **No worktrees.** Branch in place: `git checkout -b lyra-anim-migration`.
- **Never stage** `RogueSmoke/Content/Blueprints/GE_Upgrade_ChainDetonation.uasset`, `SUPERPOWERS_HANDOFF.md`, `RogueSmoke/Content/Levels/Prototyping/Test_1.umap`, or `RogueSmoke/Content/Characters/Mannequins/Anims/ABP_Hero.uasset` unless the task explicitly says so (other workstreams / user's local edits).
- **SmokeTest (`Tools\SmokeTest.ps1`, 9/9 green) before every commit.**
- **Editor restarts are required** after: ini CoreRedirects edits, structural AS changes (new UPROPERTY/UFUNCTION), and bulk content copies (asset registry rescan). Value-only AS edits hot-reload.
- **User is AFK:** machine verification gates each phase. User feel checkpoints are QUEUED, not blocking: checkpoint A after Task 10, checkpoint B after Task 17. v1 stack retirement happens only after the user's parity sign-off (not in this plan).
- Sources: Lyra content `F:\UnrealSamples\LyraStarterGame`, Lyra C++ reference `F:\UEAS\Samples\Games\Lyra\Source` (READ-ONLY), GASP `F:\UnrealSamples\SlidingAnimations\GameAnimationSample`.

## File map

| File | Role |
|---|---|
| `RogueSmoke/Config/DefaultEngine.ini` | CoreRedirects (anim instance, ContextEffects classes) |
| `RogueSmoke/Content/Characters/Heroes/Mannequin/**` | Lyra mannequin + full anim stack (copied) |
| `RogueSmoke/Content/Weapons/**` | Lyra gun meshes/ABPs/montages/sounds (copied) |
| `RogueSmoke/Content/Audio/**`, `Content/Effects/**` | Dependency closure (copied as needed) |
| `RogueSmoke/Script/Player/HeroAnimInstance.as` | Gains Lyra property surface (GroundDistance, GameplayTag_* bools) |
| `RogueSmoke/Script/Player/HeroCharacter.as` | Layer linking, slide montage driver, TickFacing gate, stamina regen |
| `RogueSmoke/Script/Player/LocomotionComponent.as` | Stamina gating on slide/slide-hop |
| `RogueSmoke/Script/Player/RaidPlayerController.as` | MoveTune knobs for stamina + recoil |
| `RogueSmoke/Script/UI/RogueHUDWidget.as` | Stamina pip row |
| `RogueSmoke/Source/RogueSmoke/Feedback/ContextEffects/*` | ContextEffects C++ port (5 classes) |
| `RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueMovementSet.h/.cpp` | Stamina attributes |
| Weapon component `.as` (located in Task 9) | Lyra montage refs, heat-model spread |

---

## Phase 0 — Content import

### Task 1: Branch, CoreRedirects, bulk copy

**Files:** Modify `RogueSmoke/Config/DefaultEngine.ini`; create content folders.

- [ ] **Step 1: Branch**

```powershell
git checkout -b lyra-anim-migration
```

- [ ] **Step 2: Add CoreRedirects** to `RogueSmoke/Config/DefaultEngine.ini` (append section):

```ini
[CoreRedirects]
+ClassRedirects=(OldName="/Script/LyraGame.LyraAnimInstance",NewName="/Script/Angelscript.RogueHeroAnimInstance")
```

(ContextEffects redirects are added in Task 12. If the spike in Task 3 shows AS classes register under a different package path, fix the NewName there.)

- [ ] **Step 3: Copy the two primary roots** (robocopy returns 1 on success-with-copies; that is OK):

```powershell
robocopy "F:\UnrealSamples\LyraStarterGame\Content\Characters\Heroes\Mannequin" "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\Content\Characters\Heroes\Mannequin" /E /NJH /NJS
robocopy "F:\UnrealSamples\LyraStarterGame\Content\Weapons" "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\Content\Weapons" /E /NJH /NJS
```

- [ ] **Step 4: Delete known Lyra-gameplay assets** that can never load (class lives in LyraGame): in `Content\Weapons\` root delete `B_Weapon.uasset`, `GA_Weapon_AutoReload.uasset`, `GA_Weapon_Fire.uasset`, `GA_Weapon_ReloadMagazine.uasset`; keep per-gun folders intact for now (Task 2 sweeps the rest mechanically).

- [ ] **Step 5: Commit config only** (content committed after closure in Task 2):

```powershell
git add RogueSmoke/Config/DefaultEngine.ini
git commit -m "feat(anim): CoreRedirect LyraAnimInstance -> RogueHeroAnimInstance (lyra migration phase 0)"
```

### Task 2: Dependency closure + load audit

**Driver: main session (editor).**

- [ ] **Step 1: Restart the editor** (`mcp__ue-cpp__editor_session_start` with `force_kill_existing: true`, wait_for_ready) so the redirect + new content are picked up.

- [ ] **Step 2: Closure walk** — run via `python_exec` until fixpoint (expect 2-3 iterations):

```python
import unreal, os, shutil
SRC = r"F:\UnrealSamples\LyraStarterGame\Content"
DST = r"C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\Content"
ar = unreal.AssetRegistryHelpers.get_asset_registry()
roots = ["/Game/Characters/Heroes/Mannequin", "/Game/Weapons"]
seen, missing = set(), set()
def pkg_exists(pkg):
    rel = pkg.replace("/Game/", "") + ".uasset"
    return os.path.exists(os.path.join(DST, rel)) or os.path.exists(os.path.join(DST, rel.replace(".uasset", ".umap")))
for root in roots:
    for ad in ar.get_assets_by_path(root, recursive=True):
        pkg = str(ad.package_name)
        if pkg in seen: continue
        seen.add(pkg)
        deps = ar.get_dependencies(unreal.Name(pkg), unreal.AssetRegistryDependencyOptions())
        for d in (deps or []):
            ds = str(d)
            if ds.startswith("/Game/") and not pkg_exists(ds):
                missing.add(ds)
copied = 0
for m in sorted(missing):
    src = os.path.join(SRC, m.replace("/Game/", "") + ".uasset")
    if os.path.exists(src):
        dst = os.path.join(DST, m.replace("/Game/", "") + ".uasset")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst); copied += 1
    else:
        unreal.log_warning("NO SOURCE: " + m)
unreal.log("missing={} copied={}".format(len(missing), copied))
```

Re-run after an editor restart (registry rescan) until `missing=0` or only `NO SOURCE` entries remain (those are /Game refs into Lyra plugin mounts — record them; they become null-safe gaps, not errors).

- [ ] **Step 3: Load audit** — find copied assets whose class is still a LyraGame type (unloadable) and delete them:

```python
import unreal
ar = unreal.AssetRegistryHelpers.get_asset_registry()
bad = []
for root in ["/Game/Characters/Heroes/Mannequin", "/Game/Weapons", "/Game/Audio", "/Game/Effects"]:
    for ad in ar.get_assets_by_path(root, recursive=True):
        cp = str(ad.asset_class_path.package_name)
        if cp == "/Script/LyraGame":
            bad.append(str(ad.package_name))
for b in bad: unreal.log_warning("LYRA-CLASS ASSET: " + b)
unreal.log("count=" + str(len(bad)))
```

Delete each listed file on disk EXCEPT anything matching `ABP_Mannequin_Base` (its class shows as redirected; if it still lists as LyraGame the redirect failed — stop and flag). Restart editor, re-run: expect `count=0`.

- [ ] **Step 4: Log sweep** — `log_tail` for `LoadErrors|Failed to load|Unknown class` while opening `/Game/Characters/Heroes/Mannequin/Animations/ABP_Mannequin_Base` via python `unreal.load_asset`. Record errors; montage/anim assets must load clean.

- [ ] **Step 5: SmokeTest must still pass** (v1 stack untouched): `powershell Tools\SmokeTest.ps1` → `SMOKE TEST PASSED: all 9 levels.`

- [ ] **Step 6: Commit content** (large; one commit):

```powershell
git add RogueSmoke/Content/Characters/Heroes RogueSmoke/Content/Weapons RogueSmoke/Content/Audio RogueSmoke/Content/Effects
git commit -m "feat(content): import Lyra mannequin anim stack + weapon assets (phase 0)"
```

---

## Phase 1 — Base anim stack

### Task 3: Spike — re-parent viability (DECISION GATE, time-box ~1h)

**Driver: main session (editor).** Success → Approach A confirmed. Failure → STOP, fall back per spec (stock-editor pre-surgery, then C++ shim).

- [ ] **Step 1: Verify the AS class path** the redirect targets:

```python
import unreal
c = unreal.load_class(None, "/Script/Angelscript.RogueHeroAnimInstance")
unreal.log(str(c))
```

Expected: a valid class, not None. If None, dump candidates: `unreal.log("\n".join([str(x.get_path_name()) for x in unreal.UObjectIterator(unreal.Class) if "RogueHeroAnimInstance" in str(x.get_name())]))` and fix the redirect NewName in DefaultEngine.ini + restart.

- [ ] **Step 2: Load + inspect parentage:**

```python
import unreal
bp = unreal.load_object(None, "/Game/Characters/Heroes/Mannequin/Animations/ABP_Mannequin_Base.ABP_Mannequin_Base")
gc = bp.generated_class()
unreal.log("parent=" + str(unreal.SystemLibrary.get_class_display_name(gc.get_super_class() if hasattr(gc,'get_super_class') else gc)))
```

Also run `blueprint_outline` on the asset: record the full **variable list** (expect `GameplayTag_IsFiring`-style bools) and montage **slot names**. Save that list — Task 4 implements exactly it.

- [ ] **Step 3: Compile check:** `blueprint_graph_compile` on `ABP_Mannequin_Base`. Record every error/warning. Expected failures allowed at this stage: unresolved property-access to `GameplayTagPropertyMap`-fed vars (they're BP vars — should be fine), anything touching `LyraCharacterMovementComponent` (record paths).

- [ ] **Step 4: Binding survival probe:** pick one tag var (e.g. `GameplayTag_IsFiring`): `blueprint_remove_variable`, recompile → note the error the binding produces; this tells us whether deleted-var bindings re-resolve to a same-named parent UPROPERTY after Task 4 (Lyra property access binds by name path). Re-add nothing — Task 4 supplies the parent properties.

- [x] **Step 5: Write spike verdict** into the plan file (this doc, under this task): redirect OK? var list? broken bindings list? slots? **Commit nothing.**

> **SPIKE VERDICT (2026-06-12): Approach A GO.**
> - Redirect works: `ABP_Mannequin_Base.parent_class == RogueHeroAnimInstance`; `class_is_child_of` confirms; AS class registered at `/Script/Angelscript.RogueHeroAnimInstance`.
> - Tag vars: exactly 5 — `GameplayTag_{IsADS,IsFiring,IsReloading,IsDashing,IsMelee}` (plan's expected set).
> - Parent surface needed: `GroundDistance` only. `GetMovementComponent()` returns STOCK `CharacterMovementComponent` — zero LyraCMC references in the 5.7 graph.
> - Compile errors, all mapped: (a) `AimPitch`/`AimYaw` BP-vars collide with our parent props AND the graph writes them via `UpdateAimingData` → Task 4 must delete those two BP vars and flip our properties to `BlueprintReadWrite` (graph's per-frame write wins inside the Lyra instance; v1 ABP instance unaffected — separate anim instance object). (b) `GroundDistance` missing → add to parent.
> - Task 2 bonus findings: closure complete at 58 extra files; 14 nosrc refs are pre-existing v1 gaps (GASP foley etc.), not Lyra needs; deleted 4 LyraGame-class assets + 1 editor-only AnimModifier (`FootstepEffectTagModifier` — Phase 4 may recopy after redirects); 5 tracked pistol texture/MI files were path-identical and overwritten by Lyra's newer versions (kept).

### Task 4: Extend `URogueHeroAnimInstance` with the Lyra surface

**Files:** Modify `RogueSmoke/Script/Player/HeroAnimInstance.as`. **v1 properties stay** (same instance serves the v1 ABP until retirement).

- [ ] **Step 1: Add properties + fills.** Add to the class (names MUST match the Task-3 var list; the set below is the expected one — extend to match):

```angelscript
    // --- Lyra ABP_Mannequin_Base surface (post-migration; v1 fields above remain) ---
    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    float GroundDistance = -1.0;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsFiring = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsReloading = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsADS = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsDashing = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsMelee = false;
```

In `BlueprintUpdateAnimation`, after the aim block:

```angelscript
        // Lyra surface: ground distance for distance-matched landing (trace only while airborne).
        if (bIsFalling)
        {
            FVector Start = Hero.GetActorLocation();
            FVector End = Start - FVector(0.0, 0.0, 2000.0);
            FHitResult Hit;
            TArray<AActor> Ignore; Ignore.Add(Hero);
            if (System::LineTraceSingle(Start, End, ETraceTypeQuery::Visibility, false, Ignore, EDrawDebugTrace::None, Hit, true))
                GroundDistance = Hit.Distance - Hero.CapsuleComponent.CapsuleHalfHeight;
            else
                GroundDistance = 2000.0;
        }
        else GroundDistance = 0.0;

        GameplayTag_IsFiring = Hero.IsFireHeldForFacing();   // add const getter if missing
        GameplayTag_IsADS = Hero.bFocusing;
        GameplayTag_IsDashing = bIsSliding;
        // IsReloading / IsMelee wired when those states exist; false is correct today.
```

(If `IsFireHeldForFacing()` doesn't exist on `AHeroCharacter`, add `bool IsFireHeldForFacing() const { return bFireHeldForFacing; }` next to `SetFireHeldForFacing`.)

- [ ] **Step 2: Compile script:** `mcp__plugin_ue-as_as-helper__run_code_test` → 0 errors.

- [ ] **Step 3: Editor restart** (structural AS change — new UPROPERTYs do NOT hot-reload).

- [ ] **Step 4: In the copied ABP, delete the now-shadowed BP vars** (each name from the Task-3 list that the parent now provides): `blueprint_remove_variable` × N → `blueprint_graph_compile` → expect 0 errors, bindings resolved to parent. If a binding goes dead instead, re-add that BP var and have a tiny thread-safe function copy from parent (record which).

- [ ] **Step 5: Fix `LyraCharacterMovementComponent` references** recorded in Task 3 (expected: only GroundDistance reads, already satisfied; if a graph node calls a Lyra CMC function directly, rewire the property access path to the parent's `GroundDistance`). Recompile → 0 errors. Save asset (`only_if_is_dirty=False` — see memory note).

- [ ] **Step 6: Commit:**

```powershell
git add RogueSmoke/Script/Player/HeroAnimInstance.as RogueSmoke/Script/Player/HeroCharacter.as "RogueSmoke/Content/Characters/Heroes/Mannequin/Animations/ABP_Mannequin_Base.uasset"
git commit -m "feat(anim): re-parent ABP_Mannequin_Base onto URogueHeroAnimInstance; Lyra property surface (phase 1)"
```

### Task 5: Switch the hero to the Lyra stack

**Files:** Modify hero BPs (python), `RogueSmoke/Script/Player/HeroCharacter.as`.

- [ ] **Step 1: Locate hero BPs:** `assets_find_by_class` for Blueprints, or outline known heroes (search `/Game` for `BP_Hero*`). Record paths + current Mesh transform (relative location/rotation — typically z=-90, yaw=-90).

- [ ] **Step 2: Add layer-link + free-look gate to `HeroCharacter.as`:**

```angelscript
    // Linked anim layer for the held weapon (Lyra pattern). Set on hero BPs; rifle by default.
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    TSubclassOf<UAnimInstance> WeaponAnimLayer;

    // v1 actor-level idle free-look. OFF under the Lyra stack (RootYawOffset/turn-in-place owns it).
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    bool bActorLevelFreeLook = false;
```

In `BeginPlay` (additive — no Super in AS):

```angelscript
        if (WeaponAnimLayer.IsValid())
            Mesh.LinkAnimClassLayers(WeaponAnimLayer);
```

In `TickFacing`, first line: `if (!bActorLevelFreeLook) return;` — and restore `bUseControllerRotationYaw = true` once in `BeginPlay` when free-look is off.

- [ ] **Step 3: Compile script** (`run_code_test`), **restart editor** (structural).

- [ ] **Step 4: Point hero BPs at the Lyra stack** (python per BP; memory: CDO edits need compile + save):

```python
import unreal
mesh = unreal.load_asset("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Manny")
abp  = unreal.load_object(None, "/Game/Characters/Heroes/Mannequin/Animations/ABP_Mannequin_Base.ABP_Mannequin_Base_C")
layer= unreal.load_object(None, "/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/ABP_RifleAnimLayers.ABP_RifleAnimLayers_C")
for path in HERO_BP_PATHS:  # from Step 1
    bp = unreal.load_object(None, path)
    cdo = unreal.get_default_object(bp.generated_class())
    sk = cdo.get_editor_property("mesh")
    sk.set_editor_property("skeletal_mesh_asset", mesh)
    sk.set_editor_property("anim_class", abp)
    cdo.set_editor_property("weapon_anim_layer", layer)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False)
```

(Exact mesh asset name verified in Step 1 of Task 2's audit — Lyra full Manny is under `Meshes/`; use what exists, `SKM_Manny` expected.)

- [ ] **Step 5: Boot `DL_Upgrades`, verify live:** PIE start, `actor_properties` on the hero pawn → mesh = SKM_Manny, anim instance class = ABP_Mannequin_Base_C; python `get_linked_anim_layers` shows the rifle layer. No log errors.

- [ ] **Step 6: Commit** (hero BPs + script).

### Task 6: Machine verification battery — Phase 1 gate

**Driver: main session (PIE in `DL_Upgrades`).**

- [ ] **Step 1: Idle integrity probe** (the −35° regression test, adapted): sample `pelvis` component-yaw at idle vs `MM_Rifle_Idle` authored value — must match within 3°.
- [ ] **Step 2: Turn-in-place probe:** at idle, yaw the control rotation 120° via `input_simulate_axis`; assert (a) foot bone world positions stay fixed (<2 cm drift) while aim tracks, then (b) past the threshold the body turns (actor/mesh root yaw changes). This replaces v1 TickFacing — confirm `bActorLevelFreeLook=false` path is inert.
- [ ] **Step 3: Locomotion probe:** simulate forward + diagonal movement; assert velocity-vs-facing Direction ≈ anim Direction; sprint at 960 shows no foot-slide (distance-matching handles above-authored speeds — visual screenshot + foot-speed sample).
- [ ] **Step 4: Jump/land:** teleport-drop the pawn; assert `GroundDistance` goes positive in air, land recovery plays (montage/state via `anim_get_active_section` or bone probe).
- [ ] **Step 5: `MoveSmoke` exec** still green (slide physics rules unchanged).
- [ ] **Step 6: SmokeTest 9/9 → commit** `"test(anim): phase 1 verification battery green"` (any probe scripts saved under `Tools/`).

### Task 7: GASP slide graft (montage-driven)

**Files:** New retargeted anims under `/Game/Characters/Heroes/Mannequin/Animations/Slide/`; modify `HeroCharacter.as`, `LocomotionComponent.as`.

- [ ] **Step 1: Retarget GASP slide set onto the Lyra skeleton** (pipeline from v1 Task 6 — memory `retarget-bake-via-python`): source anims `M_Neutral_Slide_FootOut_{Into_Lfoot,Into_Rfoot,Loop,Out_Idle_Stand,Out_Idle_Crouch,Out_Moving_Run,Out_Moving_Crouch}` from `F:\UnrealSamples\SlidingAnimations\GameAnimationSample\Content\Characters\UEFN_Mannequin\Animations\Slide\` (copy files in first; they're UEFN-skeleton). Use `unreal.IKRetargetBatchOperation.duplicate_and_retarget` targeting Lyra's mannequin mesh. If GASP's skeleton proves identical to Lyra's (test-load one sequence on the Lyra mesh first), skip the retarget and just re-skeleton.
- [ ] **Step 2: Build one slide montage** (`anim_create_montage` or python): sections `In_L`, `In_R`, `Loop` (looping), `Out_Stand`, `Out_Crouch`, `Out_Run`; slot `DefaultSlot` (confirm slot name from Task-3 outline).
- [ ] **Step 3: Drive it from locomotion state** — in `HeroCharacter.as`, subscribe to slide start/end (LocomotionComponent already has `StartSlide`/`EndSlide` internals; expose two events):

In `LocomotionComponent.as` add at class scope and fire inside `StartSlide()` / `EndSlide()`:

```angelscript
    // Anim hooks: HeroCharacter plays the slide montage off these (cosmetic, runs on all machines
    // because sliding state itself is replicated through the crouch+velocity mirrors).
    UPROPERTY()
    private AHeroCharacter AnimListener;
    void SetAnimListener(AHeroCharacter InHero) { AnimListener = InHero; }
```

…and call `AnimListener.OnSlideAnimStart()` / `AnimListener.OnSlideAnimEnd(bCrouchHeld, Speed)` at the end of `StartSlide`/`EndSlide`. In `HeroCharacter.as`:

```angelscript
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage SlideMontage;

    void OnSlideAnimStart()
    {
        if (SlideMontage == nullptr) return;
        UAnimInstance Anim = Mesh.GetAnimInstance();
        if (Anim == nullptr) return;
        Anim.Montage_Play(SlideMontage);
        // Foot-aware entry: pick In_L/In_R off the anim instance's last foot-down if available; default In_L.
        Anim.Montage_JumpToSection(n"In_L", SlideMontage);
    }

    void OnSlideAnimEnd(bool bCrouchHeld, float ExitSpeed)
    {
        UAnimInstance Anim = Mesh.GetAnimInstance();
        if (Anim == nullptr || SlideMontage == nullptr) return;
        FName Section = bCrouchHeld ? n"Out_Crouch" : (ExitSpeed > 500.0 ? n"Out_Run" : n"Out_Stand");
        Anim.Montage_JumpToSection(Section, SlideMontage);
    }
```

Sim proxies: sliding mirrors through `bIsSliding` on the anim instance — also trigger montage from `URogueHeroAnimInstance.BlueprintUpdateAnimation` on the `bIsSliding` rising/falling edge for non-owning machines (guard owner double-fire with `Montage_IsPlaying`).

- [ ] **Step 4: Compile (`run_code_test`), restart editor, assign `SlideMontage` on hero BPs** (python CDO + compile + save as Task 5 Step 4).
- [ ] **Step 5: Verify in PIE:** sprint+crouch → slide; `anim_get_active_section` returns `Loop` mid-slide and the right `Out_*` on exit; MoveSmoke still green (montage must not alter physics).
- [ ] **Step 6: SmokeTest → commit** `"feat(anim): GASP slide set as montage graft over Lyra stack (phase 1)"`.

---

## Phase 2 — Gun models

> **EXECUTION LOG (Tasks 4-7, 2026-06-12):** all done and verified. Highlights/deviations:
> - Plugins enabled in .uproject (Lyra graphs need them): `AnimationLocomotionLibrary`, `AnimationWarping`, `Metasound`.
> - `AimPitch`/`AimYaw` flipped to `BlueprintReadWrite`; ABP's shadowing BP-vars (those two + 5 `GameplayTag_*`) deleted; full chain (`ABP_Mannequin_Base` + `ABP_ItemAnimLayersBase` + `ABP_RifleAnimLayers`) compiles with ZERO errors on the AS parent.
> - Probes green: turn-in-place (RootYawOffset −120→−7.5 under smooth yaw, feet <3cm drift), locomotion (DisplacementSpeed==600 at run, no foot-slide), jump (GroundDistance feeds), MoveSmoke 3/3.
> - Slide = dynamic montages (`PlaySlotAnimationAsDynamicMontage` on DefaultSlot) driven by a Tick edge-detector — NOT a montage asset with sections; GASP set retargeted via `RTG_UEFN_to_UE5_Mannequin` (8 anims at `Animations/Slide/MM_Slide_*`). Verified live: In → 2s loop-hold → Out_Idle_Crouch. In→Loop handover untested (In clip outlives short slides) — feel-pass item.
> - **MAJOR FIND:** hero BPs carried template EventGraph cruft — an unconnected `Event Tick` node had been **swallowing the AS Tick since v1** (TickLocomotion/TickFacing/full-auto refire never ran!), plus BP-side AddMappingContext + IA_Move→DoMove double input. All 10 nodes purged from both BPs. `TickSlideAnim()` now runs BEFORE `TickLocomotion()` (same-frame start/end edge ordering).
> - Synthetic-probe gotchas for future sessions: unfocused editor runs 8-30fps; `Print(text, 0.0)` never logs; pawn gets pinned at arena walls after repeated forward-drive runs (teleport home between probes).

### Task 8: Attach SK_Rifle with its weapon ABP

- [ ] **Step 1: Inspect current gun visual:** `blueprint_outline` each hero BP — find the existing weapon mesh component (name + socket). Verify the Lyra socket python-side: list `SKM_Manny` skeleton sockets, expect `weapon_r` on `hand_r`.
- [ ] **Step 2: Repoint the weapon component:** python per hero BP — set the component's mesh to `/Game/Weapons/Rifle/Mesh/SK_Rifle`, anim class `/Game/Weapons/Rifle/Animations/ABP_Weap_Rifle.ABP_Weap_Rifle_C`, attach socket `weapon_r`, zero relative transform; compile + save. If the current component is a StaticMeshComponent, add a SkeletalMeshComponent `WeaponMesh` via `blueprint_add_component` and hide the old one.
- [ ] **Step 3: PIE check:** rifle in hand, correct grip alignment at idle/ADS (screenshot), no log errors from `ABP_Weap_Rifle`.
- [ ] **Step 4: SmokeTest → commit.**

### Task 9: Swap to Lyra montages (character + weapon)

- [ ] **Step 1: Find the v1 montage wiring:** `Grep` for `Montage` in `RogueSmoke/Script/Player/` and the weapon component `.as` (Task 9 of the v1 plan wired fire/reload montages — record the UPROPERTY names and which BP CDOs hold the assets).
- [ ] **Step 2: Repoint character montages** via CDO python: fire → `/Game/Weapons/Rifle/Animations/AM_MM_Rifle_Fire`, reload → `AM_MM_Rifle_Reload`. Upper-body slot: Lyra montages use Lyra slot names — if their slot isn't in our slot list, the montage plays on `DefaultSlot`/`UpperBody` per its asset; verify in Step 4.
- [ ] **Step 3: Weapon-mesh montages:** where the character fire montage plays (grep hit from Step 1), add the gun-side call:

```angelscript
        UAnimInstance WeapAnim = WeaponMesh != nullptr ? WeaponMesh.GetAnimInstance() : nullptr;
        if (WeapAnim != nullptr && WeaponFireMontage != nullptr)
            WeapAnim.Montage_Play(WeaponFireMontage);
```

with `UPROPERTY(EditDefaultsOnly) UAnimMontage WeaponFireMontage;` (assign `AM_Weap_Rifle_Fire`; same for reload).

- [ ] **Step 4: PIE verify:** fire → upper-body recoil anim + bolt/mag movement on the gun; reload → both meshes animate; lower body keeps running (layered, not full-body).
- [ ] **Step 5: SmokeTest → commit.**

### Task 10: Phase 1–2 gate + user checkpoint A

- [ ] **Step 1: Replication audit (machine):** confirm every anim input derives from replicated state: velocity (movement repl), `BaseAimRotation`, `bIsSliding` (crouch+speed mirrors), `GameplayTag_IsFiring` (`bFireHeldForFacing` is server-mirrored — verify it replicates to **simulated proxies**, not just owner; if owner-only, add a `Replicated` mirror).
- [ ] **Step 2: SmokeTest 9/9 + MoveSmoke + probe battery rerun** (Tasks 6 probes on the final wired state).
- [ ] **Step 3: Write user checkpoint A** into `SUPERPOWERS_HANDOFF.md`-style note `docs/superpowers/plans/2026-06-12-lyra-checkpoint-A.md`: what to feel-test (idle turn-in-place, starts/stops/pivots, sprint, slide enter/exit, fire/reload look, 2-player listen-server session — MCP can't drive 2 players). **Do not block; continue to Phase 3.**
- [ ] **Step 4: Commit.**

---

## Phase 3 — Weapon audio & VFX

### Task 11: Fill the FireFX slots with Lyra assets

- [ ] **Step 1: Inventory:** search our copied content + Lyra source content for rifle fire MetaSound (`Content/Weapons/Rifle/Sounds/`), muzzle flash / tracer / impact Niagara (search `F:\UnrealSamples\LyraStarterGame\Content\Effects` and `Plugins\GameFeatures\ShooterCore\Content` for `NS_*Muzzle*|NS_*Tracer*|NS_*Impact*|*WeaponFire*`). Copy engine-class assets + rerun the Task-2 closure script for `/Game/Audio` + `/Game/Effects`.
- [ ] **Step 2: Locate the Task-11 (v1) FireFX slot properties** (grep the weapon component for the null-safe UPROPERTY slots: muzzle NS, tracer NS, impact NS, fire sound, etc.).
- [ ] **Step 3: Assign via CDO python + compile + save.** ShooterCore assets that are GameplayCue BPs: open one, harvest the Niagara/MetaSound asset refs it points to — we take the leaf assets, never the cue BP.
- [ ] **Step 4: PIE verify:** fire → muzzle flash visible (screenshot), impact at hit point, audible fire log (`log_capture` for audio play or check `AudioComponent` spawn); no missing-asset warnings.
- [ ] **Step 5: SmokeTest → commit** `"feat(fx): Lyra rifle audio + muzzle/tracer/impact Niagara into FireFX slots (phase 3)"`.

---

## Phase 4 — ContextEffects footsteps

### Task 12: C++ port + redirects + content

**Files:** Create `RogueSmoke/Source/RogueSmoke/Feedback/ContextEffects/` (10 files), modify `DefaultEngine.ini`, `HeroCharacter.as`.

- [ ] **Step 1: Port classes** from `F:\UEAS\Samples\Games\Lyra\Source\LyraGame\Feedback\ContextEffects\` (read-only source), rename per table, fix `LYRAGAME_API`→`ROGUESMOKE_API`, includes, and replace `LyraLogChannels` with a local `DEFINE_LOG_CATEGORY_STATIC(LogRogueContextEffects, Log, All)`:

| Lyra | RogueSmoke |
|---|---|
| `ULyraContextEffectsLibrary` | `URogueContextEffectsLibrary` |
| `UAnimNotify_LyraContextEffects` | `UAnimNotify_RogueContextEffects` |
| `ULyraContextEffectComponent` | `URogueContextEffectComponent` |
| `ILyraContextEffectsInterface` | `IRogueContextEffectsInterface` (C++-only; AS never implements it) |
| `ULyraContextEffectsSubsystem` | `URogueContextEffectsSubsystem` |

- [ ] **Step 2: Build:** `mcp__ue-cpp__build` → 0 errors.
- [ ] **Step 3: Redirects** (append to `[CoreRedirects]`):

```ini
+ClassRedirects=(OldName="/Script/LyraGame.LyraContextEffectsLibrary",NewName="/Script/RogueSmoke.RogueContextEffectsLibrary")
+ClassRedirects=(OldName="/Script/LyraGame.AnimNotify_LyraContextEffects",NewName="/Script/RogueSmoke.AnimNotify_RogueContextEffects")
+ClassRedirects=(OldName="/Script/LyraGame.LyraContextEffectComponent",NewName="/Script/RogueSmoke.RogueContextEffectComponent")
```

- [ ] **Step 4: Copy content:** `Content/ContextEffects/**` (library DataAssets) + footstep sounds (closure script for `/Game/Audio` again). Restart editor; verify a library asset loads as `URogueContextEffectsLibrary` and copied Lyra anims show their footstep notifies as `AnimNotify_RogueContextEffects` (python: read notifies off `MM_Rifle_Jog_Fwd`).
- [ ] **Step 5: Attach component from AS** (runtime attach per memory `as-subclass-of-cpp-gotchas`): in `HeroCharacter.as` `BeginPlay`: `URogueContextEffectComponent::Create(this)`; configure its library list to the copied DataAssets via UPROPERTY on the hero (assign by python CDO if class-default array).
- [ ] **Step 6: PIE verify:** run around; `log_capture` shows context-effect spawns (default surface); no per-step errors.
- [ ] **Step 7: SmokeTest → commit** `"feat(feedback): ContextEffects port + Lyra footstep notifies live (phase 4)"`.

---

## Phase 5 — Recoil & spread

### Task 13: Heat model + camera recoil

**Files:** Modify weapon component `.as` (path from Task 9 Step 1), `RaidPlayerController.as` (MoveTune), `CameraFeelComponent.as` (tune values only).

- [ ] **Step 1: Add heat model to the weapon component:**

```angelscript
    // --- Lyra-style heat model: spread follows heat; heat rises per shot, cools when not firing ---
    UPROPERTY(EditDefaultsOnly, Category = "Spread|Heat")
    float HeatPerShot = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Spread|Heat")
    float HeatCooldownPerSecond = 24.0;

    UPROPERTY(EditDefaultsOnly, Category = "Spread|Heat")
    float HeatCooldownDelay = 0.25;

    // Piecewise-linear heat(0..100) -> bonus spread degrees (seeded from Lyra rifle's curve shape).
    UPROPERTY(EditDefaultsOnly, Category = "Spread|Heat")
    TArray<FVector2D> HeatToSpread;
    default HeatToSpread.Add(FVector2D(0.0, 0.0));
    default HeatToSpread.Add(FVector2D(30.0, 0.4));
    default HeatToSpread.Add(FVector2D(60.0, 1.2));
    default HeatToSpread.Add(FVector2D(100.0, 2.6));

    private float Heat = 0.0;
    private float TimeSinceShot = 1000.0;

    float EvalHeatSpread() const
    {
        if (HeatToSpread.Num() == 0) return 0.0;
        if (Heat <= HeatToSpread[0].X) return HeatToSpread[0].Y;
        for (int i = 1; i < HeatToSpread.Num(); i++)
        {
            if (Heat <= HeatToSpread[i].X)
            {
                float T = (Heat - HeatToSpread[i-1].X) / Math::Max(HeatToSpread[i].X - HeatToSpread[i-1].X, 0.001);
                return Math::Lerp(HeatToSpread[i-1].Y, HeatToSpread[i].Y, T);
            }
        }
        return HeatToSpread[HeatToSpread.Num()-1].Y;
    }
```

On each shot: `Heat = Math::Min(Heat + HeatPerShot, 100.0); TimeSinceShot = 0.0;`. In the component tick: `TimeSinceShot += Delta; if (TimeSinceShot > HeatCooldownDelay) Heat = Math::Max(0.0, Heat - HeatCooldownPerSecond * Delta);`. `GetSpreadDegrees(...)` adds `EvalHeatSpread()` to its current result.

- [ ] **Step 2: Camera recoil tune:** per-shot `KickPitch` on `CameraFeelComponent` toward Lyra-feel values (start: 0.35° per shot, recover 8°/s — already-existing knobs; just retune defaults).
- [ ] **Step 3: MoveTune knobs:** add `heatpershot`, `heatcooldown`, `heatdelay` dispatch entries + dump lines in `RaidPlayerController.as` (mirror existing entries).
- [ ] **Step 4: Verify:** `run_code_test`; PIE: hold fire 2s — crosshair bloom grows (HUD already tracks `GetSpreadDegrees`), stops growing at cap, recovers after release (sample `GetSpreadDegrees` via python before/during/after).
- [ ] **Step 5: SmokeTest → commit** `"feat(weapon): Lyra-style heat spread model + recoil tune (phase 5)"`.

---

## Phase 6 — Stamina (Deadlock pips)

### Task 14: `URogueMovementSet` attributes

**Files:** Create `RogueSmoke/Source/RogueSmoke/AbilitySystem/Attributes/RogueMovementSet.h/.cpp`. Mirror `RogueHealthSet` exactly (same macros, same OnRep trampoline pattern).

- [ ] **Step 1: Header** (`RogueMovementSet.h`):

```cpp
// RogueMovementSet.h
// Movement-economy attributes (stamina pips, Deadlock-style). C++ per D-0013; spend/regen logic
// lives in AngelScript; meta-progression upgrades are plain GameplayEffects on MaxStamina.
#pragma once

#include "CoreMinimal.h"
#include "AngelscriptAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "RogueMovementSet.generated.h"

#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

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
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION() void OnRep_Stamina(const FAngelscriptGameplayAttributeData& Old)    { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }
	UFUNCTION() void OnRep_MaxStamina(const FAngelscriptGameplayAttributeData& Old) { FAngelscriptGameplayAttributeData O = Old; OnRep_Attribute(O); }

private:
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Stamina, Category = "Movement", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData Stamina;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxStamina, Category = "Movement", meta = (AllowPrivateAccess = true))
	FAngelscriptGameplayAttributeData MaxStamina;
};
```

- [ ] **Step 2: Cpp** — constructor `Stamina/MaxStamina = 3.f`; `PreAttributeChange` clamps `Stamina` to `[0, GetMaxStamina()]`; `GetLifetimeReplicatedProps` registers both with `COND_None, REPNOTIFY_Always` (copy `RogueHealthSet.cpp`'s exact DOREPLIFETIME pattern).
- [ ] **Step 3: Grant it** where `URogueHealthSet` is granted (grep `URogueHealthSet` outside Attributes/ — PlayerState or ASC init; add the set the same way).
- [ ] **Step 4: Build (`ue-cpp build`) → 0 errors; restart editor; SmokeTest → commit.**

### Task 15: Spend / regen / gating

**Files:** Modify `LocomotionComponent.as`, `HeroCharacter.as`, `RaidPlayerController.as`.

- [ ] **Step 1: Hero stamina API** (`HeroCharacter.as`):

```angelscript
    UPROPERTY(EditDefaultsOnly, Category = "Stamina")
    float StaminaRegenSeconds = 2.5;     // one pip per this many seconds

    UPROPERTY(EditDefaultsOnly, Category = "Stamina")
    float StaminaRegenDelay = 1.0;       // pause after any spend

    private float RegenAccumulator = 0.0;
    private float RegenDelayRemaining = 0.0;

    float GetStamina() const
    {
        UAbilitySystemComponent ASC = GetAbilitySystemComponent();
        return ASC != nullptr ? ASC.GetAttributeCurrentValue(URogueMovementSet, n"Stamina") : 0.0;
    }

    bool HasStaminaPip() const { return GetStamina() >= 1.0; }

    // Server-only. Client gates locally off the replicated attribute (prediction = read-before-act).
    void SpendStaminaPip()
    {
        if (!HasAuthority()) return;
        UAbilitySystemComponent ASC = GetAbilitySystemComponent();
        if (ASC == nullptr) return;
        ASC.ApplyModToAttribute(URogueMovementSet::GetStaminaAttribute(), EGameplayModOp::Additive, -1.0);
        RegenDelayRemaining = StaminaRegenDelay;
        RegenAccumulator = 0.0;
    }
```

Regen in `Tick` (server only): delay counts down; then accumulate; each `StaminaRegenSeconds` → `+1` via `ApplyModToAttribute` while below max. (Exact AS syntax for the attribute getter: use the name-based ASC API as in `GetStamina`; if `ApplyModToAttribute` isn't bound, use the AngelscriptASC numeric-set API — check with `find_binding` first.)

- [ ] **Step 2: Gate slide + slide-hop** (`LocomotionComponent.as`): in `RequestCrouchOrSlide`, the slide-entry branch additionally requires `Hero.HasStaminaPip()`; on actual `StartSlide` the hero (server) calls `SpendStaminaPip()` — clients route through the existing slide replication (slide is CMC-mirrored; the SERVER observes slide start in `TickLocomotion` and spends there — no new RPC). Same for `NotifySlideJump` → second pip. When out of pips, slide request degrades to plain crouch (no hard feel-stop).
- [ ] **Step 3: MoveTune knobs** `staminaregenseconds`, `staminaregendelay` + dump block; **MoveSmoke additions:** assert pip spent on slide, slide denied at 0 stamina (degrades to crouch), pip restored after regen window (use time dilation to compress).
- [ ] **Step 4: `run_code_test` → restart → MoveSmoke green → SmokeTest → commit.**

### Task 16: HUD pips

**Files:** Modify `RogueSmoke/Script/UI/RogueHUDWidget.as`.

- [ ] **Step 1:** Find the health-bar build + attribute binding in `RogueHUDWidget.as` (HUD tree is built in AS `OnInitialized` per memory). Mirror it: a `UHorizontalBox` of `MaxStamina` square `UImage` pips next to the health bar; filled = white/cyan, spent = 25% alpha. Rebuild pip count on `MaxStamina` change, refill state on `Stamina` change (attribute-change delegate, same registration as health).
- [ ] **Step 2:** Tick-safe fallback: if the delegate API fights AS, poll `Hero.GetStamina()` in the existing HUD refresh tick (HUD already ticks for crosshair) — acceptable for 3 pips.
- [ ] **Step 3:** Verify headless-ish: PIE → python reads widget state? No (memory: MCP screenshots are gameview-only and widget trees are AS-built) — verify via the existing pattern: log lines on pip state change + slide → log shows 3→2→regen→3.
- [ ] **Step 4: SmokeTest → commit** `"feat(ui): stamina pip row on HUD (phase 6)"`.

### Task 17: Final gate, docs, checkpoint B

- [ ] **Step 1: Full battery:** SmokeTest 9/9, MoveSmoke (incl. stamina), probe battery (Task 6), heat-spread sample, footstep log check — all green on one editor session.
- [ ] **Step 2: Replication audit rerun** (Task 10 Step 1 list + stamina attrs to sim proxies for teammate HUD later — OwnerOnly is fine for now since only own pips render).
- [ ] **Step 3: Docs:**
  - `DECISIONS.md`: **D-0022** Lyra anim stack adopted (linked layers, CoreRedirect re-parent, skeleton swap; v1 retained until parity sign-off). **D-0023** stamina pips (3 pips, slide/slide-hop/double-jump cost 1, sprint free, GAS attributes).
  - `GLOSSARY.md`: Linked anim layer / ALI, RootYawOffset & turn-in-place, ContextEffects, Stamina pip; update **Slide** and **Anim instance** entries.
  - `docs/guides/ABP_HERO_BUILD_GUIDE.md`: prepend retirement banner pointing at the spec.
  - `startup.md` freshness pass.
- [ ] **Step 4: User checkpoint B** note (`docs/superpowers/plans/2026-06-12-lyra-checkpoint-B.md`): full feel pass list (everything from checkpoint A + audio/VFX, footsteps, bloom/recoil, stamina UI + denial feel, 2-player session). v1 retirement = separate commit after sign-off.
- [ ] **Step 5: Commit; leave branch for `superpowers:finishing-a-development-branch` after user sign-off.**

---

## Self-review notes

- Spec coverage: Phase 0→Tasks 1-2; Phase 1→3-7; Phase 2→8-10; Phase 3→11; Phase 4→12; Phase 5→13; Phase 6→14-16; verification/docs→6,10,17. Gems catalog/out-of-scope items intentionally unplanned.
- Known-unknown protocol: Task 3 is the decision gate; Tasks 4/5/9/11 consume its recorded outputs (var list, slot names, paths) rather than guessing.
- Type consistency: `WeaponAnimLayer`/`SlideMontage`/`WeaponFireMontage` (Task 5/7/9), `HasStaminaPip`/`SpendStaminaPip` (Tasks 15-16), `URogueMovementSet` (14-16) — names match across tasks.
