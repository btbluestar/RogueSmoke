# ABP_Hero Build Guide — click-by-click

> **Who this is for:** a motivated non-animator building the hero's layered animation setup
> in the Unreal Editor 5.7 GUI. You need to know how to open assets and drag pins — everything
> else is spelled out below. Node names are written EXACTLY as they appear in the editor's
> right-click node search.
>
> **What you're building:** `BS_Rifle_Strafe` (a strafe blendspace) and `ABP_Hero` (a layered
> Animation Blueprint) so the hero's legs run/strafe/slide while the upper body independently
> fires, reloads, and aims at the crosshair — the "linked body" fix from the feel plan
> (`docs/superpowers/plans/2026-06-11-movement-shooting-feel.md`, Tasks 7–8).
>
> **Source recipe:** research notes `docs/superpowers/research/2026-06-11-feel-research-notes.md`
> §C.3. This guide expands that recipe; where they differ it's noted inline.
>
> **All graph variables already exist.** They are computed in AngelScript
> (`RogueSmoke/Script/Player/HeroAnimInstance.as`, class `URogueHeroAnimInstance`). You never
> create a variable or write Blueprint logic in this guide — you only read inherited variables
> and wire pins.

**Variables the graph reads** (exact names, from `HeroAnimInstance.as`):

| Variable | Type | Meaning |
|---|---|---|
| `GroundSpeed` | float | horizontal speed (cm/s) |
| `Direction` | float | -180..180, velocity vs facing — blendspace X |
| `StrafeSpeed` | float | `min(GroundSpeed, JogAuthoredSpeed)` — blendspace Y (authored units) |
| `PlayRate` | float | anti-foot-slide playback scale above the jog row, clamped 0.8–1.6 |
| `bShouldMove` | bool | moving AND accelerating (prevents idle-pop while stopping) |
| `bIsFalling` | bool | airborne |
| `VerticalVelocity` | float | velocity Z (cm/s, + = rising) |
| `bIsCrouching` | bool | **TRUE during slides too** (slide crouches the capsule) — see §4.6 |
| `bIsSliding` | bool | in a slide |
| `bIsSprinting` | bool | sprint held (not used by the v1 graph — see §4.9 note) |
| `AimPitch` | float | -90..90 aim delta vs body |
| `AimYaw` | float | ±90 aim delta vs body |
| `AimOffsetAlpha` | float | 1 normally, 0 while incapacitated |
| `LandRecoveryAlpha` | float | 1→0 after touchdown, scaled by fall speed |

Tunables on the same class: `JogAuthoredSpeed` (623 — see §1.4), `LandRecoverySeconds` (0.4).

**Assets used** (all verified to exist):

- Skeleton `/Game/Characters/Mannequins/Meshes/SK_Mannequin`, preview mesh `.../SKM_Manny_Simple`
- Walk strafes `/Game/Characters/Mannequins/Anims/Rifle/Walk/MF_Rifle_Walk_{Fwd, Fwd_Left, Fwd_Right, Left, Right, Bwd, Bwd_Left, Bwd_Right}`
- Jog strafes `/Game/Characters/Mannequins/Anims/Rifle/Jog/MF_Rifle_Jog_{same 8 suffixes}`
- Idle `/Game/Characters/Mannequins/Anims/Rifle/MF_Rifle_Idle_ADS`
- Aim offset `/Game/Characters/Mannequins/Anims/Rifle/AIM/AO_Rifle`
- Jump set `/Game/Characters/Mannequins/Anims/Rifle/Jump/MM_Rifle_Jump_Start`, `MM_Rifle_Jump_Apex`, `MM_Rifle_Jump_Fall_Loop`, `MM_Rifle_Jump_Fall_Land`, `MM_Rifle_Jump_RecoveryAdditive`
- GASP bakes `/Game/Characters/Mannequins/Anims/GASP/{Slide_Enter, Slide_Loop_FootOut, Slide_Exit_Run, Land_Run_Light, Land_Run_Heavy, Land_Stand_Light, Fall_Loop, Sprint_Loop, Sprint_Stop}`
- Older slide loop `/Game/Characters/Mannequins/Anims/Slide/Slide_KneesOut_Loop`
- Montages (already created, referenced in Part 2): `MTG_Rifle_Fire`, `MTG_Rifle_Reload`

---

## Part 1 — Create `BS_Rifle_Strafe`

### 1.1 Create the asset

- [x] In the Content Browser, navigate to `/Game/Characters/Mannequins/Anims/`.
- [x] Right-click empty space → **Animation** → **Blend Space**. (On some UE5 menu layouts it's
      under **Animation → Legacy → Blend Space**. Either way: **Blend Space**, NOT "Blend Space 1D".)
- [x] A skeleton picker opens. Select **SK_Mannequin** (`/Game/Characters/Mannequins/Meshes/SK_Mannequin`).
- [x] Name the asset **`BS_Rifle_Strafe`**. Double-click to open it.

### 1.2 Axis setup

- [ ] In the Blend Space editor, find the **Asset Details** panel (Window → Asset Details if hidden).
- [ ] Expand **Axis Settings → Horizontal Axis** and set:
  - **Name:** `Direction`
  - **Minimum Axis Value:** `-180.0`
  - **Maximum Axis Value:** `180.0`
  - **Number of Grid Divisions:** `8`  (grid lines every 45° — exactly the 8 strafe directions)
  - **Snap to Grid:** ✅ checked
  - **Wrap Input:** ✅ checked  ← this fixes the pop when Direction crosses ±180
  - **Smoothing Time:** `0.12`  (anywhere 0.1–0.15 is fine)
  - **Smoothing Type:** `Averaged`
- [ ] Expand **Axis Settings → Vertical Axis** and set:
  - **Name:** `Speed`
  - **Minimum Axis Value:** `0.0`
  - **Maximum Axis Value:** `623.0`  (the authored jog SPEED — see §1.4; displacement/length, not displacement)
  - **Number of Grid Divisions:** `2`  (rows at 0 / 311.5 / 623; samples are typed exactly, snap off — §1.2)
  - **Snap to Grid:** ✅ checked
  - **Smoothing Time:** `0.12`
  - **Smoothing Type:** `Averaged`

> **Why no samples at Speed 0?** The blendspace never sees a standing character — the state
> machine switches to the Idle state when `bShouldMove` is false. The walk row is the floor.

### 1.3 Place the 18 samples

- [x] Find the **Asset Browser** panel (bottom-right of the Blend Space editor; Window → Asset
      Browser if hidden). It lists every animation compatible with SK_Mannequin.
- [x] Drag each animation from the Asset Browser onto the grid at the position in this table.
      With Snap to Grid on, drops snap to grid intersections. The **walk row is the middle grid
      line**, the **jog row is the top line** (typed Y: walk 304, jog 623 — §1.4):

| Direction (X) | Walk sample (Y = **304**) | Jog sample (Y = **623**) |
|---|---|---|
| `0`    | `MF_Rifle_Walk_Fwd`       | `MF_Rifle_Jog_Fwd` |
| `45`   | `MF_Rifle_Walk_Fwd_Right` | `MF_Rifle_Jog_Fwd_Right` |
| `90`   | `MF_Rifle_Walk_Right`     | `MF_Rifle_Jog_Right` |
| `135`  | `MF_Rifle_Walk_Bwd_Right` | `MF_Rifle_Jog_Bwd_Right` |
| `180`  | `MF_Rifle_Walk_Bwd`       | `MF_Rifle_Jog_Bwd` |
| `-180` | `MF_Rifle_Walk_Bwd`       | `MF_Rifle_Jog_Bwd`  ← yes, Bwd goes at **BOTH** +180 and -180 |
| `-135` | `MF_Rifle_Walk_Bwd_Left`  | `MF_Rifle_Jog_Bwd_Left` |
| `-90`  | `MF_Rifle_Walk_Left`      | `MF_Rifle_Jog_Left` |
| `-45`  | `MF_Rifle_Walk_Fwd_Left`  | `MF_Rifle_Jog_Fwd_Left` |

  (Even with Wrap Input on, place `Bwd` at both edges — wrap interpolates across the seam, the
  duplicate samples anchor it.)
- [x] Verify every sample's exact coordinates: in **Asset Details → Blend Samples**, each entry
      shows its animation and X/Y values. Fix any mis-drop by editing the numbers directly.
- [x] Sanity check: hold **Ctrl** and move the mouse over the grid — the preview character blends
      between directions. Walking the green preview dot along the ±180 edge must NOT pop.
- [x] **Save**.

### 1.4 SIDEBAR — the authored speeds (RESOLVED — measured 2026-06-11)

The MF_Rifle clips were measured from their root tracks (`extract_root_track_transform`):
**walk = 304 cm/s** (467 cm / 1.533 s), **jog = 623 cm/s** (955 cm / 1.533 s). The blendspace
rows sit at exactly these values (walk row Y=304, jog row Y=623, axis max 623) and
`JogAuthoredSpeed = 623` in `HeroAnimInstance.as`.

⚠ **Speed = displacement ÷ clip length.** The first pass of this build read the root
*displacement* (955 cm) and entered it as the *speed* — every run speed then landed in the
walk-dominant part of the axis and forward locomotion read as a mushy power-walk. If these
clips are ever swapped, re-measure as `distance / Sequence Length`, never the raw distance.

Why authored-speed placement matters: with each sample at its true speed, the blendspace's own
interpolation makes foot speed match ground speed *exactly* at every input below the jog row
(crouch 300 ≈ walk row; focus 480 = mid-blend; run 600 ≈ jog row). `PlayRate` only engages
above the jog row (sprint 960 → 960/623 ≈ 1.54× jog).

---

## Part 2 — Register the `UpperBody` anim slot

- [ ] Open any animation asset on SK_Mannequin (e.g. `MF_Rifle_Idle_ADS`) — or the ABP once it
      exists. In the menu bar: **Window → Anim Slot Manager**.
- [ ] In the Anim Slot Manager panel, select the **DefaultGroup** row, then click **Add Slot**.
- [ ] Name it exactly **`UpperBody`** (it becomes `DefaultGroup.UpperBody`).
- [ ] Slots live on the **skeleton**: this dirties `SK_Mannequin`. **Save** it (Ctrl+Shift+S /
      Save All is fine).
- [ ] Check the montages: open `MTG_Rifle_Fire` — above the montage track there's a slot dropdown
      (it reads `DefaultGroup.DefaultSlot` or `DefaultGroup.UpperBody`). If it isn't
      **`DefaultGroup.UpperBody`**, change it via the dropdown, then save. Repeat for
      `MTG_Rifle_Reload`.

> **Why:** fire/reload montages target the `UpperBody` slot, which (in Part 5) is layered onto
> the body only above `spine_01` — so the legs keep running while the arms shoot. Full-body
> montages (deaths) use the default `DefaultGroup.DefaultSlot`.

---

## Part 3 — Create `ABP_Hero` and reparent it

- [ ] Content Browser → `/Game/Characters/Mannequins/Anims/` → right-click → **Animation →
      Animation Blueprint**.
- [ ] In the dialog, pick **Skeleton: SK_Mannequin**. Leave Parent Class as default for now
      (you reparent next). Click **Create**.
- [ ] Name it exactly **`ABP_Hero`**. Double-click to open.
- [ ] Toolbar → **Class Settings** button. In the **Details** panel that appears, under
      **Class Options → Parent Class**, open the dropdown and search **`RogueHeroAnimInstance`**.
      Select **URogueHeroAnimInstance** (this is the AngelScript class — it appears in the picker
      under that name).
- [ ] Click **Compile** (toolbar). Expect success (graph is still empty).
- [ ] Confirm the variables arrived: in the **My Blueprint** panel, click the **gear icon** (top
      right of the panel) and check **Show Inherited Variables**. Under Variables you must now see
      `GroundSpeed`, `Direction`, `StrafeSpeed`, `PlayRate`, `bShouldMove`, `bIsFalling`,
      `VerticalVelocity`, `bIsCrouching`, `bIsSliding`, `bIsSprinting`, `AimPitch`, `AimYaw`,
      `AimOffsetAlpha`, `LandRecoveryAlpha` (plus `JogAuthoredSpeed`, `LandRecoverySeconds`).
      **If they're missing, STOP — the reparent didn't take.** Redo the Class Settings step.

> **Rule for the whole graph:** every value comes from these inherited variables via plain
> variable-get nodes. No math nodes, no event graph logic — the AngelScript class computes
> everything (that's why animation logic hot-reloads without touching this asset).

---

## Part 4 — The `Locomotion` state machine

### 4.1 Create the machine

- [ ] Open the **AnimGraph** (My Blueprint panel → Animation Graphs → AnimGraph).
- [ ] Right-click empty canvas → search **`State Machine`** → select **Add New State Machine...**.
- [ ] Select the new node, press **F2**, rename it **`Locomotion`**.
- [ ] Do **NOT** wire it to Output Pose yet — Part 5 puts four nodes between them.
- [ ] Double-click the `Locomotion` node to enter it. You'll see an **Entry** node.

### 4.2 Add the states and the conduit

- [ ] Right-click the canvas → **Add State...** — repeat to create EXACTLY these states (F2 to
      rename): **`Idle`**, **`Cycle`**, **`JumpStart`**, **`FallLoop`**, **`Land`**, **`Slide`**,
      **`Crouch`**.
- [ ] Right-click the canvas → **Add Conduit...** → rename it **`ToAirborne`**.
- [ ] Drag from the **Entry** node's pin to **Idle** (Entry → Idle, no condition needed).

> An optional **Apex** state (`MM_Rifle_Jump_Apex` between JumpStart and FallLoop) exists in the
> source anim set. **Skip it in v1** — JumpStart blending into FallLoop reads fine. If you add it
> later: JumpStart → Apex on `VerticalVelocity < 100.0`, Apex → FallLoop on its automatic rule.

### 4.3 Fill each state with its animation

Double-click a state to open it, then build its contents. To place an animation player: right-click
the canvas inside the state and **search for the asset name** (e.g. typing `MF_Rifle_Idle_ADS`
offers **Play MF_Rifle_Idle_ADS**) — or drag the asset in from the Asset Browser. Wire the player's
output to the state's **Output Animation Pose** node.

**State: Idle**
- [x] Add **Play MF_Rifle_Idle_ADS**. Select the node; in **Details**: **Loop** ✅.
- [x] Details → **Sync** section: **Sync Group Name** = `Locomotion` (type it), **Sync Group Role**
      = `Always Follower`. (If your Details shows a **Method** dropdown in the Sync section, set it
      to `Sync Group` first — the Name/Role fields appear then.)

**State: Cycle** (the workhorse)
- [x] Add the blendspace player: right-click → search `BS_Rifle_Strafe` → **Blendspace Player
      'BS_Rifle_Strafe'**. The node has two float input pins named after the axes: **Direction**
      and **Speed**.
- [x] From My Blueprint, drag the **`Direction`** variable onto the canvas → **Get Direction** →
      wire to the node's **Direction** pin.
- [x] Drag **`StrafeSpeed`** → Get → wire to the **Speed** pin. (NOT `GroundSpeed` —
      `StrafeSpeed` is already clamped to the axis max; raw GroundSpeed would pin the blendspace
      while sprinting and fight the play-rate trick.)
- [x] Expose the Play Rate pin: select the blendspace player node → **Details** → find **Play
      Rate**. At the right edge of that row there's a small pin/dropdown control — choose **Expose
      as Pin**. A **Play Rate** input pin appears on the node.
- [x] Drag **`PlayRate`** → Get → wire to the **Play Rate** pin. (This is the anti-foot-slide:
      above jog speed the clip plays faster instead of the feet sliding.)
- [x] Details → Sync: **Sync Group Name** = `Locomotion`, **Sync Group Role** = `Can Be Leader`.
      ← the jog blendspace is the sync LEADER; idle and crouch follow its foot phase.

**State: JumpStart**
- [x] Add **Play MM_Rifle_Jump_Start**. Details: **Loop** ☐ (unchecked). No sync group.

**State: FallLoop**
- [x] Add **Play MM_Rifle_Jump_Fall_Loop**. Details: **Loop** ✅. No sync group.
      (GASP `Fall_Loop` also exists; the rifle-template clip matches the rifle pose set — prefer it.)

**State: Land**
- [x] Add **Play Land_Run_Light** (the GASP bake at `/Game/Characters/Mannequins/Anims/GASP/Land_Run_Light`).
      Details: **Loop** ☐. No sync group.
- [ ] Note for later (don't build now): `Land_Run_Heavy` and `Land_Stand_Light` exist for a
      fall-speed/movement-aware landing blend (e.g. **Blend Poses by bool** on `LandRecoveryAlpha`
      thresholds). v1 ships with the light landing; the additive recovery in Part 5 carries the
      impact weight.

**State: Slide**
- [x] Two candidate loops exist — **preview both and pick one**:
      `/Game/Characters/Mannequins/Anims/GASP/Slide_Loop_FootOut` and
      `/Game/Characters/Mannequins/Anims/Slide/Slide_KneesOut_Loop`. Open each asset, eyeball which
      silhouette you prefer with a rifle (FootOut = leg extended, KneesOut = tucked).
- [ ] Add **Play <your pick>**. Details: **Loop** ✅. No sync group.
      
- [ ] Note for later: `Slide_Enter` / `Slide_Exit_Run` exist for a 3-phase slide (enter → loop →
      exit). v1 uses the loop only.

**State: Crouch** — ⚠ known compromise
- [ ] The project has **no crouch strafe anim set** yet (a later GASP pull). The fallback: reuse
      the walk blendspace at crouch speed. Add **Blendspace Player 'BS_Rifle_Strafe'** exactly as
      in Cycle: **Direction** pin ← `Direction`, **Speed** pin ← `StrafeSpeed`, expose **Play
      Rate** ← `PlayRate`.
- [ ] Details → Sync: **Sync Group Name** = `Locomotion`, **Sync Group Role** = `Always Follower`.
- [ ] Crouch move speed is 300, so this plays a blend just above the walk row — standing-height
      walk poses while the capsule is crouched. Visually wrong-ish, mechanically fine; it's the
      documented stopgap until a crouch set is retargeted.

### 4.4 Wire the transitions

Create a transition by dragging from the **edge** of one state onto another. Double-click the
transition icon to open its rule graph; build the condition by dragging variables from My
Blueprint and wiring through logic nodes into the **Result** node (`Can Enter Transition`).
Node names as they appear in search: **NOT Boolean**, **AND Boolean**, **OR Boolean**,
**greater** (`>`), **less equal** (`<=`).

Set the **blend time** on each transition: select the transition icon → **Details** → **Blend
Settings → Duration**. Set **Priority Order** (same Details panel) where listed — lower number
wins when two conditions are true on the same frame.

| # | From → To | Rule graph (wire exactly this into Result) | Duration | Priority |
|---|---|---|---|---|
| T1 | Idle → Cycle | `bShouldMove` | 0.15 | — |
| T2 | Cycle → Idle | `NOT bShouldMove` | 0.25 | — |
| T3 | Cycle → Slide | `bIsSliding` | 0.15 | 2 |
| T4 | Slide → Crouch | `(NOT bIsSliding) AND bIsCrouching` | 0.2 | 2 |
| T5 | Slide → Cycle | `(NOT bIsSliding) AND bShouldMove` | 0.2 | 3 |
| T6 | Slide → Idle | `(NOT bIsSliding) AND (NOT bShouldMove)` | 0.25 | 4 |
| T7 | Idle → Crouch | `bIsCrouching AND (NOT bIsSliding)` | 0.2 | — |
| T8 | Cycle → Crouch | `bIsCrouching AND (NOT bIsSliding)` | 0.2 | 3 |
| T9 | Crouch → Cycle | `(NOT bIsCrouching) AND bShouldMove` | 0.2 | 2 |
| T10 | Crouch → Idle | `(NOT bIsCrouching) AND (NOT bShouldMove)` | 0.25 | 3 |
| T11 | Crouch → Slide | `bIsSliding` | 0.15 | 2 (landing slide re-entry chain) |
| T12 | JumpStart → FallLoop | ✅ **Automatic Rule Based on Sequence Player in State** (checkbox in transition Details — no graph needed) | 0.15 | — |
| T13 | FallLoop → Land | `NOT bIsFalling` | 0.1 (fast — touchdown must read instantly) | — |
| T14 | Land → Slide | `bIsSliding` | 0.1 | 2 (slide-hop: land with crouch held re-enters the slide) |
| T15 | Land → Cycle | `bShouldMove` | 0.2 | 3 |
| T16 | Land → Idle | ✅ Automatic Rule (checkbox, as T12) | 0.3 | 4 |

Reading the rule column: `NOT bShouldMove` means Get `bShouldMove` → **NOT Boolean** → Result.
`(NOT bIsSliding) AND bIsCrouching` means Get `bIsSliding` → NOT Boolean → into pin A of an
**AND Boolean**; Get `bIsCrouching` → pin B; AND output → Result.

> **T12 optional early-out:** the recipe allows leaving JumpStart early when descent starts.
> If the jump start visibly lingers, replace the automatic rule with a custom graph:
> right-click → **Time Remaining (ratio) (MM_Rifle_Jump_Start)** → **less** `0.1` → into an
> **OR Boolean** with (`VerticalVelocity` → **less** `0.0`) → Result. Keep the simple automatic
> rule unless you see the problem.

### 4.5 The `ToAirborne` conduit (one airborne doorway, no wire spaghetti)

Every grounded state can fall or jump; instead of wiring each one to both JumpStart and FallLoop
(10 transitions), they all funnel through the conduit (5 in + 2 out).

- [ ] Drag a transition from EACH of **Idle, Cycle, Crouch, Slide, Land** onto **ToAirborne**
      (5 transitions). In each one's rule graph: right-click the **Result** node's `Can Enter
      Transition` pin area — simply **check the boolean literal on the Result node** (click the
      checkbox on the Result input so it's constant true). Set **Priority Order = 1** on all five
      (airborne beats every ground-to-ground transition). Duration 0.1.
- [ ] Double-click the **ToAirborne** conduit itself — it has its own rule graph. Wire:
      Get `bIsFalling` → Result. (The shared condition lives HERE, once, instead of in five
      incoming rules. An incoming transition only fires if the whole path through the conduit
      evaluates true.)
- [ ] Drag **ToAirborne → JumpStart**. Rule: `VerticalVelocity` → **greater** → `100.0` → Result.
      Duration 0.1, **Priority Order 1**. (Rising fast = we jumped.)
- [ ] Drag **ToAirborne → FallLoop**. Rule: `VerticalVelocity` → **less equal** → `100.0` → Result.
      Duration 0.15, **Priority Order 2**. (Not rising = we walked off an edge.)

### 4.6 ⚠ CRITICAL — why Crouch gates on `bIsCrouching AND (NOT bIsSliding)`

`bIsCrouching` mirrors the CharacterMovement crouch flag, and **the slide crouches the capsule —
so `bIsCrouching` is TRUE during every slide**. If T7/T8 test `bIsCrouching` alone, the machine
shows the crouch-walk pose the instant a slide starts (Crouch outranks Slide or fights it every
frame). The `(NOT bIsSliding)` term is not optional. (This is a deliberate correction to the
research recipe's shorthand "Crouch ⇄ on bIsCrouching" — the recipe predates the variable's
exact semantics; see the comment on `bIsCrouching` in `HeroAnimInstance.as`.)

### 4.7 Sync group summary (set in 4.3, verify now)

| Player node | Group Name | Group Role |
|---|---|---|
| Cycle's `BS_Rifle_Strafe` | `Locomotion` | **Can Be Leader** |
| Idle's `MF_Rifle_Idle_ADS` | `Locomotion` | Always Follower |
| Crouch's `BS_Rifle_Strafe` | `Locomotion` | Always Follower |
| JumpStart / FallLoop / Land / Slide players | (none) | — |

> **Why:** the sync group keeps foot phase continuous when blending idle ⇄ cycle ⇄ crouch, so a
> half-second stop doesn't reset the run cycle to the left foot.

### 4.8 Compile checkpoint

- [ ] Back out to the AnimGraph and **temporarily** wire `Locomotion` → **Output Pose** (you'll
      cut this in Part 5). Click **Compile** — must succeed with no errors. Move the preview:
      open **Window → Anim Preview Editor**, set `bShouldMove` ✅ and `StrafeSpeed` `623` — the
      preview should jog forward in place.

### 4.9 Note: sprint

There is no Sprint state in v1 — sprint is just Cycle at higher `GroundSpeed`, absorbed by
`PlayRate` (that's what the clamp 0.8–1.35 is for). `bIsSprinting` and the GASP `Sprint_Loop` /
`Sprint_Stop` bakes are reserved for a later dedicated sprint state. Don't wire `bIsSprinting`
anywhere now.

---

## Part 5 — The post-state-machine spine (the layering chain)

This is the sequence of nodes between `Locomotion` and **Output Pose**. Build it left to right.
All nodes are placed in the main **AnimGraph** (right-click → search the bold name).

> The chain: cache the locomotion pose → split the body at the spine so montages drive only the
> upper half → bend the spine to the crosshair → stack the landing-impact additive → leave a
> full-body slot for deaths → output.

- [ ] **Node 1 — Save Cached Pose.** Delete the temporary wire from 4.8. Drag off the
      `Locomotion` node's output pin → search **New Save cached pose...** → place it. Select the
      new node, **F2**, rename the cache to **`LocomotionCache`**.
- [ ] **Node 2 + 3 — two cache readers.** Right-click empty canvas → search **Use cached pose
      'LocomotionCache'** (it appears in the list now that the save node exists) → place it.
      Repeat — you need **two** of these nodes.
- [ ] **Node 4 — the UpperBody slot.** Right-click → search **Slot 'DefaultSlot'** → place it.
      Select it → **Details → Slot Name** dropdown → **`DefaultGroup.UpperBody`** (registered in
      Part 2; if it's missing from the dropdown, Part 2 wasn't saved). Wire **Use Cached Pose #2
      → Slot's Source pin**.
- [ ] **Node 5 — Layered blend per bone.** Right-click → search **Layered blend per bone** →
      place it. Wire:
  - **Use Cached Pose #1 → Base Pose** pin.
  - **Slot 'DefaultGroup.UpperBody' output → Blend Poses 0** pin.
- [ ] Configure Node 5 (select it → Details):
  - **Layer Setup** → expand **index [0]** → **Branch Filters** → click **+** to add one entry →
    set **Bone Name:** `spine_01`, **Blend Depth:** `4`.
  - **Mesh Space Rotation Blend:** ✅ checked.
  - Leave **Blend Weights [0]** at `1.0`.
  - (Tuning alternative if the split looks too soft later: Bone Name `spine_03`, Blend Depth `2`
    — one field to change, stiffer hip/chest split. Ship with `spine_01`/4.)
  - **Why:** everything below `spine_01` ignores the montage — the upper body rides the lower
    body, so firing never interrupts the legs.
- [ ] **Node 6 — Aim Offset.** Right-click → search **`AO_Rifle`** → pick the aim-offset player
      entry for the asset (listed as **AimOffset Player 'AO_Rifle'** / **Play AO_Rifle** depending
      on context — it's the one whose node has two float axis pins plus Alpha). Wire **Layered
      blend per bone output → its Base Pose pin**. Then:
  - The two float pins are named after AO_Rifle's axes — the yaw/horizontal axis pin and the
    pitch/vertical axis pin. Wire Get **`AimYaw`** → the yaw axis pin, Get **`AimPitch`** → the
    pitch axis pin. (If unsure which is which, open AO_Rifle → Asset Details → Axis Settings and
    read the axis names.)
  - Wire Get **`AimOffsetAlpha`** → **Alpha** pin.
  - **Why this sits AFTER the layered blend:** Lyra-style — the aim bends the spine on top of
    whatever the upper body is doing, so the muzzle tracks the crosshair even mid-reload.
  - If the node outputs a mangled pose, jump to **Part 6** (the AO's additive settings are wrong),
    fix, and come back.
- [ ] **Node 7 — Apply Additive.** Right-click → search **Apply Additive** → place it. Wire
      **Aim Offset output → Base** pin. Then:
  - Right-click → search **Play MM_Rifle_Jump_RecoveryAdditive** → place it. Details: **Loop** ✅
    (the alpha does the gating, the clip just needs to keep outputting). Wire its output →
    the Apply Additive node's **Additive** pin.
  - Wire Get **`LandRecoveryAlpha`** → **Alpha** pin.
  - **Why:** the landing-impact crouch-and-recover is an additive layered over EVERYTHING
    (legs, torso, even mid-reload), scaled by how hard you hit, decaying over ~0.4 s.
- [ ] **Node 8 — the full-body slot.** Right-click → search **Slot 'DefaultSlot'** → place it.
      Leave Slot Name as **`DefaultGroup.DefaultSlot`**. Wire **Apply Additive output → Source**,
      and **Slot output → Output Pose's Result pin**.
  - **Why DefaultSlot is LAST:** full-body montages (death, big hit reacts, equips) play here and
    override the entire stack — a corpse must not keep aiming at the crosshair.
- [ ] **Compile.** Zero errors, zero warnings about missing bones/slots. **Save.**

Final chain, for checking your wiring:

```
Locomotion ─ Save Cached Pose 'LocomotionCache'

Use Cached Pose #1 ──────────────► Base Pose ┐
                                             ├ Layered blend per bone ► AimOffset 'AO_Rifle' ► Apply Additive ► Slot 'DefaultGroup.DefaultSlot' ► Output Pose
Use Cached Pose #2 ► Slot          ┌─────────┘      ▲  ▲  ▲                ▲          ▲
  'DefaultGroup.UpperBody' ► Blend Poses 0          │  │  │                │          │
                                       AimYaw ──────┘  │  │                │          │
                                       AimPitch ───────┘  │                │          │
                                       AimOffsetAlpha ────┘                │          │
                  Play MM_Rifle_Jump_RecoveryAdditive (loop) ──────────────┘          │
                  LandRecoveryAlpha ──────────────────────────────────────────────────┘
```

---

## Part 6 — Verify `AO_Rifle`'s additive settings

Aim offsets only work if their source poses are **Mesh Space additive**. Check before trusting
Part 5's Node 6:

- [ ] Open `/Game/Characters/Mannequins/Anims/Rifle/AIM/AO_Rifle`.
- [ ] In **Asset Details**, find **Additive Settings** (or the **Preview Base Pose** field):
      **Preview Base Pose** should be **`MF_Rifle_Idle_ADS`**. If it's None, set it (this only
      affects the preview, but None usually signals the set was never configured).
- [ ] In **Asset Details → Blend Samples**, note the animation asset assigned to each sample.
      Open ONE of them (double-click the sample's animation).
- [ ] In that pose asset's **Asset Details → Additive Settings**, verify:
  - **Additive Anim Type:** `Mesh Space`
  - **Base Pose Type:** `Selected animation frame` (or `Selected animation`)
  - **Base Pose Animation:** `MF_Rifle_Idle_ADS`
  - **Ref Frame Index:** `0` (when using a frame)
- [ ] **If wrong** (Additive Anim Type says `No additive` or `Local Space`): set the four fields
      exactly as above, **Save** the pose asset, and repeat for EVERY sample animation listed in
      AO_Rifle's Blend Samples. (Symptoms of skipping this: the AO editor rejects samples with a
      "must be mesh space additive" warning, or in-game the torso explodes/offsets weirdly when
      looking up or down.)
- [ ] Back in AO_Rifle, move the preview crosshair around the sample grid — the character should
      smoothly aim the rifle up/down/left/right around the ADS idle. **Save.**

---

## Part 7 — Final verification checklist

- [ ] **Compile** `ABP_Hero` — green check, no warnings. **Save All.**
- [ ] **Preview mesh:** in the ABP editor, **Window → Preview Scene Settings** → **Preview Mesh**
      → `SKM_Manny_Simple`. (If the viewport already shows Manny via the skeleton default, fine.)
- [ ] **Scrub the variables live:** **Window → Anim Preview Editor**. This panel lists every
      inherited variable and lets you type values; in editor preview there's no pawn, so the
      AngelScript update returns early and your typed values stick. Walk through this script:

| Set this | Expect this in the viewport |
|---|---|
| `bShouldMove` ✅, `StrafeSpeed` 623, `Direction` 0, `PlayRate` 1.0 | forward jog |
| `Direction` → 90 / 180 / -90 | right strafe / backpedal / left strafe, no pop crossing ±180 |
| `StrafeSpeed` → 225 | walk |
| `PlayRate` → 1.35 | same jog, faster playback (sprint compensation) |
| `bIsFalling` ✅, `VerticalVelocity` 600 | jump start, then fall loop |
| `VerticalVelocity` -800, then `bIsFalling` ☐ | fall loop → landing |
| `LandRecoveryAlpha` 1.0, then back to 0 | whole-body impact crouch that releases |
| `bIsSliding` ✅ (with `bIsCrouching` ✅) | slide pose — **NOT** the crouch walk |
| `bIsSliding` ☐, `bIsCrouching` ✅ | crouch fallback (walk-pose blend — the known compromise) |
| `AimPitch` -60 → +60 (with `AimOffsetAlpha` 1) | torso/rifle sweeps down → up |
| `AimOffsetAlpha` 0 | aiming stops, base pose |

- [ ] **Montage layering check** (optional but satisfying): with the jog running in preview,
      right-click `MTG_Rifle_Fire` in the Content Browser — if your build offers "Play in
      preview" via the montage editor, confirm arms fire while legs keep jogging. Otherwise this
      is verified in PIE during Task 8.
- [ ] Hand off: the session assigns `ABP_Hero` to `BP_Vanguard`/`BP_Bombardier` (anim class), then
      the 2-player PIE pass (plan Task 8 Step 4) checks: no T-pose, 8-way strafe with torso on
      crosshair, remote-proxy aim pitch, slide pose, jump→fall→land with recovery, no foot-slide.

### Common failure symptoms

| Symptom | Cause → fix |
|---|---|
| **T-pose** in PIE or preview | Reparent missed — Part 3: Class Settings → Parent Class must be `URogueHeroAnimInstance`; or the ABP isn't assigned as the mesh's Anim Class (Task 8 Step 3 does that). |
| **Legs slide/skate at sprint speed** | `Play Rate` pin not exposed or not wired to `PlayRate` (Part 4.3, Cycle state). Also recheck §1.4 — authored jog speed is 623 (displacement/length — §1.4). |
| **Torso doesn't aim** / aims wrong | Layered-blend Branch Filter bone name typo (`spine_01`, exact) — or AO_Rifle poses aren't Mesh Space additive (Part 6) — or `AimOffsetAlpha` pin unwired. |
| **Crouch pose flashes during slides** | T7/T8 missing the `(NOT bIsSliding)` term — see §4.6. |
| **Pop when strafing through ±180** | Wrap Input unchecked on the Direction axis, or the duplicate `Bwd` sample at -180 missing (§1.2/§1.3). |
| **Idle pops in while decelerating** | A transition tests `GroundSpeed` instead of `bShouldMove` — only `bShouldMove` gates Idle⇄Cycle. |
| **Fire/reload moves the whole body** | Montage slot isn't `DefaultGroup.UpperBody` (Part 2 last step), or Node 4's Slot Name still says DefaultSlot. |
| **Fire/reload does nothing visible** | `UpperBody` slot node missing from the chain, or slot wasn't saved onto the skeleton (Part 2 — save `SK_Mannequin`). |
| **Whole body crouches oddly after every step off a ledge** | `LandRecoveryAlpha` wired somewhere other than the Apply Additive Alpha, or the additive player's clip isn't `MM_Rifle_Jump_RecoveryAdditive`. |
| **Character stuck mid-air pose on the ground** | ToAirborne conduit rule missing (`bIsFalling` goes in the CONDUIT's graph, the five incoming transitions are literal-true) — §4.5. |

---

*Recipe deviations from research §C.3, all deliberate: Land uses the GASP `Land_Run_Light` bake
(plan Task 7) instead of `MM_Rifle_Jump_Fall_Land` (kept as fallback if the bake looks off);
the baked slide loop's real name is `Slide_Loop_FootOut` (the plan's bake list called it
`Slide_Loop`); Crouch transitions add `(NOT bIsSliding)` per the variable's actual semantics;
"Vel.Z" in the recipe is the variable `VerticalVelocity` here.*
