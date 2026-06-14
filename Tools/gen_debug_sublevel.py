# Headless authoring for a reusable DEBUG SUBLEVEL (procgen, Plan 3 verification).
# L_GenDebug holds lighting (so the unlit generated greybox renders) + tagged debug cameras, and is
# streamed (always-loaded) into L_GenRaid. Any generated level can add this sublevel for debugging.
import unreal, traceback

RESULT = r"C:\Users\btblu\AppData\Local\Temp\rs_smoke\gendbg_result.txt"
LOG = []
def log(m): LOG.append(str(m))

def set_movable(actor):
    try:
        actor.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    except Exception as e:
        log("  mobility: %s" % e)

try:
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    les.new_level("/Game/Levels/L_GenDebug")
    log("L_GenDebug created")

    # Lighting so the generated greybox renders lit (movable -> no bake needed for runtime ISM).
    dl = eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 3000), unreal.Rotator(-52, -40, 0))
    set_movable(dl)
    try: dl.light_component.set_intensity(6.0)
    except Exception as e: log("  dl intensity: %s" % e)
    sl = eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 3000))
    set_movable(sl)
    log("lights spawned")

    # Debug cameras (tagged) over the arena: top-down + two angles.
    tgt = unreal.Vector(0, 0, 200)
    cams = [unreal.Vector(0, 0, 5500), unreal.Vector(-4000, -4000, 2800), unreal.Vector(4200, -1200, 1500)]
    for i, c in enumerate(cams):
        rot = unreal.MathLibrary.find_look_at_rotation(c, tgt)
        cam = eas.spawn_actor_from_class(unreal.CameraActor, c, rot)
        try: cam.set_editor_property("tags", [unreal.Name("GenDebugCam")])
        except Exception as e: log("  cam tag %d: %s" % (i, e))
        log("cam %d placed" % i)

    les.save_current_level()
    log("L_GenDebug saved")

    # Stream L_GenDebug into L_GenRaid (always loaded) so lights + cameras come with it.
    les.load_level("/Game/Levels/L_GenRaid")
    world = unreal.EditorLevelLibrary.get_editor_world()
    streaming = unreal.EditorLevelUtils.add_level_to_world(world, "/Game/Levels/L_GenDebug", unreal.LevelStreamingAlwaysLoaded)
    log("sublevel added: %s" % streaming)
    les.save_current_level()
    log("L_GenRaid saved with sublevel")
    log("DONE OK")
except Exception as e:
    log("FATAL: %s\n%s" % (e, traceback.format_exc()))

try:
    with open(RESULT, "w") as f:
        f.write("\n".join(LOG))
except Exception:
    pass
