# Wire hero-select content:
# 1. BP_RaidGamemode.HeroPawnClasses = [BP_Vanguard_C, BP_Bombardier_C] (roster order!)
#    CDO edit MUST be followed by compile_blueprint + save (memory: mcp-bp-cdo-needs-compile).
# 2. Create L_Lobby: floor + light + PlayerStart, GameMode Override -> ALobbyGameMode (AS class).
import unreal

# --- 1. BP_RaidGamemode.HeroPawnClasses ---
BP = '/Game/Blueprints/BP_RaidGamemode'
bp = unreal.EditorAssetLibrary.load_asset(BP)
if bp is None:
    raise RuntimeError(f'missing {BP}')
van = unreal.EditorAssetLibrary.load_blueprint_class('/Game/Blueprints/BP_Vanguard')
bom = unreal.EditorAssetLibrary.load_blueprint_class('/Game/Blueprints/BP_Bombardier')
if van is None or bom is None:
    raise RuntimeError('missing hero BP classes')
cdo = unreal.get_default_object(bp.generated_class())
cdo.set_editor_property('HeroPawnClasses', [van, bom])
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
if not unreal.EditorAssetLibrary.save_asset(BP):
    raise RuntimeError('save BP_RaidGamemode failed')
check = unreal.get_default_object(bp.generated_class()).get_editor_property('HeroPawnClasses')
unreal.log(f'DONE-HEROCLASSES {[str(c.get_name()) for c in check]}')

# --- 2. L_Lobby level ---
LEVEL = '/Game/Levels/L_Lobby'
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
    unreal.log(f'EXISTS {LEVEL} — recreating')
    unreal.EditorAssetLibrary.delete_asset(LEVEL)
if not les.new_level(LEVEL):
    raise RuntimeError('new_level failed')

eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

floor = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
mesh = unreal.EditorAssetLibrary.load_asset('/Engine/BasicShapes/Plane')
floor.static_mesh_component.set_static_mesh(mesh)
floor.set_actor_scale3d(unreal.Vector(30, 30, 1))
floor.set_actor_label('LobbyFloor')

sun = eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500), unreal.Rotator(-50, 30, 0))
sun.set_actor_label('Sun')
sky = eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 500), unreal.Rotator(0, 0, 0))
sky.set_actor_label('Sky')

eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0, 0, 200), unreal.Rotator(0, 0, 0))

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
ws = unreal.GameplayStatics.get_actor_of_class(world, unreal.WorldSettings)
gm = unreal.load_class(None, '/Script/Angelscript.LobbyGameMode')
if gm is None:
    raise RuntimeError('LobbyGameMode class not found')
ws.set_editor_property('DefaultGameMode', gm)

if not les.save_current_level():
    raise RuntimeError('save_current_level failed')
unreal.log(f'DONE-LOBBY-LEVEL {LEVEL} gamemode={gm.get_name()}')
