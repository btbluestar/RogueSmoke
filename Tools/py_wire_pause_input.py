# Wire the escape-menu input:
# 1. IA_Pause (duplicate of IA_Focus — bool action; factory-less, the proven headless path)
# 2. IMC_Default.default_key_mappings.mappings += Escape->IA_Pause, P->IA_Pause
#    (memory imc-key-mapping-authoring: the real rows live in default_key_mappings; REBUILD the
#     array, don't mutate; Key via import_text with the bare key name)
# 3. BP_RaidPlayerController CDO PauseAction = IA_Pause, then compile_blueprint + save
#    (memory mcp-bp-cdo-needs-compile).
import unreal

IA_SRC = '/Game/Input/Actions/IA_Focus'
IA_NEW = '/Game/Input/Actions/IA_Pause'
IMC = '/Game/Input/IMC_Default'
BP = '/Game/Blueprints/BP_RaidPlayerController'

# 1. IA_Pause
if not unreal.EditorAssetLibrary.does_asset_exist(IA_NEW):
    if not unreal.EditorAssetLibrary.duplicate_asset(IA_SRC, IA_NEW):
        raise RuntimeError('duplicate IA failed')
ia = unreal.EditorAssetLibrary.load_asset(IA_NEW)
unreal.EditorAssetLibrary.save_asset(IA_NEW)
unreal.log(f'DONE-IA {IA_NEW}')

# 2. IMC mappings (skip if already mapped)
imc = unreal.EditorAssetLibrary.load_asset(IMC)
dkm = imc.get_editor_property('default_key_mappings')
rows = list(dkm.get_editor_property('mappings'))
have = set()
for r in rows:
    a = r.get_editor_property('action')
    k = r.get_editor_property('key')
    have.add((a.get_name() if a else 'None', str(k)))

added = []
for key_name in ('Escape', 'P'):
    if ('IA_Pause', key_name) in have:
        continue
    row = unreal.EnhancedActionKeyMapping()
    row.set_editor_property('action', ia)
    key = unreal.Key()
    key.import_text(key_name)
    row.set_editor_property('key', key)
    rows.append(row)
    added.append(key_name)

dkm.set_editor_property('mappings', rows)
imc.set_editor_property('default_key_mappings', dkm)
if not unreal.EditorAssetLibrary.save_asset(IMC):
    raise RuntimeError('save IMC failed')
unreal.log(f'DONE-IMC added={added} total_rows={len(rows)}')

# 3. BP_RaidPlayerController.PauseAction
bp = unreal.EditorAssetLibrary.load_asset(BP)
cdo = unreal.get_default_object(bp.generated_class())
cdo.set_editor_property('PauseAction', ia)
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
if not unreal.EditorAssetLibrary.save_asset(BP):
    raise RuntimeError('save BP failed')
check = unreal.get_default_object(bp.generated_class()).get_editor_property('PauseAction')
unreal.log(f'DONE-BP PauseAction={check.get_name() if check else "None"}')
