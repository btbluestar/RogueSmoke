# Author the card ValueText line on each DA_Upgrade_* (numbers taken from each def's actual
# GE modifier values — see py_read_upgrade_defs.py output 2026-06-10).
import unreal

VALUES = {
    '/Game/Upgrades/DA_Upgrade_Bulwark':         '+10 Armor',
    '/Game/Upgrades/DA_Upgrade_ChainDetonation': '+100% Barrage dmg vs Clustered',
    '/Game/Upgrades/DA_Upgrade_Cooldown':        '+10% Cooldown Reduction',
    '/Game/Upgrades/DA_Upgrade_Overshield':      '+50 Max Shield',
    '/Game/Upgrades/DA_Upgrade_Power':           '+20% Ability Power',
    '/Game/Upgrades/DA_Upgrade_Swift':           '+90 Move Speed',
    '/Game/Upgrades/DA_Upgrade_Vitality':        '+25 Max Health',
    '/Game/Upgrades/DA_Upgrade_WideBarrage':     '+150 Barrage Radius',
}

ok = 0
for path, value in VALUES.items():
    da = unreal.EditorAssetLibrary.load_asset(path)
    if da is None:
        unreal.log_error(f'MISSING {path}')
        continue
    da.set_editor_property('ValueText', value)
    if unreal.EditorAssetLibrary.save_asset(path):
        ok += 1
        unreal.log(f'SET {path} -> "{value}"')
    else:
        unreal.log_error(f'SAVE-FAILED {path}')
unreal.log(f'DONE-SET-VALUES ok={ok}/{len(VALUES)}')
