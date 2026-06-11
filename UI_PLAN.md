# UI Suite Plan — main menu, hero select, upgrades, results (2026-06-10)

> ✅ **Executed.** The suite shipped and was then refactored onto CommonUI layer stacks
> (D-0016; results in `HANDOFF_ui_suite.md`). Historical plan — kept for the research and
> rationale; don't implement from it.

Plan for the full UI pass, per asset. Grounded in (a) genre research — Risk of Rain 2, Gunfire
Reborn, Hades, Deep Rock Galactic, Helldivers 2 — and (b) a codebase audit of what already
exists. Tasks #50–#54.

## Ground rules (apply to every screen)

- **Runtime-built widget trees in AngelScript** (the proven `RogueHUDWidget` pattern:
  `OnInitialized → BuildLayout → ConstructWidget/SetRootWidget`). No UMG designer layouts —
  this sidesteps the wedged editor MCP entirely and keeps logic in script (CODING_STANDARDS §6).
  Existing `WBP_*` shells stay only as class references where a BP pointer is already wired.
- **Programmer art**: solid colors, borders, text. One shared theme file so a later art pass
  touches one place.
- **Server authority**: every choice (upgrade pick, hero pick, ready, start) is a `Server_` RPC
  validated server-side; UI reads replicated state.
- **Nothing pauses in co-op** (RoR2/Gunfire convention) — upgrade choice and the escape menu are
  overlays over live gameplay.
- Verification per phase: `run_code_test` (AS), `ue-cpp build` (C++), `Tools/SmokeTest.ps1`
  (regression), headless `-game` boots for flow breadcrumbs. Visual feel = user PIE pass at the end.

## What already exists (audit result)

| Piece | State |
|---|---|
| `RogueHUDWidget.as` | Full runtime-built HUD (crosshair/bars/markers/timer/result banner) |
| `UpgradeSelectWidget.as` | Logic only — `ChooseUpgrade(i)` → `Server_ApplyUpgrade`; **no card visuals** |
| `MainMenuWidget.as` / `LobbyWidget.as` | CommonActivatableWidget controllers w/ Host/Join/Quit + Start logic; **no layout** |
| `RaidSessionSubsystem.as` | Host (`OpenLevel listen`) / join-by-IP seam — done |
| `LobbyGameMode.as` | `ServerTravel` to raid — done, but points at nonexistent `/Game/Maps/L_Raid` |
| `URogueUpgradeDef` | DisplayName/Description/Rarity/Effect — **no icon, no short value line** |
| Upgrade offer flow | Arena clear → `OfferUpgradesToAll` → `Client_OfferUpgrades(Options)` — done |
| Heroes | `AVanguard`/`ABombardier` + BPs; **no selection mechanism, no hero def asset** |
| Levels | `RaidArena` + debug levels; **no L_MainMenu, no L_Lobby** |
| Stats | `SharedScore`, run clock; **no per-player stats** |
| `ARoguePlayerState` (C++) | ASC host — natural home for replicated per-player stats |

---

## 1. Shared foundation — `Script/UI/UITheme.as` (#50)

Namespace with: the palette (HUD teal/red/shield-blue + **rarity colors**: T1 grey-white,
T2 blue, T3 purple, T4 orange — the Hades convention players already know) and construction
helpers (`MakeText`, `MakeButton` w/ bound handler, `MakeBorder`, `MakePanelBackdrop`) so every
screen composes the same styled parts.

## 2. Upgrade card widget (#50) — the centerpiece

Research consensus (Gunfire Reborn ascensions, Hades boons): horizontal row of **3 cards**,
each showing icon, name, rarity color, concrete numbers *on the card* (RoR2's icons-only
tooltips are the known anti-pattern), hotkey hints, **game keeps running**, each player picks
independently.

- **`UUpgradeCardWidget`** — ONE class, no per-upgrade widgets (user requirement). Card ≈
  280×420: rarity-colored border + faint tint fill → icon area (texture if set, else solid
  rarity block) → name → rarity/“PERSONAL/TEAM/SYNERGY” tag → value line (“+25 Max Health”) →
  description → `[1]` hotkey hint. `Populate(URogueUpgradeDef, index)` fills it at runtime;
  click or hotkey routes back to the select screen.
- **`URogueUpgradeDef` additions**: `UTexture2D Icon`, `FText ValueText` (the short stat line —
  the card's "value"). Existing 8 `DA_Upgrade_*` get ValueText via a python commandlet.
- **`UpgradeSelectWidget` rework**: dimmed full-screen backdrop, “CHOOSE AN UPGRADE” title,
  centered `UHorizontalBox` of cards (one per offered def — supports 2–4 automatically),
  keyboard 1..N + mouse, closes on pick (existing PC cursor bookkeeping reused).

## 3. Per-player stats + results screen (#51)

Research: DRG/RoR2 show a team result + per-player breakdown; per-player damage is the
most-missed stat in DRG — include it.

- **C++ `ARoguePlayerState`**: replicated `Kills, DamageDealt, DamageTaken, TimesDowned,
  Revives, UpgradesTaken` + server-only `Add*` helpers. Accumulation hooks: `UCombatSubsystem`
  (instigator already flows through `FireHitscan`/`ApplyRadialDamage`/`ApplyDamageToPlayer`;
  kill credit where health hits 0), down/revive system, `Server_ApplyUpgrade`.
- **`ResultsScreenWidget.as`**: full-screen panel on `Phase → Victory/Defeat` (short delay so
  the existing banner still lands): **RAID COMPLETE** (green) / **SQUAD WIPED** (red), subtitle
  `time · seed · score`; per-player columns (name header, stat rows), team-total column;
  buttons: **PLAY AGAIN** (host-only → fresh travel to the raid map; clients see “waiting for
  host”), **RETURN TO LOBBY** (host → ServerTravel L_Lobby), **QUIT TO MENU** (local).

## 4. Main menu (#52)

Research: brutally minimal is the genre norm (RoR2). Vertical stack, no server browser.

- **Layout** (rework `MainMenuWidget` to runtime tree): title “ROGUESMOKE”, then
  **HOST GAME / JOIN GAME / QUIT**; JOIN expands an IP `UEditableTextBox` (default 127.0.0.1)
  + CONNECT. Wired to the existing `RaidSessionSubsystem` calls.
- **`L_MainMenu`** level (python commandlet): empty-ish map, GameMode override →
  **`AMenuGameMode`** (new, tiny): spectator default pawn, PC shows cursor + UI-only input +
  creates the menu widget. `DefaultEngine.ini GameDefaultMap → L_MainMenu` so a packaged/
  `-game` boot lands on the menu. Editor startup map unchanged.

## 5. Hero select lobby + start raid (#53)

Research: the lobby IS the hero select (RoR2); picking = ready signal; duplicates blocked
(fits the setup/payoff synergy design better than Gunfire's duplicates-allowed); host start
gated on everyone ready, then a 3s countdown (DRG drop-pod beat).

- **`URogueHeroDef`** data asset class: DisplayName, RoleTag (SETUP/PAYOFF), TintColor,
  AbilityLines, `TSoftClassPtr<AHeroCharacter>` PawnClass. Assets `DA_Hero_Vanguard`,
  `DA_Hero_Bombardier` (python).
- **Lobby flow**: replicated `SelectedHeroIndex`/`bReady` per player; tile click →
  `Server_SelectHero(i)` (server rejects duplicates) → everyone's UI refreshes (slot chips
  top-right show each player's pick). Host's **START RAID** enables when all ready → replicated
  3s countdown → `LobbyGameMode.StartRaid()` (`RaidMapPath` fixed to `/Game/Levels/RaidArena`).
- **Surviving ServerTravel** (non-seamless travel rebuilds PlayerStates): the local choice is
  stashed in a **GameInstance subsystem** (persists per machine across travel); after travel the
  PC re-sends `Server_SetHeroChoice`, and the raid GameMode **respawns that player's pawn as the
  chosen hero class**. Default remains BP_Vanguard if nothing chosen (solo quick-play still works).
- **`L_Lobby`** level (python): small floor + the lobby UI; GameMode override → LobbyGameMode
  child with HeroDefs + RaidMapPath set.

## 6. Extras (#54)

- **Escape menu** — overlay, nothing pauses: Resume / Return to Lobby / Leave Game / Quit.
- **Join/leave toasts** on the HUD (mandatory for a listen server).
- Stretch: “P2 is choosing…” marker during offers; Tab scoreboard (per-player upgrades+HP) —
  high value for a synergy game, after the core suite.

## Build order & verification

#50 cards → #51 stats/results → #52 menu → #53 lobby → #54 extras. Each phase: compile checks +
smoke suite before moving on; flow-level headless boots (menu boots, host→lobby travel, RaidWin
results) where breadcrumbs can prove it. Final interactive feel pass = user PIE (task #43/#45).
