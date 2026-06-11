# Create the v3 cards (D-0020): 4 behavior evolutions (chest), the Vanguard taunt track, and
# the Bombardier barrage track. Sets RequiredHeroClass on hero-track cards from the GameMode's
# HeroPawnClasses (0 = Vanguard, 1 = Bombardier). Appends all 11 to BP_RaidGamemode.UpgradePool.
#
# Headless commandlet; run with -run=pythonscript AFTER the Task-1 C++ build (the new attributes
# must exist). Editor must be CLOSED.
#
# FORBIDDEN: do not load or save GE_Upgrade_ChainDetonation (blueprint) — another workstream owns it.
#
# Success marker: unreal.log('L3C-DONE ge=11 da=11 pool=<n>')
# Raises RuntimeError on any failure.

import re
import unreal

GE_DIR = '/Game/Blueprints/GameplayEffects/LoopV3'
DA_DIR = '/Game/Upgrades'
GAMEMODE_BP = '/Game/Blueprints/BP_RaidGamemode'
TEMPLATE_COMBAT = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MoveSpeed_T1'
TEMPLATE_HEALTH = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MaxHealth_T1'

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def load_template_mod(ge_path, token):
    cls = unreal.load_object(None, ge_path + '.' + ge_path.split('/')[-1] + '_C')
    if cls is None:
        raise RuntimeError(f'L3C-FATAL: template GE class not found: {ge_path}')
    cdo = unreal.get_default_object(cls)
    mods = cdo.get_editor_property('modifiers')
    if len(mods) == 0:
        raise RuntimeError(f'L3C-FATAL: template GE has no modifiers: {ge_path}')
    txt = mods[0].export_text()
    if token not in txt or 'AddBase' not in txt:
        raise RuntimeError(f'L3C-FATAL: template missing "{token}"/"AddBase": {txt}')
    return txt


tmpl_combat = load_template_mod(TEMPLATE_COMBAT, 'MoveSpeed')
tmpl_health = load_template_mod(TEMPLATE_HEALTH, 'MaxHealth')


def make_mod(template_text, from_token, to_attr, magnitude):
    txt = template_text.replace(from_token, to_attr)
    txt = re.sub(r'ScalableFloatMagnitude=\(Value=[-0-9.]+',
                 f'ScalableFloatMagnitude=(Value={magnitude:.6f}', txt)
    mod = unreal.GameplayModifierInfo()
    mod.import_text(txt)
    check = mod.export_text()
    if to_attr not in check or 'AddBase' not in check:
        raise RuntimeError(f'L3C-FATAL: modifier round-trip failed for {to_attr}: {check}')
    unreal.log(f'L3C: mod OK {to_attr} mag={magnitude}')
    return mod


def make_ge(ge_name, mods_list):
    ge_path = f'{GE_DIR}/{ge_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(ge_path):
        bp = unreal.EditorAssetLibrary.load_asset(ge_path)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.GameplayEffect)
        bp = asset_tools.create_asset(ge_name, GE_DIR, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError(f'L3C-FATAL: failed to create GE {ge_path}')
    cdo = unreal.get_default_object(bp.generated_class())
    cdo.set_editor_property('duration_policy', unreal.GameplayEffectDurationType.INSTANT)
    cdo.set_editor_property('modifiers', mods_list)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    if not unreal.EditorAssetLibrary.save_asset(ge_path, only_if_is_dirty=False):
        raise RuntimeError(f'L3C-FATAL: save failed {ge_path}')
    unreal.log(f'L3C: GE OK {ge_path}')
    return bp


ref_da = unreal.EditorAssetLibrary.load_asset(f'{DA_DIR}/DA_Upgrade_Swift')
if ref_da is None:
    raise RuntimeError('L3C-FATAL: DA_Upgrade_Swift not found')
upgrade_def_class = ref_da.get_class()


def make_da(da_name, ge_bp, display, rarity, value_text, desc,
            bMilestone=False, bPrereqSelf=False, bApplyToSquad=False, bSynergyUpgrade=False,
            MaxStacks=5, PrereqA=None, PrereqAStacks=1, PrereqB=None, PrereqBStacks=1,
            RequiredHeroClass=None):
    da_path = f'{DA_DIR}/{da_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(da_path):
        da = unreal.EditorAssetLibrary.load_asset(da_path)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property('data_asset_class', upgrade_def_class)
        da = asset_tools.create_asset(da_name, DA_DIR, None, f)
    if da is None:
        raise RuntimeError(f'L3C-FATAL: failed to create DA {da_path}')
    da.set_editor_property('DisplayName', display)
    da.set_editor_property('Description', desc)
    da.set_editor_property('ValueText', value_text)
    da.set_editor_property('Rarity', rarity)
    da.set_editor_property('Effect', ge_bp.generated_class())
    da.set_editor_property('MaxStacks', MaxStacks)
    da.set_editor_property('bMilestone', bMilestone)
    da.set_editor_property('bApplyToSquad', bApplyToSquad)
    da.set_editor_property('bSynergyUpgrade', bSynergyUpgrade)
    da.set_editor_property('bPrereqSelf', bPrereqSelf)
    if PrereqA is not None:
        da.set_editor_property('PrereqA', PrereqA)
        da.set_editor_property('PrereqAStacks', PrereqAStacks)
    if PrereqB is not None:
        da.set_editor_property('PrereqB', PrereqB)
        da.set_editor_property('PrereqBStacks', PrereqBStacks)
    if RequiredHeroClass is not None:
        da.set_editor_property('RequiredHeroClass', RequiredHeroClass)
    if not unreal.EditorAssetLibrary.save_asset(da_path, only_if_is_dirty=False):
        raise RuntimeError(f'L3C-FATAL: save failed {da_path}')
    unreal.log(f'L3C: DA OK {da_path} ("{display}" r{rarity})')
    return da


def load_existing_da(name):
    da = unreal.EditorAssetLibrary.load_asset(f'{DA_DIR}/{name}')
    if da is None:
        raise RuntimeError(f'L3C-FATAL: prereq DA not found: {name}')
    return da


# Prereq references
da_Wildfire = load_existing_da('DA_Synergy_Wildfire')
da_VenomCascade = load_existing_da('DA_Synergy_VenomCascade')
da_SynOverwhelm = load_existing_da('DA_Synergy_Overwhelm')
da_IronVanguard = load_existing_da('DA_Synergy_IronVanguard')
da_WideBarrage = load_existing_da('DA_Upgrade_WideBarrage')

# Hero classes from the GameMode (0 = Vanguard, 1 = Bombardier)
gm_bp = unreal.EditorAssetLibrary.load_asset(GAMEMODE_BP)
gm_class = unreal.load_object(None, GAMEMODE_BP + '.BP_RaidGamemode_C')
if gm_bp is None or gm_class is None:
    raise RuntimeError('L3C-FATAL: BP_RaidGamemode not found')
gm_cdo = unreal.get_default_object(gm_class)
hero_classes = list(gm_cdo.get_editor_property('HeroPawnClasses'))
if len(hero_classes) < 2 or hero_classes[0] is None or hero_classes[1] is None:
    raise RuntimeError(f'L3C-FATAL: HeroPawnClasses needs Vanguard+Bombardier, got {hero_classes}')
vanguard_cls, bombardier_cls = hero_classes[0], hero_classes[1]
unreal.log(f'L3C: heroes vanguard={vanguard_cls.get_name()} bombardier={bombardier_cls.get_name()}')

pool_das = []

# ---- 4 behavior evolutions (chest: synergy-class, squad-wide, 1 stack, base card prereq) ----
ge = make_ge('GE_Evo_SearingArcs', [make_mod(tmpl_combat, 'MoveSpeed', 'ChainIgniteFraction', 0.5)])
pool_das.append(make_da('DA_Evo_SearingArcs', ge, 'Searing Arcs', 3,
    'Chain arcs ignite their targets (squad)',
    'The arc carries heat. Everything it touches burns.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_Wildfire))

ge = make_ge('GE_Evo_ToxicBurst', [make_mod(tmpl_combat, 'MoveSpeed', 'PoisonBurstDps', 6.0)])
pool_das.append(make_da('DA_Evo_ToxicBurst', ge, 'Toxic Burst', 3,
    'Poisoned enemies burst a poison cloud on death (squad)',
    'The toxin does not die with the host. It looks for the next one.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_VenomCascade))

ge = make_ge('GE_Evo_Overwhelm', [make_mod(tmpl_combat, 'MoveSpeed', 'ClusterChainBonusArcs', 2.0)])
pool_das.append(make_da('DA_Evo_Overwhelm', ge, 'Critical Mass', 3,
    'Hits on Clustered enemies arc to +2 extra targets (squad)',
    'Pack them tight enough and the lightning does the rest.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_SynOverwhelm))

ge = make_ge('GE_Evo_IronBulwark', [
    make_mod(tmpl_combat, 'MoveSpeed', 'ClusterKillShieldAmount', 3.0),
    make_mod(tmpl_health, 'MaxHealth', 'MaxShield', 25.0),
])
pool_das.append(make_da('DA_Evo_IronBulwark', ge, 'Iron Bulwark', 3,
    '+25 Max Shield; Clustered kills restore 3 Shield (squad)',
    'Hold the line together and the line holds you.',
    bSynergyUpgrade=True, bApplyToSquad=True, MaxStacks=1, PrereqA=da_IronVanguard))

# ---- Vanguard taunt track ----
ge = make_ge('GE_Upgrade_MagneticPull', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntRadiusBonus', 150.0)])
da_MagneticPull = make_da('DA_Upgrade_MagneticPull', ge, 'Magnetic Pull', 1,
    '+150 Taunt radius', 'The pull reaches further. Nothing escapes the gravity of a good taunt.',
    MaxStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_MagneticPull)

ge = make_ge('GE_Upgrade_IronGrip', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntClusterDurationBonus', 1.0)])
da_IronGrip = make_da('DA_Upgrade_IronGrip', ge, 'Iron Grip', 1,
    '+1s Clustered duration', 'They stay where you put them.',
    MaxStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_IronGrip)

ge = make_ge('GE_Upgrade_ConcussiveTaunt', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntDamage', 30.0)])
da_ConcussiveTaunt = make_da('DA_Upgrade_ConcussiveTaunt', ge, 'Concussive Taunt', 2,
    'Taunt deals 30 damage (Ability Power scaled)',
    'The shout hits like a shockwave now.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=2,
    PrereqA=da_MagneticPull, PrereqAStacks=3, RequiredHeroClass=vanguard_cls)
pool_das.append(da_ConcussiveTaunt)

ge = make_ge('GE_Evo_EventHorizon', [make_mod(tmpl_combat, 'MoveSpeed', 'TauntVortex', 1.0)])
pool_das.append(make_da('DA_Evo_EventHorizon', ge, 'Event Horizon', 3,
    'Taunt becomes a 3s vortex: re-pulls and refreshes Clustered',
    'Past this point, nothing leaves.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_ConcussiveTaunt, PrereqAStacks=1,
    PrereqB=da_IronGrip, PrereqBStacks=2, RequiredHeroClass=vanguard_cls))

# ---- Bombardier barrage track ----
ge = make_ge('GE_Upgrade_HighExplosives', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageDamageBonus', 0.25)])
da_HighExplosives = make_da('DA_Upgrade_HighExplosives', ge, 'High Explosives', 1,
    '+25% Barrage damage', 'More filler, more killer.',
    MaxStacks=3, RequiredHeroClass=bombardier_cls)
pool_das.append(da_HighExplosives)

ge = make_ge('GE_Upgrade_TwinSalvo', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageSalvoCount', 1.0)])
da_TwinSalvo = make_da('DA_Upgrade_TwinSalvo', ge, 'Twin Salvo', 2,
    'Barrage strikes a second time (60% damage)',
    'The first shell is the question. The second is the answer.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_HighExplosives, PrereqAStacks=3, RequiredHeroClass=bombardier_cls)
pool_das.append(da_TwinSalvo)

ge = make_ge('GE_Evo_CarpetBombing', [make_mod(tmpl_combat, 'MoveSpeed', 'BarrageCarpet', 1.0)])
pool_das.append(make_da('DA_Evo_CarpetBombing', ge, 'Carpet Bombing', 3,
    'Barrage becomes a telegraphed strip of 5 blasts marching forward',
    'Walk the line. Or rather — make them try.',
    bMilestone=True, bPrereqSelf=True, MaxStacks=1,
    PrereqA=da_TwinSalvo, PrereqAStacks=1,
    PrereqB=da_WideBarrage, PrereqBStacks=2, RequiredHeroClass=bombardier_cls))

# ---- Pool wiring ----
pool = list(gm_cdo.get_editor_property('UpgradePool'))
before = len(pool)
existing = {p.get_name() for p in pool if p is not None}
for da in pool_das:
    if da.get_name() not in existing:
        pool.append(da)
        existing.add(da.get_name())
gm_cdo.set_editor_property('UpgradePool', pool)
unreal.BlueprintEditorLibrary.compile_blueprint(gm_bp)
if not unreal.EditorAssetLibrary.save_asset(GAMEMODE_BP, only_if_is_dirty=False):
    raise RuntimeError('L3C-FATAL: BP_RaidGamemode save failed')
unreal.log(f'L3C: UpgradePool {before} -> {len(pool)}')

unreal.log(f'L3C-DONE ge=11 da=11 pool={len(pool)}')
