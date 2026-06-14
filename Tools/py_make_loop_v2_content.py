# Create milestone, synergy, and utility upgrade cards for Loop v2 (D-0019).
# Also retunes MaxStacks/prereqs on existing pool DAs, and updates BP_RaidGamemode
# UpgradePool + UtilityPool.
#
# Headless commandlet; run with -run=pythonscript.
# Pattern: identical to py_make_weapon_upgrades.py — template-GE export_text/import_text
# for ADD_BASE modifiers; idempotent re-runs; CDO -> compile_blueprint -> save_asset for BP.
#
# Templates:
#   Combat-set attrs (MoveSpeed, WeaponDamageBonus, FireRateBonus, PierceCount, ChainCount,
#                     BurnChance, PoisonChance, BarrageRadiusBonus):
#       /Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MoveSpeed_T1  (token: 'MoveSpeed')
#   Health-set attrs (MaxHealth, MaxShield):
#       /Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MaxHealth_T1  (token: 'MaxHealth')
#   Health-set attr 'Health' (heal, not MaxHealth):
#       same MaxHealth template but token-replace '.MaxHealth' -> '.Health' only in the attribute path
#
# Multi-modifier GEs (synergy cards): build a list of two imported GameplayModifierInfo structs.
#
# New GE assets -> /Game/Blueprints/GameplayEffects/LoopV2/
# New DA assets -> /Game/Upgrades/
#
# Existing-pool retunes (MaxStacks, prereqs):
#   DA_Upgrade_PiercingRounds  MaxStacks=3
#   DA_Upgrade_ArcConductor    MaxStacks=3
#   DA_Upgrade_IncendiaryRounds MaxStacks=2
#   DA_Upgrade_Neurotoxin      MaxStacks=2
#   DA_Upgrade_ChainDetonation PrereqA=DA_Upgrade_WideBarrage, PrereqB=DA_Upgrade_Power,
#                               bApplyToSquad=True (DA only — GE blueprint off-limits)
#
# FORBIDDEN: do not load or save GE_Upgrade_ChainDetonation (blueprint) — another workstream owns it.
#
# Success marker: unreal.log('L2C-DONE ge=... da=... retuned=... pool=... utility=...')
# Raises RuntimeError on any failure.

import re
import unreal

GE_DIR   = '/Game/Blueprints/GameplayEffects/LoopV2'
DA_DIR   = '/Game/Upgrades'
GAMEMODE_BP = '/Game/Blueprints/BP_RaidGamemode'

TEMPLATE_COMBAT  = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MoveSpeed_T1'
TEMPLATE_HEALTH  = '/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MaxHealth_T1'

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# -------------------------------------------------------------------------
# 1) Load template modifier export_text strings
# -------------------------------------------------------------------------

def load_template_mod(ge_path, token, expected_token=None):
    """Load the first modifier from a GE blueprint and return its export_text string.
    Verifies the expected token is present."""
    check_token = expected_token or token
    cls = unreal.load_object(None, ge_path + '.' + ge_path.split('/')[-1] + '_C')
    if cls is None:
        raise RuntimeError(f'L2C-FATAL: template GE class not found: {ge_path}')
    cdo = unreal.get_default_object(cls)
    mods = cdo.get_editor_property('modifiers')
    if len(mods) == 0:
        raise RuntimeError(f'L2C-FATAL: template GE has no modifiers: {ge_path}')
    txt = mods[0].export_text()
    unreal.log(f'L2C: template({token}) = {txt}')
    if check_token not in txt or 'AddBase' not in txt:
        raise RuntimeError(f'L2C-FATAL: template modifier missing token "{check_token}" or "AddBase": {txt}')
    return txt

tmpl_combat = load_template_mod(TEMPLATE_COMBAT, 'MoveSpeed')
tmpl_health = load_template_mod(TEMPLATE_HEALTH, 'MaxHealth')

# -------------------------------------------------------------------------
# 2) Helper: build a GameplayModifierInfo from a template
# -------------------------------------------------------------------------

def make_mod(template_text, from_token, to_attr, magnitude, is_health_template=False):
    """
    Build one GameplayModifierInfo via import_text round-trip.
    - template_text: the raw export_text from the template GE modifier
    - from_token: the attribute name token to replace in the template (e.g. 'MoveSpeed', 'MaxHealth')
    - to_attr: the target attribute name (e.g. 'PierceCount', 'Health', 'MaxShield')
    - magnitude: float
    - is_health_template: if True, we're replacing the health-set attribute path token
    Returns the imported GameplayModifierInfo.
    """
    txt = template_text

    if is_health_template and to_attr == 'Health':
        # The health template has the attribute path segment '.MaxHealth'.
        # We need ONLY the attribute token replaced, not any other 'MaxHealth' occurrences.
        # The attribute appears as 'URogueHealthSet.MaxHealth' in the text.
        # Replace '.MaxHealth' -> '.Health' carefully (only the attribute path segment).
        # This changes URogueHealthSet.MaxHealth -> URogueHealthSet.Health.
        txt = txt.replace('.MaxHealth', '.Health')
        # Verify we didn't accidentally leave MaxHealth in the attribute portion
        if '.MaxHealth' in txt:
            raise RuntimeError(f'L2C-FATAL: ".MaxHealth" still present after replace for Health attr: {txt}')
        check_attr = 'Health'
    elif is_health_template:
        # Other health-set attrs (MaxHealth, MaxShield): replace the from_token in attribute path
        txt = txt.replace(from_token, to_attr)
        check_attr = to_attr
    else:
        # Combat-set attrs: replace the from_token (MoveSpeed) with the target attr
        txt = txt.replace(from_token, to_attr)
        check_attr = to_attr

    # Replace magnitude
    txt = re.sub(r'ScalableFloatMagnitude=\(Value=[-0-9.]+',
                 f'ScalableFloatMagnitude=(Value={magnitude:.6f}', txt)

    mod = unreal.GameplayModifierInfo()
    mod.import_text(txt)
    check = mod.export_text()
    unreal.log(f'L2C: mod round-trip({to_attr} mag={magnitude:.6f}) = {check}')
    if check_attr not in check or 'AddBase' not in check:
        raise RuntimeError(f'L2C-FATAL: modifier import_text round-trip failed for {to_attr}: {check}')
    return mod

# -------------------------------------------------------------------------
# 3) Helper: create or reuse a GE blueprint with given modifiers list
# -------------------------------------------------------------------------

def make_ge(ge_name, mods_list):
    """Create (or reuse) a GE blueprint in GE_DIR with the given list of GameplayModifierInfo."""
    ge_path = f'{GE_DIR}/{ge_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(ge_path):
        bp = unreal.EditorAssetLibrary.load_asset(ge_path)
        unreal.log(f'L2C: GE exists, reusing {ge_path}')
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.GameplayEffect)
        bp = asset_tools.create_asset(ge_name, GE_DIR, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError(f'L2C-FATAL: failed to create GE {ge_path}')

    gen_class = bp.generated_class()
    cdo = unreal.get_default_object(gen_class)
    cdo.set_editor_property('duration_policy', unreal.GameplayEffectDurationType.INSTANT)
    cdo.set_editor_property('modifiers', mods_list)

    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    if not unreal.EditorAssetLibrary.save_asset(ge_path, only_if_is_dirty=False):
        raise RuntimeError(f'L2C-FATAL: save failed {ge_path}')

    unreal.log(f'L2C: GE OK {ge_path}')
    return bp

# -------------------------------------------------------------------------
# 4) Load URogueUpgradeDef class from an existing DA
# -------------------------------------------------------------------------

existing_da_ref = unreal.EditorAssetLibrary.load_asset(f'{DA_DIR}/DA_Upgrade_Swift')
if existing_da_ref is None:
    raise RuntimeError('L2C-FATAL: DA_Upgrade_Swift not found — cannot get URogueUpgradeDef class')
upgrade_def_class = existing_da_ref.get_class()
unreal.log(f'L2C: upgrade_def_class = {upgrade_def_class.get_path_name()}')

# -------------------------------------------------------------------------
# 5) Helper: create or reuse a DataAsset, set common fields
# -------------------------------------------------------------------------

def make_da(da_name, ge_bp, display, rarity, value_text, desc,
            bMilestone=False, bPrereqSelf=False, bApplyToSquad=False, bSynergyUpgrade=False,
            MaxStacks=5, PrereqA=None, PrereqAStacks=1, PrereqB=None, PrereqBStacks=1):
    da_path = f'{DA_DIR}/{da_name}'
    if unreal.EditorAssetLibrary.does_asset_exist(da_path):
        da = unreal.EditorAssetLibrary.load_asset(da_path)
        unreal.log(f'L2C: DA exists, reusing {da_path}')
    else:
        da_factory = unreal.DataAssetFactory()
        da_factory.set_editor_property('data_asset_class', upgrade_def_class)
        da = asset_tools.create_asset(da_name, DA_DIR, None, da_factory)
    if da is None:
        raise RuntimeError(f'L2C-FATAL: failed to create DA {da_path}')

    gen_class = ge_bp.generated_class()
    da.set_editor_property('DisplayName', display)
    da.set_editor_property('Description', desc)
    da.set_editor_property('ValueText', value_text)
    da.set_editor_property('Rarity', rarity)
    da.set_editor_property('Effect', gen_class)
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

    if not unreal.EditorAssetLibrary.save_asset(da_path, only_if_is_dirty=False):
        raise RuntimeError(f'L2C-FATAL: save failed {da_path}')
    unreal.log(f'L2C: DA OK {da_path} ("{display}" r{rarity})')
    return da

# -------------------------------------------------------------------------
# 6) Load existing DAs we reference as prereqs
# -------------------------------------------------------------------------

def load_existing_da(name):
    path = f'{DA_DIR}/{name}'
    da = unreal.EditorAssetLibrary.load_asset(path)
    if da is None:
        raise RuntimeError(f'L2C-FATAL: prereq DA not found: {path}')
    unreal.log(f'L2C: loaded prereq {path}')
    return da

da_PiercingRounds    = load_existing_da('DA_Upgrade_PiercingRounds')
da_ArcConductor      = load_existing_da('DA_Upgrade_ArcConductor')
da_IncendiaryRounds  = load_existing_da('DA_Upgrade_IncendiaryRounds')
da_Neurotoxin        = load_existing_da('DA_Upgrade_Neurotoxin')
da_HeavyCaliber      = load_existing_da('DA_Upgrade_HeavyCaliber')
da_WideBarrage       = load_existing_da('DA_Upgrade_WideBarrage')
da_Power             = load_existing_da('DA_Upgrade_Power')
da_Vitality          = load_existing_da('DA_Upgrade_Vitality')
da_Overshield        = load_existing_da('DA_Upgrade_Overshield')
da_ChainDetonation   = load_existing_da('DA_Upgrade_ChainDetonation')
# DA_Upgrade_ChainShot does not exist; Overwhelm's PrereqB uses ArcConductor (per plan footnote)
unreal.log('L2C: DA_Upgrade_ChainShot not found — using DA_Upgrade_ArcConductor for Overwhelm PrereqB (per plan)')

# -------------------------------------------------------------------------
# 7) Create utility GEs + DAs
# -------------------------------------------------------------------------

ok_ge = 0
ok_da = 0
utility_das = []
pool_das = []   # milestone + synergy cards only go to UpgradePool

# Utility 1: FieldDressing — squad heal: URogueHealthSet.Health +50
ge_FieldDressing = make_ge('GE_Upgrade_FieldDressing',
    [make_mod(tmpl_health, 'MaxHealth', 'Health', 50.0, is_health_template=True)])
ok_ge += 1
da_FieldDressing = make_da('DA_Upgrade_FieldDressing', ge_FieldDressing,
    'Field Dressing', 1, 'Restore +50 HP (squad)', 'Patch each other up. Keep moving.',
    bApplyToSquad=True, MaxStacks=0)
ok_da += 1
utility_das.append(da_FieldDressing)

# Utility 2: AdrenalSurge — MoveSpeed +0.05
ge_AdrenalSurge = make_ge('GE_Upgrade_AdrenalSurge',
    [make_mod(tmpl_combat, 'MoveSpeed', 'MoveSpeed', 0.05)])
ok_ge += 1
da_AdrenalSurge = make_da('DA_Upgrade_AdrenalSurge', ge_AdrenalSurge,
    'Adrenal Surge', 1, '+5% Move Speed', 'Adrenaline keeps the legs moving when nothing else will.',
    MaxStacks=0)
ok_da += 1
utility_das.append(da_AdrenalSurge)

# -------------------------------------------------------------------------
# 8) Create milestone GEs + DAs
# -------------------------------------------------------------------------

# Milestone 1: DrillRounds — PierceCount +2 (prereq: PiercingRounds x3)
ge_DrillRounds = make_ge('GE_Upgrade_DrillRounds',
    [make_mod(tmpl_combat, 'MoveSpeed', 'PierceCount', 2.0)])
ok_ge += 1
da_DrillRounds = make_da('DA_Upgrade_DrillRounds', ge_DrillRounds,
    'Drill Rounds', 3, 'Shots pierce +2 more enemies',
    'Hardened penetrators. The round does not stop until it runs out of things.',
    bMilestone=True, bPrereqSelf=True,
    PrereqA=da_PiercingRounds, PrereqAStacks=3)
ok_da += 1
pool_das.append(da_DrillRounds)

# Milestone 2: HeavyPayload — WeaponDamageBonus +0.40 (prereq: PiercingRounds x3)
ge_HeavyPayload = make_ge('GE_Upgrade_HeavyPayload',
    [make_mod(tmpl_combat, 'MoveSpeed', 'WeaponDamageBonus', 0.40)])
ok_ge += 1
da_HeavyPayload = make_da('DA_Upgrade_HeavyPayload', ge_HeavyPayload,
    'Heavy Payload', 3, '+40% Weapon Damage',
    'Penetrators carry mass. That mass imparts itself on everything they pass through.',
    bMilestone=True, bPrereqSelf=True,
    PrereqA=da_PiercingRounds, PrereqAStacks=3)
ok_da += 1
pool_das.append(da_HeavyPayload)

# Milestone 3: StormConductor — ChainCount +3 (prereq: ArcConductor x3)
ge_StormConductor = make_ge('GE_Upgrade_StormConductor',
    [make_mod(tmpl_combat, 'MoveSpeed', 'ChainCount', 3.0)])
ok_ge += 1
da_StormConductor = make_da('DA_Upgrade_StormConductor', ge_StormConductor,
    'Storm Conductor', 3, 'Hits arc to +3 more enemies',
    'The charge finds new paths. Every arc spawns arcs.',
    bMilestone=True, bPrereqSelf=True,
    PrereqA=da_ArcConductor, PrereqAStacks=3)
ok_da += 1
pool_das.append(da_StormConductor)

# Milestone 4: OverchargedArcs — FireRateBonus +0.40 (prereq: ArcConductor x3)
ge_OverchargedArcs = make_ge('GE_Upgrade_OverchargedArcs',
    [make_mod(tmpl_combat, 'MoveSpeed', 'FireRateBonus', 0.40)])
ok_ge += 1
da_OverchargedArcs = make_da('DA_Upgrade_OverchargedArcs', ge_OverchargedArcs,
    'Overcharged Arcs', 3, '+40% Fire Rate',
    'Cycling faster means more arcs per second. The math is simple. The field is not.',
    bMilestone=True, bPrereqSelf=True,
    PrereqA=da_ArcConductor, PrereqAStacks=3)
ok_da += 1
pool_das.append(da_OverchargedArcs)

# -------------------------------------------------------------------------
# 9) Create synergy GEs + DAs (multi-modifier, bApplyToSquad, duo prereqs)
# -------------------------------------------------------------------------

# Synergy 1: Wildfire — BurnChance +0.25, WeaponDamageBonus +0.10
#   PrereqA=IncendiaryRounds, PrereqB=HeavyCaliber
ge_Wildfire = make_ge('GE_Synergy_Wildfire', [
    make_mod(tmpl_combat, 'MoveSpeed', 'BurnChance', 0.25),
    make_mod(tmpl_combat, 'MoveSpeed', 'WeaponDamageBonus', 0.10),
])
ok_ge += 1
da_Wildfire = make_da('DA_Synergy_Wildfire', ge_Wildfire,
    'Wildfire', 3, '+25% Burn Chance, +10% Weapon Damage (squad)',
    'Burning rounds hit harder. The horde learned this too late.',
    bSynergyUpgrade=True, bApplyToSquad=True,
    PrereqA=da_IncendiaryRounds, PrereqAStacks=1,
    PrereqB=da_HeavyCaliber, PrereqBStacks=1)
ok_da += 1
pool_das.append(da_Wildfire)

# Synergy 2: VenomCascade — PoisonChance +0.25, ChainCount +1
#   PrereqA=Neurotoxin, PrereqB=ArcConductor
ge_VenomCascade = make_ge('GE_Synergy_VenomCascade', [
    make_mod(tmpl_combat, 'MoveSpeed', 'PoisonChance', 0.25),
    make_mod(tmpl_combat, 'MoveSpeed', 'ChainCount', 1.0),
])
ok_ge += 1
da_VenomCascade = make_da('DA_Synergy_VenomCascade', ge_VenomCascade,
    'Venom Cascade', 3, '+25% Poison Chance, Hits arc +1 (squad)',
    'The toxin spreads between chains. Give it enough hosts.',
    bSynergyUpgrade=True, bApplyToSquad=True,
    PrereqA=da_Neurotoxin, PrereqAStacks=1,
    PrereqB=da_ArcConductor, PrereqBStacks=1)
ok_da += 1
pool_das.append(da_VenomCascade)

# Synergy 3: Overwhelm — BarrageRadiusBonus +150, ChainCount +1
#   PrereqA=WideBarrage, PrereqB=ArcConductor (ChainShot DA doesn't exist, using ArcConductor)
ge_Overwhelm = make_ge('GE_Synergy_Overwhelm', [
    make_mod(tmpl_combat, 'MoveSpeed', 'BarrageRadiusBonus', 150.0),
    make_mod(tmpl_combat, 'MoveSpeed', 'ChainCount', 1.0),
])
ok_ge += 1
da_Overwhelm = make_da('DA_Synergy_Overwhelm', ge_Overwhelm,
    'Overwhelm', 3, 'Barrage radius +150, Hits arc +1 (squad)',
    'Bigger blast, wider arcs. Density is a death sentence.',
    bSynergyUpgrade=True, bApplyToSquad=True,
    PrereqA=da_WideBarrage, PrereqAStacks=1,
    PrereqB=da_ArcConductor, PrereqBStacks=1)
ok_da += 1
pool_das.append(da_Overwhelm)

# Synergy 4: IronVanguard — MaxHealth +50, MaxShield +25
#   PrereqA=Vitality, PrereqB=Overshield (both health-set attrs)
ge_IronVanguard = make_ge('GE_Synergy_IronVanguard', [
    make_mod(tmpl_health, 'MaxHealth', 'MaxHealth', 50.0, is_health_template=True),
    make_mod(tmpl_health, 'MaxHealth', 'MaxShield', 25.0, is_health_template=True),
])
ok_ge += 1
da_IronVanguard = make_da('DA_Synergy_IronVanguard', ge_IronVanguard,
    'Iron Vanguard', 2, '+50 Max HP, +25 Max Shield (squad)',
    'Shared training. Shared toughness.',
    bSynergyUpgrade=True, bApplyToSquad=True,
    PrereqA=da_Vitality, PrereqAStacks=1,
    PrereqB=da_Overshield, PrereqBStacks=1)
ok_da += 1
pool_das.append(da_IronVanguard)

unreal.log(f'L2C: created {ok_ge} GEs and {ok_da} DAs (2 utility + 4 milestone + 4 synergy)')

# -------------------------------------------------------------------------
# 10) Retune existing DAs (MaxStacks, prereqs) — do NOT touch GE blueprints
# -------------------------------------------------------------------------

retuned = 0

# PiercingRounds: MaxStacks=3
da_PiercingRounds.set_editor_property('MaxStacks', 3)
if not unreal.EditorAssetLibrary.save_asset(f'{DA_DIR}/DA_Upgrade_PiercingRounds', only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: save failed DA_Upgrade_PiercingRounds')
unreal.log('L2C: retuned DA_Upgrade_PiercingRounds MaxStacks=3')
retuned += 1

# ArcConductor: MaxStacks=3
da_ArcConductor.set_editor_property('MaxStacks', 3)
if not unreal.EditorAssetLibrary.save_asset(f'{DA_DIR}/DA_Upgrade_ArcConductor', only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: save failed DA_Upgrade_ArcConductor')
unreal.log('L2C: retuned DA_Upgrade_ArcConductor MaxStacks=3')
retuned += 1

# IncendiaryRounds: MaxStacks=2
da_IncendiaryRounds.set_editor_property('MaxStacks', 2)
if not unreal.EditorAssetLibrary.save_asset(f'{DA_DIR}/DA_Upgrade_IncendiaryRounds', only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: save failed DA_Upgrade_IncendiaryRounds')
unreal.log('L2C: retuned DA_Upgrade_IncendiaryRounds MaxStacks=2')
retuned += 1

# Neurotoxin: MaxStacks=2
da_Neurotoxin.set_editor_property('MaxStacks', 2)
if not unreal.EditorAssetLibrary.save_asset(f'{DA_DIR}/DA_Upgrade_Neurotoxin', only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: save failed DA_Upgrade_Neurotoxin')
unreal.log('L2C: retuned DA_Upgrade_Neurotoxin MaxStacks=2')
retuned += 1

# ChainDetonation: PrereqA=WideBarrage, PrereqB=Power, bApplyToSquad=True
# IMPORTANT: do NOT touch GE_Upgrade_ChainDetonation blueprint — DA only
da_ChainDetonation.set_editor_property('PrereqA', da_WideBarrage)
da_ChainDetonation.set_editor_property('PrereqAStacks', 1)
da_ChainDetonation.set_editor_property('PrereqB', da_Power)
da_ChainDetonation.set_editor_property('PrereqBStacks', 1)
da_ChainDetonation.set_editor_property('bApplyToSquad', True)
if not unreal.EditorAssetLibrary.save_asset(f'{DA_DIR}/DA_Upgrade_ChainDetonation', only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: save failed DA_Upgrade_ChainDetonation')
unreal.log('L2C: retuned DA_Upgrade_ChainDetonation prereqs+squad')
retuned += 1

unreal.log(f'L2C: retuned {retuned} existing DAs')

# -------------------------------------------------------------------------
# 11) Update BP_RaidGamemode: append to UpgradePool, set UtilityPool
# -------------------------------------------------------------------------

gm_bp = unreal.EditorAssetLibrary.load_asset(GAMEMODE_BP)
gm_class = unreal.load_object(None, GAMEMODE_BP + '.BP_RaidGamemode_C')
if gm_bp is None or gm_class is None:
    raise RuntimeError('L2C-FATAL: BP_RaidGamemode not found')

gm_cdo = unreal.get_default_object(gm_class)

# UpgradePool: append milestone + synergy DAs (not utility — those go to UtilityPool)
pool = list(gm_cdo.get_editor_property('UpgradePool'))
before = len(pool)
existing_names = {p.get_name() for p in pool if p is not None}
added_to_pool = 0
for da in pool_das:
    if da.get_name() not in existing_names:
        pool.append(da)
        existing_names.add(da.get_name())
        added_to_pool += 1
gm_cdo.set_editor_property('UpgradePool', pool)
unreal.log(f'L2C: UpgradePool {before} -> {len(pool)} (+{added_to_pool} new cards)')

# UtilityPool: set to the two utility DAs
gm_cdo.set_editor_property('UtilityPool', utility_das)
unreal.log(f'L2C: UtilityPool set to {[da.get_name() for da in utility_das]}')

# Compile + save BP
unreal.BlueprintEditorLibrary.compile_blueprint(gm_bp)
if not unreal.EditorAssetLibrary.save_asset(GAMEMODE_BP, only_if_is_dirty=False):
    raise RuntimeError('L2C-FATAL: BP_RaidGamemode save failed')
unreal.log(f'L2C: BP_RaidGamemode saved — pool={len(pool)} utility={len(utility_das)}')

# -------------------------------------------------------------------------
# 12) Final success marker
# -------------------------------------------------------------------------
unreal.log(f'L2C-DONE ge={ok_ge} da={ok_da} retuned={retuned} pool={len(pool)} utility={len(utility_das)}')
