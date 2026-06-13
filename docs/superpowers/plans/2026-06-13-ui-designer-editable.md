# Make the UI Editable by the Designer (you)

> **Status: COLOR PHASE DONE** (commit `1e778b1`, 2026-06-13; `run_code_test` clean, SmokeTest
> 11/11). The whole UI palette is now editable via `DA_UITheme`. **Remaining (optional follow-up):**
> sizes/font scales are still code consts — see "Notes" below. Verify any further work with
> `run_code_test` + `SmokeTest.ps1`.

**Goal:** Let the user tweak the UI's *look* (colors, sizes, fonts, key spacing) in the Unreal
editor without prompting Claude to edit code. (User pain: "so I don't need to tweak the look by
promoting [prompting] you.")

## Findings (verified 2026-06-13)

The **entire UI is built procedurally in AngelScript** — `git grep` shows **0** `UPROPERTY(BindWidget)`
and `ConstructWidget` in every `Script/UI/*.as` (HUD 19, menus/lobby/results/upgrade-cards 2–5 each).
The `WBP_*` assets are empty BP children. So **none** of the look is editable in the editor today.

Why it ended up this way (`RogueHUDWidget.as` header): MCP/python can't author UMG widget trees, so
the HUD was hand-built in code. That optimized for the *agent's* tooling limit at the *user's*
expense — backwards, since the user can author UMG and wants to.

There is already a half-step: `UITheme.as` (`RogueUITheme` namespace) centralizes the color palette
+ construct helpers — but as code `const`s, not editor-exposed (and `RogueHUDWidget` duplicates its
own `Accent`/`Danger`/`ShieldColor` instead of using it).

## Two tiers of "editable"

1. **Look/style** (colors, sizes, font scales, key offsets) — covers ~all of "tweak the look."
   **Solvable by Claude, non-breaking.**
2. **Layout** (moving widgets around, adding/removing elements) — needs real UMG authoring in the
   designer, which **only the user can do** (MCP can't build widget trees). Out of scope until the
   user wants to convert specific screens to `BindWidget`.

## Recommended approach (Tier 1): a UI theme DataAsset

A `UDataAsset` the widgets read style from, so editing one asset restyles the whole UI — matching
the project's own data-driven boundary (CODING_STANDARDS §5/§6; `DA_` convention). Claude *can*
author this (it's a class + a `.uasset` with property values, not a UMG tree).

- [ ] **Define `URogueUIThemeData : UDataAsset`** (`Script/UI/UIThemeData.as`) with
      `UPROPERTY(EditAnywhere)` fields: palette (`Accent`, `Danger`, `Victory`/`Shield`,
      `TextPrimary`, `TextDim`, `PanelDark`, `BackdropDim`), rarity colors (array or 4 fields),
      and the high-traffic sizes/scales (health/shield bar size, result-banner scale, crosshair
      base, stamina pip size/gap, edge-arrow scale, damage-number life/rise). Defaults = the
      current literals (so look is identical out of the box → non-breaking).
- [ ] **Resolve one instance** the widgets can reach. Simplest: a `URogueUIDeveloperSettings`
      (`UDeveloperSettings`) with a `TSoftObjectPtr<URogueUIThemeData>`, or a fixed path load in a
      UI helper (`RogueUITheme::Get()`), cached. (DeveloperSettings is cleaner — shows under
      Project Settings and is itself editable.)
- [ ] **Point the widgets at it:** replace the `RogueUITheme` namespace `const`s and
      `RogueHUDWidget`'s duplicated palette/size consts with reads from the resolved theme asset.
      Keep behavior identical when the asset is missing (fall back to the current literals).
- [ ] **Author `DA_UITheme.uasset`** with the current values; wire it in the developer settings.
- [ ] **Verify:** `run_code_test` clean; `SmokeTest.ps1` 11/11 (UI boots; no regression). Then the
      user edits `DA_UITheme` → all screens retint/resize live, no code.

## Notes
- This removes the palette duplication as a bonus.
- Font *family/size* swaps want a Slate font asset on the theme too (UMG default font is used now).
- Per-screen layout edits remain a later, user-driven UMG pass (convert that screen to `BindWidget`,
  author the tree in the designer, strip its `ConstructWidget` block).
- Standing exclusions still apply; `F:\UEAS` read-only.
