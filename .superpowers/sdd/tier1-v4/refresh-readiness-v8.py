"""Record completed per-unit audits without changing acceptance contracts."""
import ast
import copy
import hashlib
import json
from pathlib import Path

base = Path(__file__).resolve().parent
out = base / 'readiness-matrix-v8.json'
if out.exists():
    raise RuntimeError('Preserve previous readiness version')


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def identity(path):
    raw = path.read_bytes()
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def successful_guard(path, command):
    guard = read(path)
    if (guard['guard_version'] != 'c58-m0-r1' or guard['command'] != command or
            guard['receipt'] != str(path) or guard['status'] != 'completed' or
            type(guard['returncode']) is not int or guard['returncode'] != 0 or
            guard['cap_hit'] is not False or guard.get('stop_reason') or
            guard['job_limit_gb'] != 0.8 or guard['job_cap_bytes'] != 858992640 or
            guard['heavy'] is not True or guard['disk']['min_free_gb'] != 40 or
            guard['thresholds']['test_override'] is not False or
            guard['thresholds']['test_slot_dir'] is not False):
        raise RuntimeError('Exact successful guarded invocation required')
    return guard


prior = read(base / 'readiness-matrix-v7.json')
result = copy.deepcopy(prior)
if prior['checkpoint']['financial_batches_independently_checked'] != [0, 1, 2]:
    raise RuntimeError('Unexpected prior checked frontier')
build_receipt = base / 'logs/t17-runtime-b0003-b0006-v1.receipt.json'
build_stdout = base / 'logs/t17-runtime-b0003-b0006-v1.out.log'
build_command = [
    '.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.fund_release_runtime', 'build',
    '--execution-contract', 'C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json',
    '--expected-contract-sha256', '45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e',
    '--expected-runtime-code-sha256', 'e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594',
    '--expected-snapshot-sha256', 'baf8f5cc238e9b2c38ab6583463fa0d23166928b645a348137eb5e43d0092f3d',
    '--bindings', 'C:/atx/.superpowers/sdd/tier1-v4/task16-fund-reader-binding-v1.json',
    '--generation', 'r17_20261004_002', '--max-new-batches', '4',
    '--extract-admission', 'C:/atx/.superpowers/sdd/tier1-v4/task16-extract-admission-r19-v1.json',
    '--expected-admission-sha256', '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f',
]
build = successful_guard(build_receipt, build_command)
if ast.literal_eval(build_stdout.read_text(encoding='utf-8').strip()) != {
    'status': 'partial_cache_only', 'published': False, 'missing_batches': list(range(7, 85))
}:
    raise RuntimeError('Unexpected retained frontier from completed build')

controller = base / 't17-financial-unit-validate-v1.py'
controller_pin = identity(controller)
evidence = {str(path): identity(path) for path in (build_receipt, build_stdout, controller)}
audits = []
for bid in range(3, 7):
    tag = f't17-financial-native-validation-b{bid:04d}-v1'
    path = base / f'{tag}.json'
    guard_path = base / 'logs' / f'{tag}.receipt.json'
    start_path = base / 'logs' / f'{tag}.start.json'
    audit = read(path)
    guard = successful_guard(guard_path, [
        '.venv/Scripts/python.exe', 'C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-validate-v1.py',
        '--batch', str(bid), '--attempt-tag', 'v1'])
    start = read(start_path)
    if (audit['status'] != 'ONE_RETAINED_FINANCIAL_UNIT_NATIVE_TYPED_REPLAY_VERIFIED' or
            type(audit['batch']) is not int or audit['batch'] != bid or
            audit['successful_external_guard_required'] is not True or
            audit['external_guard_receipt'] != str(guard_path) or
            type(audit['new_financial_units_created']) is not int or
            audit['new_financial_units_created'] != 0 or
            any(audit[key] is not False for key in ('publication', 'source_quality_certified', 'gate_promotion')) or
            audit['controller_code'] != controller_pin or
            audit['start'] != {'path': str(start_path), **identity(start_path)} or
            start['controller_code'] != controller_pin or start['batch'] != bid or
            start['unit_pins'] != audit['unit_pins'] or
            start['frontier'] != list(range(7)) or
            audit['observed_frontier_not_newly_admitted'] != list(range(7))):
        raise RuntimeError('Unbound or promoting per-unit audit')
    for item in (path, guard_path, start_path):
        evidence[str(item)] = identity(item)
    audits.append({'batch': bid, 'result': str(path), 'guard': str(guard_path),
                   'seconds': guard['run_seconds'], 'peak_bytes': guard['native_peak_job_memory_bytes']})

result.update(schema='atx.tier1-v4.readiness/v8', version=8, client_date='2026-10-04')
result['checkpoint'].update(
    controller_commit='b3f7c503',
    qualification='Seven retained financial units independently checked through the recorded native replay and root evidence chain. This metadata update does not rerun source checks or qualify financial economics, a final publication, gold panels or consumer release.',
    financial_batches_admitted=7,
    financial_batches_independently_checked=list(range(7)),
    financial_additional_admissions_pending_current_replay=[],
    actual_data_jobs='FOUR_PER_UNIT_AUDITS_COMPLETED; NEXT_CONTINUATION_NOT_STARTED_AT_THIS_CHECKPOINT',
    engineering='Task1 P9 indexes repair2 active with exact legacy leaks reproduced. Task17 R26 repair2 active. Short d0d92665, notes1607b771 and family75efb231 await independent combined review; ownership exception helper final checks active.',
)
record = {
    'artifact': '.superpowers/sdd/tier1-v4/review-financial-unit-validation-v1.md',
    'supporting_artifacts': [str(build_receipt), '.superpowers/sdd/tier1-v4/t17-financial-unit-wrapper-portability-v1.md', *[a['result'] for a in audits], *[a['guard'] for a in audits]],
    'evidence_identities': evidence, 'unit_measurements': audits,
    'result': f'Native four-new-unit continuation completed{build["run_seconds"]}s/{build["native_peak_job_memory_bytes"]}peakbytes. Each retained3..6 was then independently source-rederived by unchanged admit_unit with full typed/schema/population/counter comparison and source/runtime/code/unit identity checks before/after. Separate immutable results bind exact successful external guard commands. Together with prior v7 evidence for0..2, seven of85 units checked. No complete financial publication or source-economic/release gate passes.',
    'checkpoint_version': 8, 'gate_promotion': False,
}
for row in result['requirements']:
    if row['id'] in {'D01', 'D06', 'D07', 'C03', 'C04', 'O01', 'O03', 'O04'}:
        row.setdefault('observations', []).append(copy.deepcopy(record))
for old, new in zip(prior['requirements'], result['requirements'], strict=True):
    assert {k: v for k, v in old.items() if k != 'observations'} == {k: v for k, v in new.items() if k != 'observations'}
    assert new.get('observations', [])[:len(old.get('observations', []))] == old.get('observations', [])
assert len(result['requirements']) == 46
assert result['additional_required_readiness'] == prior['additional_required_readiness']
for path, pin in evidence.items():
    if identity(Path(path)) != pin:
        raise RuntimeError('Evidence changed during metadata update')
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({'old_contracts_preserved': 46, 'required_p9_appendix_unchanged': True,
                  'independently_checked_financial_units': 7, 'release_status': 'NOT_PRODUCTION_READY'}))
