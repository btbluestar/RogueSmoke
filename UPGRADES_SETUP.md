# Upgrades — authoring the pool (GE content)

The roguelike upgrade **flow** is built and verified (D-0013): on arena-clear the server rolls a
choice from a pool and shows each player a pick screen; choosing applies a GameplayEffect to the
player's ASC. What's left is **authoring the upgrade content** — the GameplayEffect assets — which
is editor work (you own this; the modifier UI is hard to drive from script).

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
