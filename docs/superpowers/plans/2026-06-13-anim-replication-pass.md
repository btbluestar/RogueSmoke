# Anim Replication Pass — Sim-Proxy Stance & Slide

> **For agentic workers:** ready-to-execute. All edits are AngelScript-only; verify with
> `as-helper run_code_test` (compile) + `SmokeTest.ps1` 9/9 (interactive editor CLOSED).
> Final confirmation is a **2-player listen-server** session — only the user can run it.

**Goal:** Close the two documented checkpoint-A item-7 proxy gaps so a *remote* player
animates correctly on your machine: (A) the firing upper-body stance, and (B) the slide montage.

**Status of inputs (verified 2026-06-13 against the committed `6e8585a` tree):**
- `bFocusing` (drives `GameplayTag_IsADS`) is **already** `UPROPERTY(Replicated)`
  (`HeroCharacter.as:96`) — reaches proxies, no change needed.
- Fire/reload **montages** already multicast (`Multicast_FireFX`/`Multicast_ReloadFX`) — shots
  already animate on proxies. The *stance bool* is the only fire gap.

---

## Why these are broken on proxies (root cause)

The anim instance (`HeroAnimInstance.as`) ticks on **every** machine the mesh is relevant on,
including simulated proxies (its landing detection comment at line 149 confirms this is by
design). But two pieces of state it reads from the Hero are **not proxy-valid**:

1. **`bFireHeldForFacing`** (`HeroCharacter.as:159`) is a `private bool`, not replicated. It is
   set on the owner (`RaidPlayerController.as:279/290`) and on the server
   (`Server_SetWantsToFire` → `SetFireHeldForFacing`, `HeroCharacter.as:800`). On a sim proxy it
   stays its default `false`, so `GameplayTag_IsFiring = Hero.IsFireHeldForFacing()`
   (`HeroAnimInstance.as:144`) is **always false** → remote players never show the fire stance.

2. **Slide state.** `TickSlideAnim()` is gated to `IsLocallyControlled() || HasAuthority()`
   (`HeroCharacter.as:596`) — never called on a sim proxy. *And* even if it were, it reads
   `Locomotion.IsSliding()` → `LocomotionComponent.bSliding` (`LocomotionComponent.as:110`,
   private, not replicated), which only updates where `TickLocomotion` runs (same owner+server
   gate, `HeroCharacter.as:601`). So a proxy has no valid slide state to drive the montage.
   (The comment at `HeroCharacter.as:706-707` *claims* "all machines … proxies mirror" — that is
   currently aspirational/wrong and must be corrected.)

---

## Task 1: Replicate the firing stance to sim proxies

**Files:** Modify `RogueSmoke/Script/Player/HeroCharacter.as`

- [ ] **Step 1 — make `bFireHeldForFacing` replicated, owner-skipped.**
  The owner already mirrors it locally for prediction, so we only need it on *non-owners*.
  Replace the declaration at `HeroCharacter.as:157-159`:

```angelscript
    // Owner+server mirror of held-fire intent: bWantsToFire is server-only, but facing must
    // agree on the predicting owner too or the body snaps on correction. Replicated to sim
    // proxies (SkipOwner — the owner sets it from input) so the remote fire stance animates.
    UPROPERTY(Replicated, ReplicationCondition = SkipOwner)
    bool bFireHeldForFacing = false;
```

- [ ] **Step 2 — verify compile.** `as-helper run_code_test` → no errors.
      (A replicated property in AngelScript needs no manual `GetLifetimeReplicatedProps`; the
      plugin generates it from the specifier. Confirm there is no other `UPROPERTY`-less
      assumption — `bFireHeldForFacing` is read via `IsFireHeldForFacing()` accessor only.)

- [ ] **Step 3 — sanity:** `GameplayTag_IsFiring` and `GameplayTag_IsADS` now both derive from
      replicated state on proxies. No anim-instance change required.

## Task 2: Replicate slide-active state and drive the proxy montage

**Files:** Modify `RogueSmoke/Script/Player/HeroCharacter.as`

Approach: a server-authoritative replicated bool with an `OnRep` is more robust than a
multicast for a *looping* cosmetic (relevancy / late-join get correct current state; a missed
multicast edge would otherwise leave a stuck or absent loop). The owner keeps predicting locally
(unchanged); only proxies consume the replicated bool.

- [ ] **Step 1 — add the replicated mirror.** Near the other slide fields
      (`HeroCharacter.as:142`, by `bSlideAnimActive`):

```angelscript
    // Server-authoritative slide-active mirror for sim proxies (owner+server drive the montage
    // off the live locomotion state; proxies have no ticking locomotion, so they mirror this).
    UPROPERTY(Replicated, ReplicationCondition = SkipOwner)
    bool bReplicatedSliding = false;
```

- [ ] **Step 2 — server publishes it each authority tick.** In `Tick`, inside the
      `if (HasAuthority())` block (`HeroCharacter.as:609`), add:

```angelscript
            // Mirror slide state to sim proxies (their locomotion doesn't tick).
            bReplicatedSliding = (Locomotion != nullptr && Locomotion.IsSliding());
```

- [ ] **Step 3 — proxies drive the montage from the replicated bool.** Change the Tick gate so
      proxies also edge-detect slide, and make `TickSlideAnim` read the right source per role.
      Replace the gate block (`HeroCharacter.as:595-603`) so `TickSlideAnim()` is called on
      proxies too, but locomotion/facing stay owner+server:

```angelscript
        // Anim edge-detect runs everywhere; the physics tick stays owner+server.
        TickSlideAnim();
        if (IsLocallyControlled() || HasAuthority())
        {
            Locomotion.TickLocomotion(DeltaSeconds);
            TickFacing(DeltaSeconds);
        }
```

      Then in `TickSlideAnim` (`HeroCharacter.as:708-710`), source the slide state by role:

```angelscript
    private void TickSlideAnim()
    {
        // Owner predicts + server confirms off live locomotion; sim proxies mirror the
        // replicated bool (their locomotion component doesn't tick).
        bool bSlidingNow = (IsLocallyControlled() || HasAuthority())
            ? (Locomotion != nullptr && Locomotion.IsSliding())
            : bReplicatedSliding;
```

      ⚠️ `TickSlideAnim` runs BEFORE `Locomotion.TickLocomotion` for the owner (the edge-order
      comment at line 598). That ordering still holds — keep `TickSlideAnim()` first.

- [ ] **Step 4 — fix the now-accurate comment** at `HeroCharacter.as:706-707` to state proxies
      mirror via `bReplicatedSliding` (not "derives from replicated movement").

- [ ] **Step 5 — verify compile.** `as-helper run_code_test` → no errors.

## Task 3: Machine verification

- [ ] **Step 1 — SmokeTest, editor CLOSED.** `Tools/SmokeTest.ps1` → expect 9/9 PASS.
      (Slide path exercised by movement levels; firing by RaidArena/Enemy levels. A regression in
      the owner path — the common case the smoke covers — would surface here.)
- [ ] **Step 2 — single-player PIE regression (owner unaffected):** `MoveSmoke` 4/4, and a quick
      visual: owner still slides + shows fire stance exactly as in `6e8585a` (these paths use the
      `IsLocallyControlled()` branch, untouched in behavior).

## Task 4: User confirmation (2-player listen server — cannot be machine-verified)

- [ ] Host + 1 client. On the **host's** screen, the joined client should now: (a) show the
      slide montage when they slide, and (b) hold the firing upper-body stance while shooting
      (not just the muzzle/montage burst). Update `2026-06-12-lyra-checkpoint-A.md` item 7
      "KNOWN PROXY GAPS" once confirmed.

---

## Commit

One commit after Tasks 1–3 are green (do **not** wait on the 2P confirmation — the code is
correct and smoke-verified; the 2P pass only *observes* it):

```
fix(anim-net): replicate fire stance + slide state to sim proxies (checkpoint-A item 7)

Remote players showed neither the firing upper-body stance nor the slide montage on
other machines: bFireHeldForFacing and the locomotion slide state were owner+server only.
Both now replicate (SkipOwner; owner still predicts locally) and the slide anim driver runs
on proxies off the replicated mirror. Owner path unchanged. SmokeTest 9/9; 2P observe-only.
```

**Respect standing exclusions** — never stage `GE_Upgrade_ChainDetonation.uasset`,
`ABP_Hero.uasset`, `Test_1.umap`, `SUPERPOWERS_HANDOFF.md`. This pass touches only
`HeroCharacter.as` (and, if anything, a doc).
