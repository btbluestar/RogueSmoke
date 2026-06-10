# Read every DA_Upgrade_* def + its GameplayEffect modifiers so card ValueText lines can be
# authored with the REAL numbers. Read-only.
import unreal

assets = unreal.EditorAssetLibrary.list_assets('/Game/Upgrades', recursive=True)
for path in sorted(assets):
    da = unreal.EditorAssetLibrary.load_asset(path)
    if da is None or not isinstance(da, unreal.RogueUpgradeDef):
        continue
    name = da.get_editor_property('DisplayName')
    desc = da.get_editor_property('Description')
    rarity = da.get_editor_property('Rarity')
    value = da.get_editor_property('ValueText')
    effect = da.get_editor_property('Effect')
    unreal.log(f'=== {path}')
    unreal.log(f'  Name={name} | Rarity={rarity} | ValueText="{value}"')
    unreal.log(f'  Desc={desc}')
    if effect is None:
        unreal.log('  Effect=None')
        continue
    cdo = unreal.get_default_object(effect)
    try:
        mods = cdo.get_editor_property('Modifiers')
        for m in mods:
            attr = m.get_editor_property('Attribute')
            op = m.get_editor_property('ModifierOp')
            mag = m.get_editor_property('ModifierMagnitude')
            sf = mag.get_editor_property('ScalableFloatMagnitude')
            val = sf.get_editor_property('Value')
            unreal.log(f'  MOD attr={attr.get_editor_property("AttributeName")} op={op} value={val}')
    except Exception as e:
        unreal.log(f'  (modifier read failed: {e})')
unreal.log('DONE-READ-UPGRADES')
