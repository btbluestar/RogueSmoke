# Mark the synergy upgrade card(s): bSynergyUpgrade=True means the card comes ONLY from the
# mini-boss chest, never from level-up offers (UpgradeLoop concept, 2026-06-11). Currently just
# Chain Detonation — the glossary's canonical cross-player synergy upgrade. Headless commandlet.
import unreal

SYNERGY_CARDS = ['/Game/Upgrades/DA_Upgrade_ChainDetonation']

ok = 0
for path in SYNERGY_CARDS:
    da = unreal.EditorAssetLibrary.load_asset(path)
    if da is None:
        unreal.log_error(f'SYN-ERR: missing {path}')
        continue
    da.set_editor_property('bSynergyUpgrade', True)
    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        ok += 1
        unreal.log(f'SYN: marked {path}')
    else:
        unreal.log_error(f'SYN-ERR: save failed {path}')

unreal.log(f'SYN-DONE {ok}/{len(SYNERGY_CARDS)}')
