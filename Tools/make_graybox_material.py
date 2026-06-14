# Headless authoring for M_Graybox — the blockout/debug material the procgen stamper tints per
# element type. UNLIT + an emissive "Tint" VectorParameter, so each stamped box reads its function
# color regardless of scene lighting (graybox convention: flat unlit color, no art, no bake).
# Run headless:  UnrealEditor-Cmd <uproject> -run=pythonscript -script=Tools/make_graybox_material.py
import unreal, traceback

RESULT = r"C:\Users\btblu\AppData\Local\Temp\rs_smoke\graybox_mat.txt"
LOG = []
def log(m): LOG.append(str(m))

try:
    pkg = "/Game/Materials/M_Graybox"
    if unreal.EditorAssetLibrary.does_asset_exist(pkg):
        unreal.EditorAssetLibrary.delete_asset(pkg)
        log("deleted existing")

    at = unreal.AssetToolsHelpers.get_asset_tools()
    mat = at.create_asset("M_Graybox", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew())
    log("material created: %s" % mat)

    # Unlit so the color shows with zero lights (debug overlay reads identically in any scene).
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    mel = unreal.MaterialEditingLibrary
    tint = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -350, 0)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.6, 0.63, 0.65, 1.0))
    # VectorParameter's default RGB output -> Emissive Color.
    mel.connect_material_property(tint, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    log("Tint -> Emissive wired")

    mel.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(pkg)
    log("saved %s" % pkg)
    log("DONE OK")
except Exception as e:
    log("FATAL: %s\n%s" % (e, traceback.format_exc()))

try:
    with open(RESULT, "w") as f:
        f.write("\n".join(LOG))
except Exception:
    pass
