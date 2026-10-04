"""Record retained/interrupted work without promoting any acceptance gate."""
import copy
import hashlib
import json
from pathlib import Path

base = Path(__file__).resolve().parent
out = base / 'readiness-matrix-v9.json'
if out.exists():
    raise RuntimeError('Preserve previous readiness version')
evidence = {}


def read(name):
    path = base / name
    raw = path.read_bytes()
    evidence[str(path)] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    return json.loads(raw)


def completed(name, command, cap):
    row = read(name)
    if (row['guard_version'] != 'c58-m0-r1' or row['command'] != command or
            row['status'] != 'completed' or type(row['returncode']) is not int or
            row['returncode'] != 0 or row['cap_hit'] is not False or row.get('stop_reason') or
            row['job_limit_gb'] != cap or row['thresholds']['test_override'] is not False or
            row['thresholds']['test_slot_dir'] is not False):
        raise RuntimeError('Successful exact original guard required')
    return row


prior = read('readiness-matrix-v8.json')
result = copy.deepcopy(prior)
if prior['checkpoint']['financial_batches_independently_checked'] != list(range(7)):
    raise RuntimeError('Unexpected historical checked frontier')
stopped = read('logs/t17-runtime-b0007-b0014-v1.receipt.json')
inspection = read('t17-interruption-inspection-v5.json')
if (evidence[str(base / 'logs/t17-runtime-b0007-b0014-v1.receipt.json')]['sha256'] !=
        '87bf8ab1a24f313da18b8bc008525fd125348675b4995616ed3adb2b0a41b4db' or
        evidence[str(base / 't17-interruption-inspection-v5.json')]['sha256'] !=
        '6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95' or
        stopped['status'] != 'stopped_low_commit' or stopped['returncode'] != 137 or
        inspection['retained_admission_batch_ids'] != list(range(8)) or
        inspection['final_manifest_exists'] or inspection['current_pointer_exists']):
    raise RuntimeError('Retained interruption differs from independent evidence')
window = read('logs/host-headroom-window-v8.json')
completed('logs/host-headroom-observe-v8.receipt.json', [
    '.venv/Scripts/python.exe', 'C:/atx/.superpowers/sdd/tier1-v4/host-headroom-observe-v8.py'], .1)
if (window['reschedule_condition_met'] is not True or len(window['samples']) != 31 or
        window['minimum_commit_free_gib'] < 3 or window['minimum_physical_free_gib'] < 1):
    raise RuntimeError('Original scheduling condition not met')
recovery = read('logs/t17-preserved-orphan-lock-v5.json')
completed('logs/t17-preserve-orphan-lock-v5.receipt.json', [
    'powershell.exe','-NoProfile','-File','C:/atx/.superpowers/sdd/tier1-v4/t17-preserve-orphan-lock-v5.ps1'], .3)
if (recovery['status'] != 'ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK' or
        recovery['sha256'] != '0ab4580b4fac8de5c6528dfbd63a64110bcd9742c19c80a5b6247ca8307af11a' or
        recovery['retained_admission_batch_ids'] != list(range(8))):
    raise RuntimeError('Wrong preserved stopped-owner lock')
refused = read('logs/t17-financial-native-validation-b0007-v1.receipt.json')
if (refused['status'] != 'refused_low_disk' or refused.get('child_pid') is not None or
        refused['command'] != ['.venv/Scripts/python.exe', 'C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-validate-v1.py', '--batch','7','--attempt-tag','v1'] or
        refused['disk']['min_free_gb'] != 40 or refused['disk']['free_gb'] >= 40 or
        refused['job_limit_gb'] != .8 or refused['heavy'] is not True or refused['cap_hit'] is not False):
    raise RuntimeError('Unexpected audit attempt outcome')
for name in ('t17-financial-native-validation-b0007-v1.json', 'logs/t17-financial-native-validation-b0007-v1.start.json'):
    if (base / name).exists():
        raise RuntimeError('Refused audit has unexpected child result/start')
result.update(schema='atx.tier1-v4.readiness/v9', version=9, client_date='2026-10-04')
result['checkpoint'].update(
    controller_commit='c54de7d5', financial_batches_admitted=8,
    financial_batches_independently_checked=list(range(7)),
    financial_additional_admissions_pending_current_replay=[7],
    actual_data_jobs='NONE_ROOT_AT_CHECKPOINT; AUDIT7_V1_REFUSED_LOW_DISK_BEFORE_CHILD',
    qualification='Eight retained native admissions were independently inspected after a host stop, but only0..6 have separate successful native audit evidence. The dead23988 lock was safely archived after a qualifying memory window. Audit7 did not start: free disk34.8474GiB was below the unchanged40GiB floor. No financial final publication/source-economic/gold/consumer gate passes.',
    engineering='Task1 P9 repair2 active,13PIT/source and4release fixture tests pass. Exception helper655be56e plus a70d298f independently approved after correcting repeated archive hashing; analogous private family/window/ratio transaction efficiencies remain in progress within Task17 R26repair2. Shared acyclic market helper active. Calendar publisher capture/clock leads remain unqualified; configured CPI warehouse absent does not establish global absence.',
)
record = {'artifact': '.superpowers/sdd/tier1-v4/t17-interruption-inspection-v5.json',
          'evidence_identities': copy.deepcopy(evidence), 'checkpoint_version': 9,
          'gate_promotion': False,
          'result': result['checkpoint']['qualification']}
for row in result['requirements']:
    if row['id'] in {'D01','D06','D07','C03','C04','O01','O03','O04'}:
        row.setdefault('observations', []).append(copy.deepcopy(record))
for old, new in zip(prior['requirements'], result['requirements'], strict=True):
    assert {k: v for k, v in old.items() if k != 'observations'} == {k: v for k, v in new.items() if k != 'observations'}
    assert new.get('observations', [])[:len(old.get('observations', []))] == old.get('observations', [])
assert len(result['requirements']) == 46
assert result['additional_required_readiness'] == prior['additional_required_readiness']
for name, pin in evidence.items():
    raw = Path(name).read_bytes()
    if {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()} != pin:
        raise RuntimeError('Metadata evidence changed during update')
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'old_contracts_preserved': 46, 'required_p9_appendix_unchanged': True,
                  'retained_financial_units': 8, 'independently_audited_units': 7,
                  'release_status': 'NOT_PRODUCTION_READY'}))
