# py_retarget_gasp.py
# Bake the staged GASP (UEFN-skeleton) clips onto OUR Manny via IKRetargetBatchOperation,
# then move+rename the outputs to /Game/Characters/Mannequins/Anims/GASP/<NewName>.
# Run headlessly:
#   UnrealEditor-Cmd.exe <proj> -run=pythonscript -script="...\py_retarget_gasp.py" -abslog=...
# (commandlet python 'print' is lost — use unreal.log; see memory headless-verification-limits)
import unreal

OUT_DIR = "/Game/Characters/Mannequins/Anims/GASP"
SRC_MESH = "/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin"
# Target = OUR mesh (the project's own Manny, NOT the staged GASP copy) — retarget-bake-via-python memory.
TGT_MESH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"
RTG = "/Game/Characters/UE5_Mannequins/Rigs/RTG_UEFN_to_UE5_Mannequin"

ANIMS = {
    "/Game/Characters/UEFN_Mannequin/Animations/Slide/M_Neutral_Slide_FootOut_Into_Lfoot": "Slide_Enter",
    "/Game/Characters/UEFN_Mannequin/Animations/Slide/M_Neutral_Slide_FootOut_Loop": "Slide_Loop_FootOut",
    "/Game/Characters/UEFN_Mannequin/Animations/Slide/M_Neutral_Slide_FootOut_Out_Moving_Run": "Slide_Exit_Run",
    "/Game/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_F_Land_Run_Light_Lfoot": "Land_Run_Light",
    "/Game/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_F_Land_Run_Heavy_Lfoot": "Land_Run_Heavy",
    "/Game/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_F_Land_Stand_Light_Lfoot": "Land_Stand_Light",
    "/Game/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_Loop_Fall": "Fall_Loop",
    "/Game/Characters/UEFN_Mannequin/Animations/Sprint/M_Neutral_Sprint_Loop_F": "Sprint_Loop",
    "/Game/Characters/UEFN_Mannequin/Animations/Sprint/M_Neutral_Sprint_Stop_F_Lfoot": "Sprint_Stop",
}

def main():
    src_mesh = unreal.load_asset(SRC_MESH)
    tgt_mesh = unreal.load_asset(TGT_MESH)
    rtg = unreal.load_asset(RTG)
    if not src_mesh or not tgt_mesh or not rtg:
        unreal.log_error("[RetargetGASP] FAILED to load mesh/rtg: src=%s tgt=%s rtg=%s" % (src_mesh, tgt_mesh, rtg))
        return

    # The batch op takes FAssetData, not loaded objects.
    assets = []
    for path in ANIMS:
        ad = unreal.EditorAssetLibrary.find_asset_data(path)
        if not ad.is_valid():
            unreal.log_error("[RetargetGASP] missing source anim: " + path)
            return
        assets.append(ad)
    unreal.log("[RetargetGASP] loaded %d source anim asset-datas" % len(assets))

    # duplicate_and_retarget: suffix-mode duplication in-place, retargeted onto tgt_mesh.
    op = unreal.IKRetargetBatchOperation()
    try:
        result = op.duplicate_and_retarget(assets, src_mesh, tgt_mesh, rtg, "", "", "", "_RT")
    except Exception as e:
        unreal.log_error("[RetargetGASP] duplicate_and_retarget signature mismatch: %s" % e)
        unreal.log("[RetargetGASP] dir(op): %s" % [m for m in dir(op) if "retarget" in m.lower()])
        return
    unreal.log("[RetargetGASP] batch op returned: %s" % str(result))

    # Move + rename outputs (the dup lands next to its source with the _RT suffix).
    eal = unreal.EditorAssetLibrary
    ok = 0
    for src_path, new_name in ANIMS.items():
        dup_path = src_path + "_RT"
        dst_path = OUT_DIR + "/" + new_name
        if not eal.does_asset_exist(dup_path):
            unreal.log_error("[RetargetGASP] expected dup missing: " + dup_path)
            continue
        if eal.does_asset_exist(dst_path):
            eal.delete_asset(dst_path)
        if not eal.rename_asset(dup_path, dst_path):
            unreal.log_error("[RetargetGASP] rename failed: %s -> %s" % (dup_path, dst_path))
            continue
        # Verify the baked anim targets OUR skeleton.
        baked = unreal.load_asset(dst_path)
        skel = baked.get_editor_property("skeleton") if baked else None
        skel_path = skel.get_path_name() if skel else "NONE"
        if "/Game/Characters/Mannequins/Meshes/SK_Mannequin" in skel_path:
            ok += 1
            unreal.log("[RetargetGASP] OK %s (skeleton=%s)" % (dst_path, skel_path))
        else:
            unreal.log_error("[RetargetGASP] WRONG SKELETON %s -> %s" % (dst_path, skel_path))

    saved = eal.save_directory(OUT_DIR, only_if_is_dirty=False, recursive=True)
    unreal.log("[RetargetGASP] RESULT %d/%d baked, save_directory=%s" % (ok, len(ANIMS), saved))

main()
