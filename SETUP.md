# Setup

> How to get RogueSmoke building and running from a clean machine. The AngelScript fork needs a
> **source build of Unreal**, which is the painful part — follow this once, then you're set.
> Source of truth: https://angelscript.hazelight.se/getting-started/installation/

## 0. Prerequisites (Windows)

- Visual Studio (with the "Game development with C++" workload) — needed to build the engine
  and the C++ hot-path module.
- ~150+ GB free disk and an hour-plus for the first engine build.
- Git + Git LFS (large binaries belong in LFS).
- An Epic Games account linked to GitHub for Unreal source access.

## 1. Get Unreal Engine source access

The AngelScript integration requires direct changes to engine code, so you need source access.
Follow Epic's guide to link your accounts: https://www.unrealengine.com/en-US/ue-on-github

## 2. Build the AngelScript engine fork

- Clone the Hazelight fork: https://github.com/Hazelight/UnrealEngine-Angelscript
- **Confirm it has a stable UE 5.7 branch** before committing (D-0001). If 5.7 isn't ready,
  decide explicitly whether to wait or pin an earlier version.
- Build the editor in Visual Studio as usual.
- Note: any extra engine plugins must be **built from source** — this fork is not compatible
  with pre-built binary plugins.

## 3. Install the VS Code extension

- Install VS Code: https://code.visualstudio.com/
- Install the **Unreal Angelscript** extension (publisher: Hazelight) from the marketplace.
  It provides autocomplete, go-to-definition, and live debugging against the running editor.

## 4. Open the project

- Project root: `C:\Users\btblu\Documents\RogueSmoke` (contains `RogueSmoke.uproject`).
- Open `RogueSmoke.uproject` with the **custom** editor you built (not a launcher engine).
- The plugin auto-creates a `Script/` folder. From the editor: **Tools -> Open Angelscript
  workspace** to open `Script/` in VS Code.

## 5. Prove the pipeline before writing gameplay

Do these in order; don't skip ahead — each rules out a class of "is it me or the engine" bugs.

1. **Hot-reload smoke test.** Add a trivial `ASmokeTestActor` that `Print`s on `BeginPlay`.
   Place it, hit Play, confirm the print. Edit the message, save, confirm hot-reload updates it
   live. If this doesn't work, stop and fix the toolchain first.
2. **Verify API signatures against the running compiler.** Helper namespaces (`System::`,
   `Math::`), subsystem `::Get` accessors, and Mass APIs vary by fork/engine version. The MVP
   doc's code is correct *patterns*, not guaranteed signatures — the editor + compiler are the
   source of truth.
3. **Stand up the seam.** Create `UCombatSubsystem` (C++) with stub implementations that just
   `GetAllActorsOfClass` elites. Get the seam compiling and callable from script before touching Mass.
4. **First combo on Actors only.** Implement Vanguard + Bombardier + taunt/barrage against the
   stub subsystem; prove the synergy on Actor enemies.
5. **Spike Mass.** Add fodder as Mass agents, route them through the same subsystem queries, and
   compare against your enemy-count target (D-0003, GDD §11.2).
6. **Slice it.** Add the synergy upgrade, a minimal upgrade-select widget, a raid objective +
   basic extraction, then run a host-authoritative 2-player test (see `TESTING.md`).

## 6. Daily workflow

- Edit `.as` files in VS Code; save to hot-reload (non-structural changes reload live in PIE).
- Edit C++ (`Source/RogueSmoke/`) → rebuild in Visual Studio → relaunch editor.
- Keep commits small and buildable; never commit with broken script compilation (CODING_STANDARDS §10).

> This guide is a strong candidate to graduate into a Claude Code **build-and-run skill** once
> the steps stabilize, so the agent (and CI) follow the exact recipe instead of rediscovering it.
