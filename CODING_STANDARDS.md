# Coding Standards

> AngelScript conventions for **RogueSmoke** (UE5 + UnrealEngine-Angelscript).
> This is the *how*. For *what/why*, see `ARCHITECTURE.md`.
> Source of truth for the language: https://angelscript.hazelight.se/

## 1. Files & folders

- Scripts are `.as` files under `Project/Script/`. The plugin auto-loads everything there.
- One primary class per file; file name matches the class without the Unreal prefix
  (`HeroCharacter.as` contains `class AHeroCharacter`).
- Mirror the folder map in `ARCHITECTURE.md` (`Core/`, `Player/`, `Items/`, ...).
- Use explicit `import` statements; prefer the editor's **Add Import To** (Shift+Alt+I) over
  hand-typing import paths.

## 2. Naming

Mirror Unreal C++ conventions so script and engine code read the same:

- Actors: `A` prefix (`AHeroCharacter`, `AEnemyDrone`).
- Components: `U` prefix (`UInventoryComponent`).
- Structs: `F` prefix (`FItemRoll`). Enums: `E` prefix (`EFloorType`).
- Booleans: `b` prefix (`bIsDead`, `bReplicates`).
- Functions & properties: `PascalCase`. Locals & parameters: `camelCase`.
- Blueprint subclasses of script classes: `BP_` prefix (`BP_HeroCharacter`).
- Data assets: `DA_`; data tables: `DT_`.
- Avoid abbreviations except the established Unreal ones (`Num`, `Idx`, `Comp`).

## 3. AngelScript idioms

- Enable replication and other class defaults with `default` statements in the class body:
  ```angelscript
  class AReplicatedActor : AActor
  {
      default bReplicates = true;
  }
  ```
- Expose to the editor/Blueprint with `UPROPERTY()` / `UFUNCTION()` specifiers, same as C++.
- Prefer composition: put reusable behavior in `UActorComponent` subclasses.
- Use `FName` literals (`n"MyName"`) and formatted strings (`f"value = {x}"`) where supported.
- Keep functions small; this language hot-reloads on save, so iterate in tiny steps.

## 4. Networking conventions (read this twice)

The genre lives or dies on correct replication. Defaults here differ from C++.

### 4.1 The reliability gotcha
**AngelScript RPCs default to RELIABLE** — the opposite of C++. Mark transient/cosmetic
RPCs `Unreliable` explicitly, or you will flood the reliable channel.

```angelscript
// Cosmetic, fire-and-forget -> must opt into Unreliable
UFUNCTION(NetMulticast, Unreliable)
void Multicast_PlayHitFlash() { ... }
```

### 4.2 RPC direction & naming
- Specifiers: `Server`, `Client`, `NetMulticast` (optionally `BlueprintAuthorityOnly`).
- Prefix the function with its direction so call sites are unambiguous:
  - `Server_RequestUseAbility(...)` — client → server (validate before applying).
  - `Client_ShowReward(...)` — server → one owning client.
  - `Multicast_SpawnVFX(...)` — server → all clients (cosmetic).
- Client intent never trusts the client: validate inputs server-side inside the `Server_` RPC.

### 4.3 Replicated properties
```angelscript
class AHeroCharacter : ACharacter
{
    default bReplicates = true;

    UPROPERTY(Replicated)
    float Health = 100.0;

    // Only the owning client needs to see private build details
    UPROPERTY(Replicated, ReplicationCondition = OwnerOnly)
    int Currency = 0;

    // React to a replicated change on clients
    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Health)
    float MaxHealth = 100.0;

    UFUNCTION()  // ReplicatedUsing target MUST be a UFUNCTION
    void OnRep_Health() { UpdateHealthBar(); }
}
```
- Use `ReplicationCondition` (`OwnerOnly`, `SkipOwner`, `SimulatedOnly`, ...) to cut bandwidth.
- `OnRep_` functions must be `UFUNCTION()` or Unreal can't see them.

### 4.4 Authority discipline
- Gate authoritative logic with an authority check (`HasAuthority()` / role checks) before
  mutating game state. Never apply damage, grant loot, or spawn replicated actors on a client.
- Spawn replicated actors **on the server only**.

## 5. Procedural generation = determinism

- All gameplay randomness goes through a **seeded RNG stream** owned by the RunManager.
  Never call unseeded global random for anything that affects the world.
- Generation must not depend on: actor iteration order, hash ordering, wall-clock time,
  or per-machine float nondeterminism in branching logic.
- The same seed must produce the same floor/loot on host and clients. Add a debug command to
  print/replay the seed; it will save hours.

## 6. Blueprint vs script boundary

- **Logic in script. Content in Blueprints/DataAssets.**
- Blueprints exist to assign assets (meshes, materials, sounds) to script classes and to wire
  designer-tweakable defaults — not to hold gameplay logic.
- UMG widgets stay as Blueprint assets; drive them from script controllers.

## 7. Known plugin limitations (avoid these)

From the official development-status page:

- `Super::Function()` only calls a script parent. You **cannot** call the C++ super of a
  `BlueprintNativeEvent` from script — design around it.
- **Unreal Interfaces (UInterface/IInterface) are not supported in AngelScript.** Use base
  classes, components, or gameplay tags instead of interfaces.
- A commercial project should keep an engine programmer available; this is a source-built
  custom engine, and platform/engine issues are yours to debug.

## 8. Performance

- Script is faster than Blueprint and approaches C++ when transpiled in shipping builds.
- Avoid per-tick work where an event/timer suffices; roguelikes spawn a lot of actors.
- Pool frequently spawned actors (projectiles, pickups, common enemies).
- Replicate the minimum: prefer events over high-frequency property replication.

## 9. Tests

- Use the plugin's script tests for pure logic (generation determinism, loot rolls, math).
  See https://angelscript.hazelight.se/scripting/script-tests/.
- Determinism tests are the highest-value tests in this project: assert that a fixed seed
  yields a fixed layout/loot result.

## 10. Comments & commits

- Comment *why*, not *what*. Document any non-obvious networking authority decision inline.
- Keep commits small and buildable; never commit with broken script compilation.
- Don't commit secrets, large binaries outside LFS, or local-only config.
