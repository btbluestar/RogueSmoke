# Multiplayer Setup (Core + Session Layer)

How to wire the script layer added for the menu → lobby → run flow into content. The **logic**
lives in AngelScript (`Script/Core/`, `Script/UI/`); this doc covers the **content** half
(maps, Blueprints, project settings) per the Blueprint/script boundary (CODING_STANDARDS §6).

Backend for the MVP is **LAN / direct IP** over the built-in NULL online subsystem — no Steam
or EOS config needed (D-0012). Friends join by typing the host's IP.

## What the scripts give you

| Script class | File | Role |
|---|---|---|
| `ARaidGameState` | `Core/RaidGameState.as` | Replicated run state: `MasterSeed`, `Phase` (`ERunPhase`), `FloorNumber`, `SharedScore`, player count. |
| `URunManager` | `Core/RunManager.as` | Server-only seed roll + run state machine. Owns the seeded `FRandomStream` (D-0007). |
| `ARaidGameMode` | `Core/RaidGameMode.as` | Server-only raid rules. Creates `URunManager`, rolls the seed on `BeginPlay`. |
| `ALobbyGameMode` | `Core/LobbyGameMode.as` | Lobby rules. `StartRaid()` ServerTravels the party into the raid map. |
| `URaidSessionSubsystem` | `Core/RaidSessionSubsystem.as` | Host (OpenLevel `?listen`) / Join (ClientTravel). The one file to change for Steam/EOS later. |
| `UMainMenuWidget` | `UI/MainMenuWidget.as` | Host / Join / Quit button handlers. |
| `ULobbyWidget` | `UI/LobbyWidget.as` | Player count + host-only Start button. |

## 1. Maps (create under `Content/Maps/`)

Create three maps. Names match the defaults baked into the scripts — if you name them
differently, update the matching `UPROPERTY` on the BP (see each step).

- **`L_MainMenu`** — tiny/empty. Boots here. No GameMode override needed.
- **`L_Lobby`** — staging map. World Settings → GameMode Override = `BP_LobbyGameMode`.
- **`L_Raid`** — the playable arena (you can promote `Levels/Prototyping/Test_1` or build new).
  World Settings → GameMode Override = `BP_RaidGameMode`. Place an `ARaidObjective` and your
  elites here as before.

## 2. Blueprints

All are asset-assignment only — **no logic** (CODING_STANDARDS §6).

- **`BP_RaidGameMode`** (child of `ARaidGameMode`)
  - `PlayerControllerClass` = `BP_RaidPlayerController` (your existing input-wired PC).
  - `DefaultPawnClass` = a `BP_HeroCharacter` (e.g. `BP_Vanguard`).
  - `GameStateClass` is already set in script — leave it.
- **`BP_LobbyGameMode`** (child of `ALobbyGameMode`)
  - `PlayerControllerClass` = `BP_RaidPlayerController` (or a lobby-only PC).
  - `RaidMapPath` = `/Game/Maps/L_Raid` (change if your raid map path differs).
- **`WBP_MainMenu`** (child of `UMainMenuWidget`)
  - Lay out **Host**, **Join**, **Quit** buttons + an editable **IP text box**.
  - Wire `OnClicked`: Host → `OnHostClicked`; Quit → `OnQuitClicked`;
    Join → `OnJoinClicked(<text box>.GetText().ToString())`.
  - `LobbyMap` defaults to `L_Lobby`; `DefaultJoinAddress` to `127.0.0.1` (one-click local test).
- **`WBP_Lobby`** (child of `ULobbyWidget`)
  - Bind a text block to `GetConnectedPlayerCount()`; wire **Start** → `OnStartClicked`.
  - Optional: bind Start's visibility/enabled to `IsLocalPlayerHost()`.

## 3. Show the widgets

The menu/lobby PlayerController must create and add the widget on `BeginPlay` (client side),
plus set input mode to UI and show the cursor. Either do it in `BP_RaidPlayerController`
(branch on current level) or make a tiny `BP_MenuPlayerController`. This is standard UMG
boilerplate — `Create Widget` → `Add to Viewport` → `Set Input Mode UI Only`.

## 4. Project settings (`Config/DefaultEngine.ini`)

Once `L_MainMenu` exists, point startup at it (currently the FirstPerson template map):

```ini
[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=/Game/Maps/L_MainMenu.L_MainMenu
GameDefaultMap=/Game/Maps/L_MainMenu.L_MainMenu
```

Leave `GlobalDefaultGameMode` empty/menu-appropriate; the Lobby and Raid maps set their own
GameMode via World Settings. **Don't** repoint these before the map exists — it breaks launch.

## 5. Test (TESTING.md)

1. PIE with **2 players, Net Mode = Play As Listen Server**, or two standalone instances.
2. Host instance: **Host** → lands in `L_Lobby` as listen server.
3. Second instance: **Join** with `127.0.0.1` (same machine) or the host's LAN IP.
4. Host: **Start** → both travel into `L_Raid`; check the log for
   `[RunManager] Run started — master seed N` (server only). Confirm the seed is identical
   on both machines via `ARaidGameState.MasterSeed` (it's `BlueprintReadOnly`, replicated).

## Notes / still-open hooks

- **Max party size** is still open (DECISIONS.md). `ALobbyGameMode::OnPostLogin` is where you'd
  reject joiners past the cap.
- **Steam/EOS**: swap only `URaidSessionSubsystem` — host/join callers don't change.
- The seed roll is the single permitted entropy draw; everything downstream must use
  `URunManager::GetStream()` (CODING_STANDARDS §5).
