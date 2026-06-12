# Lyra Migration — User Checkpoint A (after Phases 1–2)

> **2026-06-13 UPDATE — your results are in and the three failures are fixed:**
> - **#3 slide**: montages played into a slot Lyra's graph doesn't have (`DefaultSlot`) —
>   they "played" invisibly. Now on `FullBodySlot`; slide pose verified on screen.
> - **#1 torso + #5 gun-90°-right**: one bug. The graph's aim writers were accidentally
>   deleted during the re-parent; `AimYaw` (= −RootYawOffset, the upper-body counter-rotation)
>   was stuck at 0, freezing the torso and leaving the gun pointing with the offset root.
>   Pristine ABP restored; aim pitch/yaw verified live, gun tracks the crosshair.
> Please re-test 1/3/5 (and 7 when you can). Details: plan doc §"Checkpoint-A bug-fix log".

Machine verification is green; these are the things only a human feel-pass can judge.
Nothing blocks on this — Phases 3–6 continue; v1 retirement waits for your sign-off.

## How to test
Open the editor (current branch `lyra-anim-migration`), PIE in
`/Game/Levels/DebuggingLevels/DL_Upgrades` (no enemies shooting back) and then a real raid level.
`MoveDebug` overlay is still available; `MoveTune dump` still works.

## Feel checklist
1. **Idle turn-in-place** — orbit the camera at idle: feet stay planted, torso tracks,
   authored turn steps fire past ~95°. (The v1 actor-level free-look is OFF; flip
   `bActorLevelFreeLook` on a hero BP only if you want to A/B it.)
2. **Starts / stops / pivots** — tap-move in different directions; distance matching should
   give you planted, weighty starts and stops with no foot-slide. Sprint (960) included.
3. **Slide** — sprint+crouch: GASP slide-in → loop → exit anim picked by how you leave
   (keep crouch / stand / keep running). Known feel-pass items: slide-in is left-foot only
   (no foot-phase pick yet), In→Loop handover untested on long slides.
4. **Jump/land** — distance-matched landing recovery.
5. **THE GUN** — you now actually have one (v1's mesh slot was empty!): Lyra SK_Rifle in the
   `weapon_r` grip socket, animated bolt/magazine via its own ABP, character + weapon fire
   and reload montages.
6. **FULL-AUTO** — hold LMB: continuous fire with auto-reload mid-hold. Two root causes
   fixed: the hero BPs' template `Event Tick` stub had been swallowing the entire AS Tick
   (refire logic never ran in v1), AND `DA_Weapon_AssaultRifle.bFullAuto` was False.
7. **2-player listen server** (host + 1 client — MCP can't drive two players):
   - Remote player's locomotion/turn-in-place/aim (all derived from replicated data).
   - KNOWN PROXY GAPS (will look wrong on the OTHER machine, fix queued in the replication
     pass): a remote player's slide montage won't play on your client (driver currently
     gated to owner+server), and the `GameplayTag_IsFiring` ABP stance bool doesn't reach
     simulated proxies (fire montages DO multicast, so shots still animate).

## What changed under you (summary)
- Heroes run Lyra's `ABP_Mannequin_Base` (re-parented onto our AS anim instance via
  CoreRedirects) + linked `ABP_RifleAnimLayers`; mesh is Lyra's full-detail SKM_Manny.
- Hero BP EventGraphs purged (were swallowing AS Tick + double-binding move input).
- Plugins enabled: AnimationLocomotionLibrary, AnimationWarping, Metasound.
- v1 ABP/anims untouched on disk for rollback.
