"""Independently bind reviewed runtime to the previously admitted physical snapshot."""
import json
import os
from pathlib import Path
from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
workspace = Path(__file__).resolve().parent
out = workspace / 't17-runtime-admission-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous runtime admission')
snapshot_path = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1/generations/r17_20261004_002/snapshot.json')
extract_path = workspace / 'task16-extract-admission-r19-v1.json'
contract_path = Path('C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json')
expected_physical_snapshot = 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'
expected_extract = '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f'
expected_contract = '45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e'
expected_code = 'e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594'
pins = {str(p): N.identity(p) for p in (snapshot_path, extract_path, contract_path, Path(__file__).resolve())}
for path, expected in ((snapshot_path, expected_physical_snapshot), (extract_path, expected_extract), (contract_path, expected_contract)):
    if pins[str(path)]['sha256'] != expected:
        raise RuntimeError(f'Previously admitted physical bytes changed: {path.name}')
snapshot = N.read(snapshot_path)
N.require_snapshot(snapshot)
runtime = R.load_runtime(contract_path, expected_contract, expected_code)
canonical_snapshot = N.digest(snapshot)
if canonical_snapshot == expected_physical_snapshot:
    raise RuntimeError('Unexpected canonical/physical snapshot equality; inspect serialization')
for path, pin in pins.items():
    if N.identity(Path(path)) != pin:
        raise RuntimeError('Evidence changed during two-pass identity admission')
R.recheck_runtime(runtime)
result = {
    'status': 'RUNTIME_AND_CANONICAL_SNAPSHOT_IDENTITIES_VERIFIED',
    'generation': 'r17_20261004_002',
    'expected_snapshot_sha256': canonical_snapshot,
    'snapshot_physical_identity': pins[str(snapshot_path)],
    'expected_runtime_code_sha256': expected_code,
    'expected_contract_sha256': expected_contract,
    'expected_admission_sha256': expected_extract,
    'runtime': runtime,
    'evidence': pins,
    'qualification': 'Code/config and prior physical snapshot identity only. Native build must still freeze/recheck complete current sources, independently replay financial units and validate all original gates. No financial admission or production acceptance.'
}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'generation', 'expected_snapshot_sha256', 'snapshot_physical_identity', 'expected_runtime_code_sha256', 'qualification')}), flush=True)
