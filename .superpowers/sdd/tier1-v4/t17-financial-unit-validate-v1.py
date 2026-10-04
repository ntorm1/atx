"""Root incremental audit: unchanged native typed replay of one retained unit.

This never creates a financial unit, publishes a generation, substitutes output
hashes for typed replay, or bypasses any final whole-generation validation.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time

from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External memory guard required')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--batch', type=int, choices=range(85), required=True)
parser.add_argument('--attempt-tag', required=True)
args = parser.parse_args()
if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,30}', args.attempt_tag):
    raise ValueError('Simple unique attempt tag required')
base = Path(__file__).resolve().parent
tag = f't17-financial-native-validation-b{args.batch:04d}-{args.attempt_tag}'
output = base / f'{tag}.json'
start_path = base / 'logs' / f'{tag}.start.json'
if output.exists() or start_path.exists():
    raise RuntimeError('Preserve previous audit attempt')
stage = Path('C:/atx/atx-db/data/alpha_panel/v1/fundamentals_release_v1')
generation = stage / 'generations/r17_20261004_002'
snapshot_path = generation / 'snapshot.json'
snapshot_pin = {'bytes': 10411360, 'sha256': 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
snapshot_sha = 'baf8f5cc238e9b2c38ab6583463fa0d23166928b645a348137eb5e43d0092f3d'
contract_path = Path('C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json')
contract_sha = '45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e'
runtime_sha = 'e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594'
extract_admission = base / 'task16-extract-admission-r19-v1.json'
extract_admission_sha = '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f'
admission_path = generation / f'units/batch-{args.batch:04d}/admission.json'
if not admission_path.is_file():
    raise RuntimeError('Existing native admission required; this audit never computes a new unit')


def unit_pins():
    """Freeze exact selected bytes; native validator remains the semantic authority."""
    pins = {str(admission_path): N.identity(admission_path)}
    row = N.read(admission_path)
    attempt = N._member(admission_path.parent, row['attempt'])
    for path in (attempt / 'start.json', attempt / 'native' / f'batch-{args.batch:04d}.json'):
        pins[str(path)] = N.identity(path)
    for path in N.payloads(attempt / 'native', args.batch).values():
        pins[str(path)] = N.identity(path)
    if N.identity(admission_path) != pins[str(admission_path)]:
        raise RuntimeError('Admission changed while inspecting unit')
    return row, pins


def current_frontier():
    allowed = {f'batch-{bid:04d}' for bid in N.REQUIRED}
    found = []
    for path in (generation / 'units').iterdir():
        if not path.is_dir() or path.is_symlink() or path.is_junction() or path.name not in allowed:
            raise RuntimeError('Unexpected unit path')
        if (path / 'admission.json').is_file():
            found.append(int(path.name[-4:]))
    return sorted(found)


def source_check(snapshot, con, runtime, proof_pin):
    # The independently admitted complete85 extraction inventory remains bound.
    # full=False is native current-source/code/preparation identity validation;
    # selected financial typed replay is separately mandatory below.
    N.recheck(snapshot, con, full=False, extract_admission=extract_admission,
              expected_admission_sha256=extract_admission_sha, force_full_replay=False)
    R._recheck_proof(stage, generation, runtime, snapshot_sha, proof_pin)
    if N.identity(snapshot_path) != snapshot_pin:
        raise RuntimeError('Independently pinned snapshot bytes changed')


started = time.monotonic()
runtime = R.load_runtime(contract_path, contract_sha, runtime_sha)
with R.connection(stage, runtime) as (con, resources), N.writer_lock(stage):
    if (stage / 'pointer-transaction.json').exists() or (stage / 'current.json').exists() or (generation / 'manifest.json').exists():
        raise RuntimeError('This audit is only for the current incomplete candidate')
    if N.identity(snapshot_path) != snapshot_pin:
        raise RuntimeError('Independently pinned snapshot changed')
    snapshot = N.read(snapshot_path)
    if N.digest(snapshot) != snapshot_sha:
        raise RuntimeError('Canonical snapshot differs')
    proof_pin = R.check_proof(stage, generation, runtime, snapshot_sha)
    before_row, before_pins = unit_pins()
    frontier = current_frontier()
    source_check(snapshot, con, runtime, proof_pin)
    audit_code_pin = N.identity(Path(__file__).resolve())
    start = {'status': 'ONE_UNIT_NATIVE_TYPED_REPLAY_STARTED', 'batch': args.batch,
             'snapshot': snapshot_pin, 'snapshot_sha256': snapshot_sha,
             'runtime_proof': proof_pin, 'resources': resources, 'unit_pins': before_pins,
             'frontier': frontier, 'controller_code': audit_code_pin}
    N._write_immutable(start_path, start)
    start_pin = N.identity(start_path)
    admitted = N.admit_unit(snapshot, generation, con, args.batch)
    if admitted is None or admitted != before_row:
        raise RuntimeError('Native typed replay did not validate the exact selected admission')
    source_check(snapshot, con, runtime, proof_pin)
    after_row, after_pins = unit_pins()
    if after_row != before_row or after_pins != before_pins or current_frontier() != frontier:
        raise RuntimeError('Candidate unit/frontier changed during replay')
    if N.identity(start_path) != start_pin or N.identity(Path(__file__).resolve()) != audit_code_pin:
        raise RuntimeError('Audit evidence/code changed during replay')
    result = {'status': 'ONE_RETAINED_FINANCIAL_UNIT_NATIVE_TYPED_REPLAY_VERIFIED',
              'batch': args.batch, 'admission': admitted, 'unit_pins': after_pins,
              'snapshot': snapshot_pin, 'snapshot_sha256': snapshot_sha,
              'runtime_proof': proof_pin, 'controller_code': audit_code_pin,
              'start': {'path': str(start_path), **start_pin}, 'resources': resources,
              'observed_frontier_not_newly_admitted': frontier,
              'successful_external_guard_required': True,
              'external_guard_receipt': str(base / 'logs' / f'{tag}.receipt.json'),
              'new_financial_units_created': 0, 'publication': False,
              'source_quality_certified': False, 'gate_promotion': False,
              'seconds': round(time.monotonic() - started, 3),
              'qualification': 'Valid only with the matching successful external guard receipt (completed/exit0/no cap or stop) and exact audit invocation. Only the named retained unit was rederived from the frozen originals and compared using unchanged native admit_unit typed/schema/counter validation. Its exact bytes and current source/code/preparation/runtime identities are bound before and after. Other frontier units receive no new semantic credit here. Complete85 publication, original financial/source-economic gates and final full-generation validation remain mandatory.'}
    N._write_immutable(output, result)
print(json.dumps({'status': result['status'], 'batch': args.batch, 'receipt': str(output),
                  'identity': N.identity(output), 'publication': False, 'gate_promotion': False}), flush=True)
