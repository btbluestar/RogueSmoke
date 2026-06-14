# Create the 8 weapon-upgrade GameplayEffects + URogueUpgradeDef cards and append them to
# BP_RaidGamemode.UpgradePool (WEAPON_UPGRADES_PLAN.md). Headless commandlet; run with
# -run=pythonscript. All GEs are INSTANT + ADD_BASE flat adds on URogueCombatSet attributes
# (memory: ge-modifier-editing-via-python — percentages live IN the attribute as fractions,
# so a flat ADD_BASE is correct and stacks on repeat picks).
#
# Modifier authoring: modifier_op is locked for set_editor_property, so we export_text() a
# known-good ADD_BASE modifier from GE_Upgrade_MoveSpeed_T1 (same attribute set class) and
# string-replace the attribute name + magnitude, then import_text() into a fresh struct.
import re
import unreal

GE_DIR = '/Game/Blueprints/GameplayEffects/Weapons'
DA_DIR = '/Game/Upgrades'
TEMPLATE_GE = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MoveSpeed_T1'
GAMEMODE_BP = '/Game/Blueprints/BP_RaidGamemode'

# (ge_name, attribute, magnitude, da_name, display, rarity, value_text, description)
UPGRADES = [
    ('GE_Upgrade_WeaponDamage_T1', 'WeaponDamageBonus', 0.20, 'DA_Upgrade_HeavyCaliber',
     'Heavy Caliber', 1, '+20% Weapon Damage',
     'Denser slugs. Every bullet hits harder.'),
    ('GE_Upgrade_FireRate_T1', 'FireRateBonus', 0.25, 'DA_Upgrade_Overclock',
     'Overclocked Receiver', 1, '+25% Fire Rate',
     'The action cycles faster than spec allows.'),
    ('GE_Upgrade_Magazine_T1', 'MagazineBonus', 0.50, 'DA_Upgrade_ExtendedDrum',
     'Extended Drum', 1, '+50% Magazine Size',
     'More chamber, fewer pauses.'),
    ('GE_Upgrade_ReloadSpeed_T1', 'ReloadSpeedBonus', 0.30, 'DA_Upgrade_GreasedBolt',
     'Greased Bolt', 1, '+30% Reload Speed',
     'Hands move on muscle memory.'),
    ('GE_Upgrade_Pierce_T2', 'PierceCount', 1.0, 'DA_Upgrade_PiercingRounds',
     'Piercing Rounds', 2, 'Shots pierce +1 enemy',
     'Overpenetration is a feature. Line them up.'),
    ('GE_Upgrade_ChainShot_T2', 'ChainCount', 2.0, 'DA_Upgrade_ArcConductor',
     'Arc Conductor', 2, 'Hits arc to 2 nearby enemies',
     'Charged rounds leap to whatever stands too close (50% damage).'),
    ('GE_Upgrade_Burn_T3', 'BurnChance', 0.25, 'DA_Upgrade_IncendiaryRounds',
     'Incendiary Rounds', 3, '25% chance to ignite',
     'Burns for 50% of the hit over 3s. The horde is flammable.'),
    ('GE_Upgrade_Poison_T3', 'PoisonChance', 0.25, 'DA_Upgrade_Neurotoxin',
     'Neurotoxin Coating', 3, '25% chance to poison',
     '100% of the hit as venom over 6s. Patience kills.'),
]

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# --- Template modifier text (ADD_BASE on URogueCombatSet, post-b6c058b known-good) ---
tmpl_class = unreal.load_object(None, TEMPLATE_GE + '.GE_Upgrade_MoveSpeed_T1_C')
if tmpl_class is None:
    raise RuntimeError('WUP-FATAL: template GE class not found')
tmpl_cdo = unreal.get_default_object(tmpl_class)
tmpl_mods = tmpl_cdo.get_editor_property('modifiers')
if len(tmpl_mods) == 0:
    raise RuntimeError('WUP-FATAL: template GE has no modifiers')
template_text = tmpl_mods[0].export_text()
unreal.log(f'WUP: template modifier = {template_text}')
if 'MoveSpeed' not in template_text or 'AddBase' not in template_text:
    raise RuntimeError('WUP-FATAL: template modifier missing expected tokens')

# --- URogueUpgradeDef class (AngelScript-defined): take it from an existing DA asset ---
existing = unreal.EditorAssetLibrary.load_asset(DA_DIR + '/DA_Upgrade_Swift')
if existing is None:
    raise RuntimeError('WUP-FATAL: DA_Upgrade_Swift not found')
upgrade_def_class = existing.get_class()
unreal.log(f'WUP: upgrade def class = {upgrade_def_class.get_path_name()}')

ok_ge = 0
ok_da = 0
new_das = []

for ge_name, attr, magnitude, da_name, display, rarity, value_text, desc in UPGRADES:
    ge_path = f'{GE_DIR}/{ge_name}'

    # 1) GE blueprint (skip if a previous run already made it — idempotent re-runs).
    if unreal.EditorAssetLibrary.does_asset_exist(ge_path):
        bp = unreal.EditorAssetLibrary.load_asset(ge_path)
        unreal.log(f'WUP: GE exists, reusing {ge_path}')
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.GameplayEffect)
        bp = asset_tools.create_asset(ge_name, GE_DIR, unreal.Blueprint, factory)
    if bp is None:
        unreal.log_error(f'WUP-ERR: failed to create {ge_path}')
        continue

    gen_class = bp.generated_class()
    cdo = unreal.get_default_object(gen_class)
    cdo.set_editor_property('duration_policy', unreal.GameplayEffectDurationType.INSTANT)

    mod_text = template_text.replace('MoveSpeed', attr)
    mod_text = re.sub(r'ScalableFloatMagnitude=\(Value=[-0-9.]+',
                      f'ScalableFloatMagnitude=(Value={magnitude:.6f}', mod_text)
    mod = unreal.GameplayModifierInfo()
    mod.import_text(mod_text)
    check = mod.export_text()
    if attr not in check or 'AddBase' not in check:
        unreal.log_error(f'WUP-ERR: modifier import_text round-trip failed for {ge_name}: {check}')
        continue
    cdo.set_editor_property('modifiers', [mod])

    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    if not unreal.EditorAssetLibrary.save_asset(ge_path, only_if_is_dirty=False):
        unreal.log_error(f'WUP-ERR: save failed {ge_path}')
        continue
    ok_ge += 1
    unreal.log(f'WUP: GE OK {ge_path} ({attr} +{magnitude})')

    # 2) Upgrade card DataAsset pointing at the GE class.
    da_path = f'{DA_DIR}/{da_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(da_path):
        da = unreal.EditorAssetLibrary.load_asset(da_path)
        unreal.log(f'WUP: DA exists, reusing {da_path}')
    else:
        da_factory = unreal.DataAssetFactory()
        da_factory.set_editor_property('data_asset_class', upgrade_def_class)
        da = asset_tools.create_asset(da_name, DA_DIR, None, da_factory)
    if da is None:
        unreal.log_error(f'WUP-ERR: failed to create {da_path}')
        continue

    da.set_editor_property('DisplayName', display)
    da.set_editor_property('Description', desc)
    da.set_editor_property('ValueText', value_text)
    da.set_editor_property('Rarity', rarity)
    da.set_editor_property('Effect', gen_class)
    if not unreal.EditorAssetLibrary.save_asset(da_path, only_if_is_dirty=False):
        unreal.log_error(f'WUP-ERR: save failed {da_path}')
        continue
    ok_da += 1
    new_das.append(da)
    unreal.log(f'WUP: DA OK {da_path} ("{display}" r{rarity} -> {ge_name})')

# 3) Append the new cards to BP_RaidGamemode.UpgradePool (CDO edit -> compile -> save,
#    memory: mcp-bp-cdo-needs-compile).
gm_bp = unreal.EditorAssetLibrary.load_asset(GAMEMODE_BP)
gm_class = unreal.load_object(None, GAMEMODE_BP + '.BP_RaidGamemode_C')
if gm_bp is None or gm_class is None:
    raise RuntimeError('WUP-FATAL: BP_RaidGamemode not found')
gm_cdo = unreal.get_default_object(gm_class)
pool = list(gm_cdo.get_editor_property('UpgradePool'))
before = len(pool)
existing_names = {p.get_name() for p in pool if p is not None}
for da in new_das:
    if da.get_name() not in existing_names:
        pool.append(da)
gm_cdo.set_editor_property('UpgradePool', pool)
unreal.BlueprintEditorLibrary.compile_blueprint(gm_bp)
if not unreal.EditorAssetLibrary.save_asset(GAMEMODE_BP, only_if_is_dirty=False):
    raise RuntimeError('WUP-FATAL: BP_RaidGamemode save failed')
unreal.log(f'WUP: pool {before} -> {len(pool)}')

unreal.log(f'WUP-DONE ge={ok_ge}/8 da={ok_da}/8 pool={len(pool)}')
