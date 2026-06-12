# Lyra Migration — User Checkpoint B (final feel pass, after Phases 3–6)

Everything from **checkpoint A** (anim stack + gun) plus the rest of the migration.
All phases are machine-verified and committed on `lyra-anim-migration`; this list is the
human judgment pass. v1 retirement and the branch finish wait for your sign-off.

## New since checkpoint A

1. **Weapon audio/VFX (Phase 3)** — fire a burst: layered Lyra rifle MetaSound per shot,
   muzzle flash on the barrel, tracers, concrete impact puffs at hit points, and the
   AutoRifle tail ring-out when you release the trigger. (`ImpactEnemyFX` is intentionally
   empty — damage numbers + hitmarker carry enemy feedback. Tracer endpoint param isn't wired;
   tracers aim by spawn rotation — flag if they look off at close range.)
2. **Footsteps (Phase 4)** — run around: surface-aware footsteps (concrete on the debug
   floors) from Lyra's authored notifies; jumps/lands have their own land sounds. Volume/mix
   is unmixed first-pass.
3. **Heat-curve spread (Phase 5)** — hold full-auto and watch the crosshair: opening shots
   stay tight (flat early curve), bloom ramps hard in sustained fire, recovers after release.
   `MoveTune heatpershot/heatcooldown/spreadrecoverydelay` tune it live. Your recoil numbers
   were NOT touched.
4. **Stamina pips (Phase 6, D-0023)** — pip row under the health bar: slide costs 1,
   slide-hop's jump costs 1, sprint free; out of pips the crouch press gives a plain crouch
   (no slide). Regen one pip at a time (2.5 s, 1 s pause after a spend) —
   `MoveTune staminaregenseconds/staminaregendelay`. Regen *pacing* deserves your judgment;
   probes confirmed spend/regen logic but wall-clock pacing was noisy in a throttled editor.
5. **Slide anim handover** — long slides should hand from slide-in to the loop; short slides
   live entirely in the In clip. Slide-in is left-foot-only for now (foot-phase pick is a
   polish item).

## 2-player listen-server session (needs you — MCP drives only 1 player)

- Remote player's locomotion, turn-in-place, aim tracking (all replicated-data-driven; should
  just work).
- KNOWN GAPS (queued for the replication pass — confirm they're the only ones):
  - Remote player's **slide montage** won't play on your client (T-pose-less — they'll just
    run crouched during their slide).
  - Remote **fire stance** (IsFiring additive) doesn't reach proxies; fire montages DO
    multicast, so shots still animate.
  - Stamina pips render only for your own hero (by design).

## After your sign-off

Say the word and I will: retire the v1 anim stack (unhook + delete `ABP_Hero`,
`BS_Rifle_Strafe`, the AO_* yaw poses, the v1 GASP retargets; guide already carries the
retirement banner), run the final battery, and finish the branch (merge options per
superpowers:finishing-a-development-branch).
