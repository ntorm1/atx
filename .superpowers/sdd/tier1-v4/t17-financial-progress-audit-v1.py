"""Inspect the first successful native financial unit and current frozen sources."""
import ast
import json
import os
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 't17-financial-progress-audit-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous progress evidence')
bridge_path = workspace / 't17-runtime-admission-v1.json'
bridge = N.read(bridge_path)
guard_path = workspace / 'logs/t17-runtime-pilot-b0000-v2.receipt.json'
stdout_path = workspace / 'logs/t17-runtime-pilot-b0000-v2.out.log'
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
    '--generation': 'r17_20261004_002', '--max-new-batches': '1',
    '--extract-admission': 'C:/atx/.superpowers/sdd/tier1-v4/task16-extract-admission-r19-v1.json',
    '--expected-admission-sha256': '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f',
}
if options != expected_options or len(command) != 4 + 2 * len(expected_options):
    raise RuntimeError('Unexpected or repeated command arguments')
if ast.literal_eval(stdout_path.read_text(encoding='utf-8').strip()) != {
    'status': 'partial_cache_only', 'published': False, 'missing_batches': list(range(1, 85))
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
pins = {str(p): N.identity(p) for p in (bridge_path, guard_path, stdout_path, snapshot_path, proof_path, Path(__file__).resolve())}
path = generation / 'units/batch-0000/admission.json'
row = N.read(path)
if set(row) != {'version', 'batch', 'snapshot_sha256', 'attempt', 'start_identity', 'files', 'native_receipt'} or row['version'] != N.VERSION or type(row['batch']) is not int or row['batch'] != 0 or row['snapshot_sha256'] != N.digest(snapshot):
    raise RuntimeError('Unsupported financial admission')
attempt = N._member(path.parent, row['attempt'])
start_path = attempt / 'start.json'
start = N.read(start_path)
if N.identity(start_path) != row['start_identity'] or start['snapshot_sha256'] != N.digest(snapshot) or start['batch'] != 0 or start['attempt'] != attempt.name:
    raise RuntimeError('Native attempt provenance mismatch')
native = attempt / 'native'
native_receipt = native / 'batch-0000.json'
if N.identity(native_receipt) != row['native_receipt'] or set(row['files']) != set(N.TABLES):
    raise RuntimeError('Native receipt/table inventory mismatch')
if {p.name for p in native.iterdir()} != {p.name for p in N.payloads(native, 0).values()} | {'batch-0000.json'}:
    raise RuntimeError('Unsupported native member inventory')
for name, table in row['files'].items():
    payload = N._member(native, table['file'])
    if N.identity(payload) != {key: table[key] for key in ('sha256', 'bytes')} or pq.ParquetFile(payload).metadata.num_rows != table['rows']:
        raise RuntimeError('Native payload identity/footer mismatch')
    pins[str(payload)] = N.identity(payload)
for p in (path, start_path, native_receipt):
    pins[str(p)] = N.identity(p)
with R.connection(stage, runtime) as (con, resources):
    N.recheck(snapshot, con, full=False, extract_admission=Path(options['--extract-admission']), expected_admission_sha256=options['--expected-admission-sha256'], force_full_replay=False)
    R._recheck_proof(stage, generation, runtime, options['--expected-snapshot-sha256'], proof_pin)
for path, pin in pins.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Audit evidence changed during source checks')
if (generation / 'manifest.json').exists() or (stage / 'current.json').exists():
    raise RuntimeError('Unexpected complete financial publication')
result = {
    'status': 'ONE_NATIVE_FINANCIAL_UNIT_IDENTITIES_VERIFIED', 'batch': 0,
    'files': row['files'], 'native_receipt_semantics': N.receipt_semantics(native_receipt),
    'attempt': str(attempt), 'missing_batches': list(range(1, 85)), 'evidence': pins,
    'native_seconds': guard['run_seconds'], 'native_peak_bytes': guard['native_peak_job_memory_bytes'],
    'qualification': 'Successful native producer performed independent typed financial replay before admission. Root rechecked complete current frozen sources plus runtime/snapshot/receipt/output/footer identities twice. Original financial/source-economic gates and all85 final publication remain open.'
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'batch', 'files', 'missing_batches', 'native_seconds', 'native_peak_bytes', 'qualification')}), flush=True)
