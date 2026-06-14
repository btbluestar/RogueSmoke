# Handoff — 3rd-person shooting + enemy roster (overnight 2026-06-09 → 10)

> 📦 **Historical handoff** — accurate for its date; the roster and damage seam it introduced
> are now documented in `DECISIONS.md` D-0017 and `GLOSSARY.md`.

Built while you slept. **Everything below is committed on `main` and compiles/builds clean**
(`ue-cpp build` + `as-helper run_code_test` exit 0). The focus-input wiring (item 1) was finished
headlessly via a python commandlet after the editor closed; what genuinely remains is **PIE feel-checks**
(needs your hands — the in-editor MCP command path was wedged this session) and optional art/tuning.

## Commits (newest first)
- `e8a1358` line-of-sight gate on ranged attacks (Spitter/Brood spit whiff through walls now)
- `eeb3a56` wire focus-aim to hold-RMB (the editor input wiring below — done headlessly)
- `6a603a2` Brood-mother mini-boss + wire elite roster into the raid loop
- `5c5c4d8` bio-horde elite roster (Carapace/Spitter/Bloater/Lunger)
- `f7ae989` enemy→player damage seam + Crawler melee (Plan 2 foundation)
- `049f33b` 3rd-person muzzle convergence + light focus-aim + hit feedback (Plan 1)
- `3af9a36` off-screen edge indicators on the HUD (earlier)

## What's done

### Plan 1 — Shooting (3rd person)
- **Camera→muzzle convergence:** `UCombatSubsystem::ResolveAimPoint` (C++) traces camera→crosshair;
  `GA_WeaponFire` now fires the damage trace **and tracer from the muzzle** toward that point (was:
  straight from the camera, so bullets came out of your face). Muzzle = `HeroCharacter.GetMuzzleLocation()`
  (weapon socket, or a shoulder-offset fallback so it works with no weapon art yet).
- **Light focus-aim (hold):** zooms FOV 90→70, pulls the boom 350→220, tightens spread, slows the
  strafe. Camera blend is local/cosmetic; spread+move-speed mirror to the server like sprint.
- **Feedback:** crosshair blooms while moving / tightens on focus; hitmarker flashes on confirmed hits.

### Plan 2 — Enemies (bio-horde theme)
- **Enemy→player damage seam (C++):** `UCombatSubsystem::ApplyDamageToPlayer` /
  `ApplyRadialDamageToPlayers` apply a transient instant Damage GE to the hero's ASC (armor/shield/health
  resolve in `RogueHealthSet`). Enemies had **no way to hurt players** before this.
- **Crawler** (`AFodderEnemy`): contact melee — the swarm now bites.
- **`AAttackingElite` (C++ base):** self-contained (visible cube body + Visibility-blocking collision,
  like `AFodderEnemy` — hittable with no Blueprint), runs the shared loop (acquire nearest **living**
  hero via GAS health, approach, telegraph, attack). Per-archetype attack is a `BlueprintNativeEvent`
  overridden in AngelScript. Telegraphs are debug-drawn (yellow wind-up + slam footprint).
- **AngelScript archetypes:** **Carapace** (tanky radial-slam shield elite — the taunt/cluster synergy
  anchor that now fights back), **Spitter** (ranged kiter), **Bloater** (suicide bomber, blast on
  contact-or-death), **Lunger** (telegraph→lunge gap-closer; dodge with the slide), **Brood-mother**
  (mini-boss: cycles spit / summon-wave / artillery-AoE).
- **Raid loop wired:** `RaidObjective` spawns a seeded mix of elites + a boss at start (defaults to the
  full roster, so it works with **no editor wiring**). They gate "clear the arena"; fodder waves are
  non-gating pressure. → clear elites+boss under swarm → call extraction → survive defend wave → win.

## What YOU need to do (editor only)

1. ~~**Focus input**~~ — ✅ **DONE** (`eeb3a56`, via a headless python commandlet once the editor was
   closed): `IA_Focus` created, **RMB → IA_Focus** mapped in `IMC_Default.default_key_mappings` (the live
   UE5.7 array — the deprecated top-level `Mappings` is ignored at runtime), `FocusAction = IA_Focus` set +
   compiled on `BP_RaidPlayerController`. Only the **in-PIE feel-test** of focus remains (task #43).
2. **(Optional) weapon art:** assign a `SkeletalMesh` + a `Muzzle` socket on the hero BPs and the
   `WeaponMesh` field on the `DA_Weapon_*` — gives a visible gun + exact muzzle. Until then the
   shoulder-offset fallback is used (convergence still works).
3. **(Optional) focus tuning:** `FocusSpreadMultiplier / FocusFOV / FocusArmLength` on the `DA_Weapon_*`.
4. **(Optional) enemy art:** elites/fodder use engine cubes/spheres. Swap to bio-horde creature meshes
   (and tint the material per archetype for friend/enemy/hazard readability) on BP subclasses later.
5. **PIE verification checklist** (DL_Combat; the MCP wedge is my tooling, your editor PIE is fine):
   - Shooting: fire at an **off-centre** elite → bullets/tracer come from the gun and hit the crosshair.
     Hold RMB → zoom + tighter spread + slower strafe. Hitmarker flashes on hits.
   - Enemies: each archetype **telegraphs** (yellow sphere; slam shows its footprint) then attacks;
     your **health drops** (watch `RogueHealthSet.Health`); downed players aren't targeted.
   - Loop: clear all elites + boss → "extraction ready" → call it → survive → "EXTRACTED".
   - Re-check the synergy: Vanguard taunt clusters a Carapace → Bombardier barrage punishes it.

## Notes / known limitations (all MVP-acceptable; flagged for polish)
- **Editor MCP was wedged this session:** `ping` answered but every command returned
  `PLUGIN_INTERNAL_ERROR`, so I couldn't author content/run PIE via MCP. Headless `ue-cpp build` and
  `as-helper run_code_test` worked fine. (Recorded in memory.)
- **Telegraphs are debug-draw only** — real Niagara/decal telegraphs + attack/death GameplayCues are the
  juice pass (part of #24). Replace the `DrawDebug*` calls.
- **Dynamic damage GE** allocates a `UGameplayEffect` per hit (GC pressure at scale) — fine for MVP;
  cache/pool or use a SetByCaller asset GE later.
- **Ranged/charge feel upgrades DONE (task #40):** Spitter/Brood spit gate on **line of sight**
  (`HasLineOfSightToActor`); the **Spitter now lobs an arcing `ASpitterProjectile`** (visible glob, splash
  on landing at the target's fire-time position — move to dodge); the **Lunger** leaps via a reusable
  `AAttackingElite::StartDash` (smooth dodgeable slide, not a teleport). Arc/dash *feel* tuning is PIE (#43).
- **Readability:** each archetype now has a distinct **body color** (Carapace blue / Spitter green /
  Bloater orange / Lunger magenta / boss dark-red) and a live **HP%** label above it (debug draw).
- **Brood-mother** can over-summon (its summons bypass the fodder soft-cap); tune `SummonCount`/cadence.
- Elites are kinematic and hold their spawn Z (no gravity) — place the objective at ground level.

## Tuning knobs
- Enemy stats: `UPROPERTY`s on `AAttackingElite` + each archetype's `default`s (HP, ranges, damage,
  telegraph, scale). Crawler: `AFodderEnemy` (MeleeDamage/AttackInterval/AttackRange).
- Roster/balance: `RaidObjective` → `EliteRoster`, `InitialEliteCount`, `BossClass`, `EliteSpawnRadius`,
  fodder-wave fields.
- Shooting: `WeaponDefinition` (spread/heat/recoil + focus fields).
