"""Inspect three retained native financial units after successful replay-only validation."""
import ast
import json
import os
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 't17-financial-progress-audit-v4.json'
if out.exists():
    raise RuntimeError('Preserve previous progress evidence')
bridge_path = workspace / 't17-runtime-admission-v1.json'
bridge = N.read(bridge_path)
guard_path = workspace / 'logs/t17-runtime-replay-b0000-b0002-v1.receipt.json'
stdout_path = workspace / 'logs/t17-runtime-replay-b0000-b0002-v1.out.log'
guard_pin = N.identity(guard_path)
stdout_pin = N.identity(stdout_path)
guard = N.read(guard_path)
command = guard['command']
if guard['status'] != 'completed' or guard['returncode'] != 0 or guard['cap_hit']:
    raise RuntimeError('Successful bounded native execution not proven')
if command[:4] != ['.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.fund_release_runtime', 'build']:
    raise RuntimeError('Unexpected native runtime entrypoint')
options = dict(zip(command[4::2], command[5::2], strict=True))
expected_options = {
    '--execution-contract': 'C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json',
    '--expected-contract-sha256': '45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e',
    '--expected-runtime-code-sha256': 'e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594',
    '--expected-snapshot-sha256': 'baf8f5cc238e9b2c38ab6583463fa0d23166928b645a348137eb5e43d0092f3d',
    '--bindings': 'C:/atx/.superpowers/sdd/tier1-v4/task16-fund-reader-binding-v1.json',
    '--generation': 'r17_20261004_002', '--max-new-batches': '0',
    '--extract-admission': 'C:/atx/.superpowers/sdd/tier1-v4/task16-extract-admission-r19-v1.json',
    '--expected-admission-sha256': '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f',
}
if options != expected_options or len(command) != 4 + 2 * len(expected_options):
    raise RuntimeError('Unexpected or repeated command arguments')
if ast.literal_eval(stdout_path.read_text(encoding='utf-8').strip()) != {
    'status': 'partial_cache_only', 'published': False, 'missing_batches': list(range(3, 85))
}:
    raise RuntimeError('Unexpected bounded native result')
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
snapshot_path = generation / 'snapshot.json'
snapshot = N.read(snapshot_path)
expected_physical = {'sha256': 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c', 'bytes': 10411360}
if N.identity(snapshot_path) != expected_physical or bridge['snapshot_physical_identity'] != expected_physical or N.digest(snapshot) != options['--expected-snapshot-sha256']:
    raise RuntimeError('Independently admitted snapshot identity changed')
runtime = R.load_runtime(Path(options['--execution-contract']), options['--expected-contract-sha256'], options['--expected-runtime-code-sha256'])
proof_path = R.proof_path(stage, generation)
proof_pin = R.check_proof(stage, generation, runtime, options['--expected-snapshot-sha256'])
pins = {str(p): N.identity(p) for p in (bridge_path, snapshot_path, proof_path, Path(__file__).resolve())}
pins[str(guard_path)] = guard_pin
pins[str(stdout_path)] = stdout_pin
inspection_path = workspace / 't17-interruption-inspection-v4.json'
inspection_pin = {'bytes': 15890, 'sha256': '945ca1845869df47904a1ed24bd4371bac078a155d23c7fd2817398c46759746'}
if N.identity(inspection_path) != inspection_pin:
    raise RuntimeError('Independently pinned pre-replay unit inspection changed')
inspection = N.read(inspection_path)
if inspection['status'] != 'HOST_STOPPED_FINANCIAL_CONTINUATION_INSPECTED' or inspection['retained_admission_batch_ids'] != [0, 1, 2]:
    raise RuntimeError('Unexpected independently inspected pre-replay frontier')
pins[str(inspection_path)] = inspection_pin
recovery_path = workspace / 'logs/t17-preserved-orphan-lock-v4.json'
recovery_guard_path = workspace / 'logs/t17-preserve-orphan-lock-v4.receipt.json'
for p in (recovery_path, recovery_guard_path):
    pins[str(p)] = N.identity(p)
recovery = N.read(recovery_path)
recovery_guard = N.read(recovery_guard_path)
if (recovery['status'] != 'ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK' or
        recovery['candidate_inspection_sha256'] != inspection_pin['sha256'] or
        recovery['guard_receipt_sha256'] != '20f5006c5ab8b1051e455756fe27662095e951c8734277d8d703ba7dfa9fb110' or
        recovery['bytes'] != 5 or recovery['sha256'] != '5173a948f060ec7c42e5588c5ea03fffbf71d105dae50f2415e8cfc7e3c9dab7' or
        recovery['retained_admission_batch_ids'] != [0, 1, 2] or
        recovery['source'] != str(stage / '.writer.lock') or
        recovery['destination'] != str(stage / '_interrupted_locks/r17_20261004_002-pid10544-runtime-continuation-v1-lowcommit.lock')):
    raise RuntimeError('Recovery does not bind the exact independently inspected pre-replay units')
if (recovery_guard['status'] != 'completed' or recovery_guard['returncode'] != 0 or recovery_guard['cap_hit'] or
        recovery_guard['command'] != ['powershell.exe', '-NoProfile', '-File', 'C:/atx/.superpowers/sdd/tier1-v4/t17-preserve-orphan-lock-v4.ps1']):
    raise RuntimeError('Successful exact dead-owner preservation not proven')

def check_frontier():
    found = []
    for unit in (generation / 'units').iterdir():
        if not unit.is_dir() or unit.is_symlink() or unit.is_junction():
            raise RuntimeError('Unexpected native unit path')
        if unit.name not in {f'batch-{bid:04d}' for bid in range(85)}:
            raise RuntimeError('Unexpected native unit name')
        if (unit / 'admission.json').exists():
            found.append(int(unit.name[-4:]))
    if sorted(found) != [0, 1, 2]:
        raise RuntimeError('Current complete native admission frontier differs')

check_frontier()

def inspected_identity(path):
    pin = inspection['native_members'].get(str(path))
    if pin is None or N.identity(path) != pin:
        raise RuntimeError('Unit bytes differ from independently pinned pre-replay inspection')
    pins[str(path)] = pin
    return pin

prior_path = workspace / 't17-financial-progress-audit-v2.json'
prior_pin = {'bytes': 7402, 'sha256': '9cef7b23958163d6ad2e23167929625687c86c6d544ac5dac7ea3065a3444322'}
if N.identity(prior_path) != prior_pin:
    raise RuntimeError('Prior independently checked first-unit audit changed')
prior_audit = N.read(prior_path)
if prior_audit['status'] != 'ONE_NATIVE_FINANCIAL_UNIT_IDENTITIES_VERIFIED' or prior_audit['batch'] != 0:
    raise RuntimeError('Prior independently checked first unit required')
pins[str(prior_path)] = prior_pin
unit_records = []
for bid in range(3):
    path = generation / f'units/batch-{bid:04d}/admission.json'
    inspected_identity(path)
    row = N.read(path)
    if row != inspection['admissions'][str(bid)]:
        raise RuntimeError('Retained admission metadata differs from pre-replay inspection')
    if set(row) != {'version', 'batch', 'snapshot_sha256', 'attempt', 'start_identity', 'files', 'native_receipt'} or row['version'] != N.VERSION or type(row['batch']) is not int or row['batch'] != bid or row['snapshot_sha256'] != N.digest(snapshot):
        raise RuntimeError('Unsupported financial admission')
    attempt = N._member(path.parent, row['attempt'])
    start_path = attempt / 'start.json'
    inspected_identity(start_path)
    start = N.read(start_path)
    if N.identity(start_path) != row['start_identity'] or start['snapshot_sha256'] != N.digest(snapshot) or start['batch'] != bid or start['attempt'] != attempt.name:
        raise RuntimeError('Native attempt provenance mismatch')
    native = attempt / 'native'
    native_receipt = native / f'batch-{bid:04d}.json'
    inspected_identity(native_receipt)
    if N.identity(native_receipt) != row['native_receipt'] or set(row['files']) != set(N.TABLES):
        raise RuntimeError('Native receipt/table inventory mismatch')
    if {p.name for p in native.iterdir()} != {p.name for p in N.payloads(native, bid).values()} | {f'batch-{bid:04d}.json'}:
        raise RuntimeError('Unsupported native member inventory')
    for name, table in row['files'].items():
        payload = N._member(native, table['file'])
        if N.identity(payload) != {key: table[key] for key in ('sha256', 'bytes')} or pq.ParquetFile(payload).metadata.num_rows != table['rows']:
            raise RuntimeError('Native payload identity/footer mismatch')
        inspected_identity(payload)
    for p in (path, start_path, native_receipt):
        inspected_identity(p)
    if bid == 0 and row['files'] != prior_audit['files']:
        raise RuntimeError('Previously checked first unit changed')
    unit_records.append({'batch': bid, 'files': row['files'], 'native_receipt_semantics': N.receipt_semantics(native_receipt), 'attempt': str(attempt)})
with R.connection(stage, runtime) as (con, resources):
    N.recheck(snapshot, con, full=False, extract_admission=Path(options['--extract-admission']), expected_admission_sha256=options['--expected-admission-sha256'], force_full_replay=False)
    R._recheck_proof(stage, generation, runtime, options['--expected-snapshot-sha256'], proof_pin)
for path, pin in pins.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Audit evidence changed during source checks')
check_frontier()
if (generation / 'manifest.json').exists() or (stage / 'current.json').exists():
    raise RuntimeError('Unexpected complete financial publication')
result = {
    'status': 'NATIVE_FINANCIAL_UNITS_IDENTITIES_VERIFIED', 'batches': list(range(3)),
    'units': unit_records, 'missing_batches': list(range(3, 85)), 'evidence': pins,
    'native_seconds': guard['run_seconds'], 'native_peak_bytes': guard['native_peak_job_memory_bytes'],
    'qualification': 'Successful replay-only native build independently rederived and typed-compared all retained batches0-2 through the unchanged admission protocol. This audit binds each unit/admission/start/native-receipt/payload to the exact independently pinned pre-replay inspection and current post-replay identities; no new unit was created or additional unit counted. Root rechecked current frozen sources through the unchanged admitted-extraction protocol plus runtime/snapshot/receipt/output/footer identities twice. This audit does not itself duplicate native typed replay. Original financial/source-economic gates and all85 final publication remain open.'
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'batches', 'missing_batches', 'native_seconds', 'native_peak_bytes', 'qualification')}), flush=True)
