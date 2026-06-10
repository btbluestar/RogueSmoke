# Create /Game/VFX/M_TelegraphRing — the one material behind all telegraph/death-burst discs
# (TelegraphZoneFX + the AttackingElite ground rings). Unlit translucent two-sided, with
# "Color" (vector -> emissive) and "Opacity" (scalar -> opacity) parameters driven per-MID.
# Headless commandlet; idempotent (re-runs rebuild the expression graph).
import unreal

DIR = '/Game/VFX'
PATH = f'{DIR}/M_TelegraphRing'

mel = unreal.MaterialEditingLibrary

if unreal.EditorAssetLibrary.does_asset_exist(PATH):
    mat = unreal.EditorAssetLibrary.load_asset(PATH)
    unreal.log(f'TGM: exists, rebuilding {PATH}')
else:
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat = tools.create_asset('M_TelegraphRing', DIR, unreal.Material, unreal.MaterialFactoryNew())
if mat is None:
    raise RuntimeError('TGM-FATAL: could not create/load material')

mel.delete_all_material_expressions(mat)

color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, -100)
color.set_editor_property('parameter_name', 'Color')
color.set_editor_property('default_value', unreal.LinearColor(1.0, 0.3, 0.05, 1.0))

opacity = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -400, 150)
opacity.set_editor_property('parameter_name', 'Opacity')
opacity.set_editor_property('default_value', 0.3)

ok1 = mel.connect_material_property(color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
ok2 = mel.connect_material_property(opacity, '', unreal.MaterialProperty.MP_OPACITY)
if not (ok1 and ok2):
    raise RuntimeError('TGM-FATAL: failed to connect material expressions')

mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property('two_sided', True)

mel.recompile_material(mat)
if not unreal.EditorAssetLibrary.save_asset(PATH, only_if_is_dirty=False):
    raise RuntimeError('TGM-FATAL: save failed')
unreal.log(f'TGM-DONE {PATH}')
