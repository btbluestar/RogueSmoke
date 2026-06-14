# Design Threads

> Live design questions that are **not yet decided** — captured so they don't get lost between
> sessions. Each thread has a status, the current recommendation, and the reasoning. When a thread
> is settled it graduates to a numbered entry in `DECISIONS.md` and is marked **Resolved** here.
>
> This is the "thinking out loud" doc; `DECISIONS.md` is the "this is settled" doc.

---

## DT-1 — Stamina pip economy: what consumes a pip

**Status:** Open (model agreed, consumers not built yet)

**Context.** Pips (D-0023) were originally spent by slide and slide-hop. Play-feel testing showed
sliding shouldn't be gated, so the slide/slide-hop pip spends were removed — sliding, sprinting, and
jumping are now **free**. Nothing currently consumes a pip; the attribute/regen/HUD plumbing is left
in place (dormant) on `URogueMovementSet`.

**Model (from Deadlock).** Free movement vs. metered burst movement:

| Move | Pip cost |
|------|----------|
| Walk / sprint | Free |
| Slide / slide-hop | Free |
| Jump | Free |
| **Dash** (see DT-2) | **1 pip** |
| **Air-jump / double-jump** (see DT-4) | **1 pip** |

**Recommendation.** Keep the pip system in place and dormant. Wire it to **dash** and **air/double-jump**
when those are built. Do **not** rip it out.

**Open sub-questions:** MaxStamina (pip count), regen rate/delay once there are real consumers.

**See:** `DECISIONS.md` D-0023, GLOSSARY "stamina pip".

---

## DT-2 — Dash control binding

**Status:** Open (dash not built; this is the input question that blocks building it)

**Context.** Deadlock binds dash to Shift because sprint there is **automatic**. In RogueSmoke the
player controls sprint manually on **hold-Shift**, so Shift is taken — dash needs another input.
Dash is also **not** in the GDD MVP movement kit (D-0015 is sprint/crouch/slide/jump/double-jump),
so adding it is a deliberate extension.

**Options considered:**
- **Double-tap a movement direction** (A,A = dash left). *(Recommended.)* No new binding, reads
  intuitively, and it's the Titanfall/Apex tap-strafe lineage that fits the Apex/Deadlock feel.
- Discrete dash key (mouse side-button, or `Alt`). Simple but spends a binding and is less expressive.
- Make sprint a toggle and reuse Shift+direction for dash. Rejected — muddies the sprint input.

**Recommendation.** Double-tap-direction dash. Costs 1 pip (DT-1).

---

## DT-3 — Shoot while sprinting

**Status:** Open (not specified in the GDD)

**Context.** The GDD movement list (§9.1) doesn't say whether you can fire while sprinting. The
camera is **strafe/aim-locked** (D-0014): the character always faces the crosshair, even while
sprinting, so the gun naturally still points where you aim — firing while sprinting is mechanically
free, no animation conflict.

**Recommendation.** **Allow it, with a sprint accuracy penalty** (a spread multiplier applied while
sprinting). Makes it a risk/reward choice — mobility vs. precision — rather than a strict upgrade
over standing/focus fire. Small tunable on the weapon component (the spread system already exists),
not a new system.

**Open sub-question:** exact spread multiplier; whether focus (ADS) cancels sprint or just overrides
the penalty.

---

## DT-4 — Double-jump as meta-progression

**Status:** Open (parked — depends on meta-progression scope, GDD §6.3 / DECISIONS still-open)

**Context.** D-0015 lists double-jump in the MVP kit; the GDD also floats jump *count* as a
meta-progression upgrade ("unlock the double-jump," potentially 2–3 air jumps). Air-jumps are a
candidate pip consumer (DT-1).

**Recommendation.** Keep `MaxJumpCount` a tunable knob (already is). Defer the unlock/economy until
the meta-progression scope is decided and the core loop is proven (GDD §6.3). If air-jumps cost a
pip, that ties DT-1 and DT-2 into one shared "burst movement" budget.

---

## DT-5 — Slide steering as a meta-upgrade

**Status:** Implemented as a tunable; balance/meta-wiring open

**Context.** The slide is a momentum move: input thrust during the slide is clamped to
`SlideSteerAccel` (on `URogueLocomotionComponent`) so friction actually degrades it on flat ground.
`SlideSteerAccel = 0` is pure momentum (no steering); higher values let the player nudge the slide's
direction. The user wants "some steering, not much," and flagged it as a natural meta-progression
upgrade (more steering = a skill/control unlock).

**Tradeoff.** Steering and the flat-ground degrade pull against each other: a high `SlideSteerAccel`
lets a held move-key re-accelerate the slide toward the lifted crouch cap, which flattens the
friction decay the slide feel depends on. So keep the base value modest (currently 1000) and let
meta-upgrades grant *small* increments.

**Recommendation.** Ship a low base steer; expose `SlideSteerAccel` as a meta knob (catalogued in
[MetaUpgradeVariables.md](MetaUpgradeVariables.md)). Revisit the exact value after feel-testing.
