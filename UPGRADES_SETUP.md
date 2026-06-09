# Upgrades — authoring the pool (GE content)

> **STATUS (done):** 8 Tier-1 GEs are authored, fixed, and wired. The pool on `BP_RaidGamemode`
> points at 8 `URogueUpgradeDef` assets in `/Game/Upgrades` (Vitality, Bulwark, Overshield, Power,
> Quick Hands, Swift, Wide Barrage, Chain Detonation). All verified in PIE to move their attributes.
> This doc is kept as reference for authoring **more** upgrades / higher tiers.
>
> **Lesson learned (applies to any new GE):** for a *permanent* roguelike buff authored as an
> **INSTANT** effect, every modifier must use **ADD_BASE** (it permanently adds to the base value).
> **MULTIPLY_ADDITIVE on INSTANT multiplies the base** (e.g. MoveSpeed 600 × 0.15 = 90), which is
> almost never what you want. Use percentages only with **Infinite**-duration GEs. Flat bonuses that
> start at 0 (BarrageRadiusBonus) must be ADD_BASE regardless. A `MoveSpeed` GE also relies on the
> hero's `OnMoveSpeedChanged` callback (now wired) to take effect live.

The roguelike upgrade **flow** is built and verified (D-0013): on arena-clear the server rolls a
choice from a pool and shows each player a pick screen; choosing applies a GameplayEffect to the
player's ASC. Authoring **more** upgrade content (the GameplayEffect assets) is editor work.

## How the flow works (already wired, don't change)
- `ARaidGameMode.UpgradePool` (`TArray<URogueUpgradeDef>`) + `OptionsPerOffer` — assigned on
  **BP_RaidGamemode**. `OfferUpgradesToAll()` rolls distinct options (seeded, salted per offer so it
  never perturbs the master stream) and calls `Client_OfferUpgrades` on each player.
- **BP_RaidPlayerController.UpgradeWidgetClass** = `WBP_UpgradeSelect` (a child of
  `UUpgradeSelectWidget`) — shown on the owning client. Its buttons call `ChooseUpgrade(Index)`
  (already on the AngelScript parent) → `Server_ApplyUpgrade` → the GE applies.
- Trigger: `RaidObjective` → `ExtractionReady` calls `OfferUpgradesToAll()`. (Add more triggers —
  per elite kill, on a timer — by calling that function from anywhere server-side.)
- Each `URogueUpgradeDef` (a Data Asset) carries `DisplayName`, `Description`, `Rarity`, and the
  `Effect` (a `TSubclassOf<UGameplayEffect>`).

## What to author: the GameplayEffect assets
Make a **Blueprint Class → GameplayEffect** for each upgrade (Content/Blueprints/, prefix `GE_`).
Set **Duration Policy = Infinite** (permanent run buff), then add **Modifiers**:

| Upgrade (example) | Attribute (`URogueHealthSet` / `URogueCombatSet`) | Op | Magnitude |
|---|---|---|---|
| Vitality | `RogueHealthSet.MaxHealth` | Add | +50 |
| Bulwark | `RogueHealthSet.Armor` | Add | +25 |
| Overshield | `RogueHealthSet.MaxShield` (+ a Shield top-up) | Add | +50 |
| **Chain Detonation** (synergy) | `RogueCombatSet.BarrageClusterBonus` | Add | +1.0 |
| **Wide Barrage** (synergy) | `RogueCombatSet.BarrageRadiusBonus` | Add | +150 |
| Power | `RogueCombatSet.AbilityPower` | Add | +0.25 |
| Cooldown | `RogueCombatSet.CooldownReduction` | Add | +0.15 |

> The **synergy** upgrades are the priority (GDD §6.2): `GA_Barrage` already reads
> `BarrageRadiusBonus`/`BarrageClusterBonus`, so those visibly grow the payoff. Verified: the *grant*
> path works; the existing `GE_Upgrade_ChainDetonation` is currently an **empty shell (0 modifiers)** —
> add the `BarrageClusterBonus +1` modifier to make it do something.

> **MoveSpeed caveat:** a `+MoveSpeed` GE won't take effect live yet — the locomotion component seeds
> base speed once on possession. Wire a `MoveSpeed` attribute-changed callback to re-`SetBaseSpeed`
> before shipping move-speed upgrades.

## Then wire the pool
For each GE, point a `URogueUpgradeDef` at it (the `Effect` field) and add it to
`BP_RaidGamemode.UpgradePool`. Placeholder defs exist at `/Game/Upgrades/DA_Upgrade_Test{A,B,C}`
(currently all point at the empty shell) — repoint them or make new ones for 6–10 total.

The `WBP_UpgradeSelect` widget currently has no layout — add N buttons (one per option) whose
`OnClicked` call `ChooseUpgrade(0/1/2)`, plus a text block per option bound to the offered upgrade's
`DisplayName`/`Description`.
