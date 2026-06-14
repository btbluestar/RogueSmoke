# Build /Game/Levels/DebuggingLevels/DL_Upgrades — the upgrade-testing firing range.
# Duplicates DL_Enemy_Carapace (so GameMode override / floor / lighting / PlayerStart carry over),
# strips the enemy test stand, and places an AUpgradeTestRange at the PlayerStart so the LINE
# formation recedes straight downrange from where the player spawns. Idempotent: re-runs clear any
# previously placed stands/ranges and re-place. Headless commandlet; run with -run=pythonscript.
# (Class paths: AS classes live at /Script/Angelscript.<Name> — memory as-uclass-paths-in-python.)
import unreal

SRC_LEVEL = '/Game/Levels/DebuggingLevels/DL_Enemy_Carapace'
DST_LEVEL = '/Game/Levels/DebuggingLevels/DL_Upgrades'
RANGE_CLASS_PATH = '/Script/Angelscript.UpgradeTestRange'
STAND_CLASS_PATH = '/Script/Angelscript.EnemyTestStand'

if not unreal.EditorAssetLibrary.does_asset_exist(DST_LEVEL):
    if not unreal.EditorAssetLibrary.duplicate_asset(SRC_LEVEL, DST_LEVEL):
        raise RuntimeError('DLU-FATAL: duplicate_asset failed')
    unreal.log(f'DLU: duplicated {SRC_LEVEL} -> {DST_LEVEL}')
else:
    unreal.log(f'DLU: {DST_LEVEL} exists, reconciling')

if not unreal.EditorLevelLibrary.load_level(DST_LEVEL):
    raise RuntimeError('DLU-FATAL: load_level failed')

range_class = unreal.load_object(None, RANGE_CLASS_PATH)
stand_class = unreal.load_object(None, STAND_CLASS_PATH)
if range_class is None:
    raise RuntimeError('DLU-FATAL: UpgradeTestRange class not found (script compiled?)')

# Strip the inherited enemy stand + any range from a previous run.
removed = 0
player_start = None
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    if actor is None:
        continue
    cls = actor.get_class()
    if (stand_class is not None and cls == stand_class) or cls == range_class:
        unreal.EditorLevelLibrary.destroy_actor(actor)
        removed += 1
    elif isinstance(actor, unreal.PlayerStart):
        player_start = actor
unreal.log(f'DLU: removed {removed} old stand/range actors')

loc = unreal.Vector(0.0, 0.0, 100.0)
rot = unreal.Rotator(0.0, 0.0, 0.0)
if player_start is not None:
    loc = player_start.get_actor_location()
    rot = player_start.get_actor_rotation()
    unreal.log(f'DLU: anchoring range at PlayerStart {loc}')
else:
    unreal.log_warning('DLU: no PlayerStart found, placing range at origin')

placed = unreal.EditorLevelLibrary.spawn_actor_from_class(range_class, loc, rot)
if placed is None:
    raise RuntimeError('DLU-FATAL: failed to place UpgradeTestRange')

if not unreal.EditorLevelLibrary.save_current_level():
    raise RuntimeError('DLU-FATAL: save_current_level failed')

unreal.log(f'DLU-DONE level={DST_LEVEL} removed={removed} range_at={loc}')
