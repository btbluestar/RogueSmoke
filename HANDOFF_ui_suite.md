# Handoff — full UI suite (2026-06-10)

> 📦 **Historical handoff** — accurate for its date; later work (the D-0018–20 upgrade screens,
> input-config fixes) lives in `DECISIONS.md` and the `Script/UI/` file headers.

> **Update (same day): CommonUI compliance refactor (commit `6c42a18`).** The suite now follows
> canonical Common UI practice: `CommonGameViewportClient` is the viewport class (input routing
> actually runs), `URogueUILayout` is a per-player root with **Game / GameMenu / Menu layer
> stacks** — the only `AddToViewport` in the project — and every screen is a
> `UCommonActivatableWidget` pushed onto a layer. Input mode + cursor come from each screen's
> `GetDesiredInputConfig` (HUD = Game+capture, upgrade overlay = All so you can fight while
> choosing, menus = Menu+cursor); all manual `bShowMouseCursor` code is gone. **Esc/gamepad-B
> (the CommonUI back action) now closes the escape menu**; screens close via `DeactivateWidget()`.
> Focus targets are set per screen, keeping the gamepad door open. Remaining SHOULD-tier item:
> migrating `UButton` → `UCommonButtonBase` + style classes — deferred until after your visual
> PIE pass (AS-defined style brushes can't be verified headless). Details in
> `Script/UI/RogueUILayout.as` header + memory `commonui-in-as-fork`.

Built the complete front-end + in-run UI pass (tasks #50–#54, plan in `UI_PLAN.md`): main menu,
hero-select lobby with ready/start, data-driven upgrade cards, end-of-run results with per-player
stats, escape menu, and join/leave toasts. **All committed on `main`; C++ + script compile clean;
smoke suite 7/7 PASS after every phase.**

## TL;DR — how to see it
- **Full flow (closest to the shipped game):** launch with `-game` (no map arg) → **L_MainMenu**
  → HOST GAME → lobby: click a hero tile, READY, START RAID → 3s countdown → RaidArena as your
  chosen hero → clear/`RaidKillElites`/`RaidWin` → results panel → PLAY AGAIN / RETURN TO LOBBY.
- **In PIE:** open `L_MainMenu` (or `L_Lobby`, or jump straight into `RaidArena` — direct play
  still works; you just get the default Vanguard).
- **Escape menu:** press **P** in PIE (Esc also works but stops PIE in-editor), or type `RaidPause`.
- **Console debug:** `RaidWin` / `RaidLose` / `RaidResults` (instant results panel) /
  `LobbyForceStart` (in the lobby: pick hero 0 + ready + launch, no clicking).

## What was built (everything is a runtime-built widget tree — no UMG layouts to maintain)
| Piece | File(s) | Notes |
|---|---|---|
| Shared theme | `Script/UI/UITheme.as` | palette, rarity colors (grey/blue/purple/orange), MakeText/MakePanel/MakeTextButton |
| **Upgrade cards** | `UpgradeCardWidget.as`, `UpgradeSelectWidget.as` | ONE reusable card class, populated at runtime from `URogueUpgradeDef` (icon slot, name, **ValueText** stat line, rarity frame, description, `[1][2][3]` hotkeys). No pause. All 8 `DA_Upgrade_*` got real value lines from their GE numbers |
| Results screen | `ResultsScreenWidget.as` | RAID COMPLETE / SQUAD WIPED, time+seed, per-player stat columns + TEAM totals, PLAY AGAIN (host) / RETURN TO LOBBY / LEAVE / QUIT. HUD escalates banner→panel 2.5s after the phase resolves |
| Per-player stats | `RoguePlayerState` (C++) | replicated Kills / DamageDealt / DamageTaken / Downs / Revives / Upgrades; credited in `HealthComponent::ApplyDamage` (one funnel, pool-safe kill credit), `ApplyDamageToPlayer`, DownComponent (reviver gets the revive), `Server_ApplyUpgrade` |
| Main menu | `MainMenuWidget.as`, `MenuGameMode/PC`, `L_MainMenu` | HOST / JOIN (IP row) / QUIT; `GameDefaultMap` → L_MainMenu, `EditorStartupMap` → RaidArena |
| **Hero-select lobby** | `LobbyWidget.as`, `LobbyHeroTile.as`, `LobbyPlayerController.as`, `LobbyGameMode.as`, `L_Lobby` | lobby IS the hero select (RoR2): tiles from the `RogueHeroes` code roster, duplicates blocked server-side, squad panel, READY, host START gated on all-ready, replicated 3s countdown, ServerTravel |
| Chosen-hero spawn | `RaidGameMode.as`, `RaidPlayerController.as`, `RaidSessionSubsystem.as` | players start as spectators; pick survives travel in the GameInstance subsystem; `Server_SetHeroChoice` → embody ONCE (avoids ability-set double-grant on the PlayerState ASC). Direct PIE = default Vanguard |
| Escape menu + toasts | `EscapeMenuWidget.as`, HUD | nothing pauses; RESUME / RETURN TO LOBBY (host) / LEAVE / QUIT; "Player joined/left" toasts off PlayerArray |

## Verified headless (the strong one: the whole chain in ONE boot)
`L_Lobby -ExecCmds="LobbyForceStart"` → lobby shown → launch in 3s → **ServerTravel** →
RaidArena → run started → **hero embodied: choice=0 (VANGUARD)** → 4 ring elites + boss spawned.
Also: menu boots from bare `-game`; `RaidWin`+`RaidResults` constructs the results panel
(victory=true); `RaidPause` constructs the escape menu; smoke suite 7/7 after each phase.

## Needs your eyes (interactive PIE — #55)
Layout/spacing/readability of every screen (headless proves construction, not looks); clicking
cards + hotkeys mid-fight; two-player lobby (second PIE window or JOIN by IP): pick replication,
duplicate blocking, both heroes spawning, per-player stat columns on the results screen.

## Known MVP caveats
- **PLAY AGAIN** uses OpenLevel (drops remote clients — they rejoin). RETURN TO LOBBY ServerTravels
  and keeps the squad; prefer it in co-op. (Next polish: make PLAY AGAIN ServerTravel too.)
- Esc stops PIE in-editor — use **P** there. Both work in `-game`/packaged.
- Upgrade card icons are rarity-tinted blocks until art exists (`URogueUpgradeDef.Icon` is wired).
- Remote-client damage-taken stat counts pre-mitigation damage (armor/shield resolve later).
- The legacy `WBP_MainMenu` / `WBP_Lobby` / `WBP_UpgradeSelect` shells are unused now (widgets are
  created from the script classes directly); safe to delete in an editor session sometime.
