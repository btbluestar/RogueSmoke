# Create the front-end L_MainMenu level: empty map + PlayerStart, GameMode Override ->
# AMenuGameMode (AngelScript class; AS classes live at /Script/Angelscript.<Name>).
import unreal

LEVEL = '/Game/Levels/L_MainMenu'

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
    unreal.log(f'EXISTS {LEVEL} — recreating')
    unreal.EditorAssetLibrary.delete_asset(LEVEL)

if not les.new_level(LEVEL):
    raise RuntimeError('new_level failed')

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

# PlayerStart so the spectator shell spawns deterministically.
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0, 0, 200), unreal.Rotator(0, 0, 0))

ws = unreal.GameplayStatics.get_actor_of_class(world, unreal.WorldSettings)
if ws is None:
    raise RuntimeError('no WorldSettings actor found')
gm = unreal.load_class(None, '/Script/Angelscript.MenuGameMode')
if gm is None:
    raise RuntimeError('MenuGameMode class not found — script not compiled in?')
ws.set_editor_property('DefaultGameMode', gm)

if not les.save_current_level():
    raise RuntimeError('save_current_level failed')
unreal.log(f'DONE-MENU-LEVEL {LEVEL} gamemode={gm.get_name()}')
