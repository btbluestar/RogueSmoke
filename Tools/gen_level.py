# Headless authoring for the procgen Phase-1 loop (Plan 3, Part D).
# Creates BP_GenRaidGamemode (child of BP_RaidGamemode, bGeneratedArena=true) and the L_GenRaid level
# (one RaidObjective in HoldAndChannel/generated mode + a PlayerStart + the GameMode override).
# Logs every step to a result file (commandlet print is unreliable).
import unreal
import traceback

RESULT = r"C:\Users\btblu\AppData\Local\Temp\rs_smoke\gend_result.txt"
LOG = []
def log(m): LOG.append(str(m))

def try_set(obj, names, val):
    """Try several property-name spellings; return the one that stuck (or None)."""
    for n in names:
        try:
            obj.set_editor_property(n, val)
            return n
        except Exception as e:
            log("  set %r failed: %s" % (n, e))
    return None

try:
    # 1. BP_GenRaidGamemode as a child of BP_RaidGamemode (inherits hero/PC/pawn/gamestate classes).
    parent_bp = unreal.load_asset("/Game/Blueprints/BP_RaidGamemode")
    parent_class = parent_bp.generated_class()
    log("parent_class=%s" % parent_class)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    bp_pkg = "/Game/Blueprints/BP_GenRaidGamemode"
    if unreal.EditorAssetLibrary.does_asset_exist(bp_pkg):
        unreal.EditorAssetLibrary.delete_asset(bp_pkg)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    new_bp = asset_tools.create_asset("BP_GenRaidGamemode", "/Game/Blueprints", unreal.Blueprint, factory)
    unreal.BlueprintEditorLibrary.compile_blueprint(new_bp)
    gen = new_bp.generated_class()
    cdo = unreal.get_default_object(gen)
    used = try_set(cdo, ["bGeneratedArena", "b_generated_arena", "generated_arena"], True)
    log("bGeneratedArena set via %s" % used)
    if used:
        log("  readback=%s" % cdo.get_editor_property(used))
    unreal.BlueprintEditorLibrary.compile_blueprint(new_bp)
    unreal.EditorAssetLibrary.save_asset(bp_pkg)
    log("BP saved: %s" % bp_pkg)

    # 2. New level.
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    les.new_level("/Game/Levels/L_GenRaid")
    log("level created")

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # 3. Objective (HoldAndChannel + generated placement).
    obj_class = unreal.load_object(None, "/Script/Angelscript.RaidObjective")
    log("obj_class=%s" % obj_class)
    obj = eas.spawn_actor_from_class(obj_class, unreal.Vector(0, 0, 0))
    # Mode is an enum (EObjectiveMode.HoldAndChannel == 1). Try enum then int.
    mode_used = None
    try:
        obj.set_editor_property("Mode", unreal.EObjectiveMode.HOLD_AND_CHANNEL); mode_used = "enum"
    except Exception as e:
        log("  Mode enum failed: %s" % e)
        mode_used = try_set(obj, ["Mode", "mode"], 1)
    log("Mode set via %s" % mode_used)
    used2 = try_set(obj, ["bUseGeneratedLayout", "b_use_generated_layout"], True)
    log("bUseGeneratedLayout set via %s" % used2)

    # 4. PlayerStart (fallback; heroes spawn at the Drop node).
    eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0, 0, 200))
    log("playerstart spawned")

    # 5. GameMode override on World Settings.
    world = unreal.EditorLevelLibrary.get_editor_world()
    ws = world.get_world_settings()
    gm_used = try_set(ws, ["default_game_mode", "DefaultGameMode"], gen)
    log("gamemode override set via %s -> %s" % (gm_used, ws.get_editor_property(gm_used) if gm_used else "FAIL"))

    # 6. Save.
    les.save_current_level()
    log("level saved")
    log("DONE OK")
except Exception as e:
    log("FATAL: %s\n%s" % (e, traceback.format_exc()))

try:
    with open(RESULT, "w") as f:
        f.write("\n".join(LOG))
except Exception:
    pass
