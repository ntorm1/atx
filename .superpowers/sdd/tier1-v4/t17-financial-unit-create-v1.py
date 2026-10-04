"""Create one missing unit through unchanged native double-replay admission.

Earlier units are byte-bound and left untouched. This wrapper does not replay or
newly qualify them, merge, publish, or replace final original whole85 validation.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time
from atx_db.alpha_panel import fund_batch_release as N, fund_release_runtime as R

if not os.environ.get('ATX_GUARD_JOB'):
    raise RuntimeError('External guard required')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--batch', type=int, choices=range(85), required=True)
parser.add_argument('--attempt-tag', required=True)
args = parser.parse_args()
if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,30}', args.attempt_tag):
    raise ValueError('Simple unique attempt tag required')
base = Path(__file__).resolve().parent
tag = f't17-financial-native-create-b{args.batch:04d}-{args.attempt_tag}'
output = base / f'{tag}.json'
start_path = base / 'logs' / f'{tag}.start.json'
if output.exists() or start_path.exists():
    raise RuntimeError('Preserve previous controller attempt')
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


def no_publication():
    if any(path.exists() for path in (stage / 'pointer-transaction.json', stage / 'current.json', generation / 'manifest.json')):
        raise RuntimeError('Only the current incomplete candidate is supported')


def frontier():
    allowed = {f'batch-{bid:04d}' for bid in N.REQUIRED}
    found = []
    for path in (generation / 'units').iterdir():
        if not path.is_dir() or path.is_symlink() or path.is_junction() or path.name not in allowed:
            raise RuntimeError('Unexpected native unit directory')
        if (path / 'admission.json').is_file():
            found.append(int(path.name[-4:]))
    return sorted(found)


def unit_record(bid):
    admission_path = generation / f'units/batch-{bid:04d}/admission.json'
    row = N.read(admission_path)
    if (row.get('version') != N.VERSION or type(row.get('batch')) is not int or
            row['batch'] != bid or row.get('snapshot_sha256') != snapshot_sha):
        raise RuntimeError('Stale native admission metadata')
    pins = {str(admission_path): N.identity(admission_path)}
    attempt = N._member(admission_path.parent, row['attempt'])
    for path in (attempt / 'start.json', attempt / 'native' / f'batch-{bid:04d}.json',
                 *N.payloads(attempt / 'native', bid).values()):
        pins[str(path)] = N.identity(path)
    if N.identity(admission_path) != pins[str(admission_path)]:
        raise RuntimeError('Admission changed during byte binding')
    return {'admission': row, 'pins': pins}


def recheck(snapshot, con, runtime, proof_pin):
    N.recheck(snapshot, con, full=False, extract_admission=extract_admission,
              expected_admission_sha256=extract_admission_sha, force_full_replay=False)
    R._recheck_proof(stage, generation, runtime, snapshot_sha, proof_pin)
    if N.identity(snapshot_path) != snapshot_pin:
        raise RuntimeError('Exact independently pinned snapshot changed')
    no_publication()


started = time.monotonic()
runtime = R.load_runtime(contract_path, contract_sha, runtime_sha)
with R.connection(stage, runtime) as (con, resources), N.writer_lock(stage):
    no_publication()
    if N.identity(snapshot_path) != snapshot_pin:
        raise RuntimeError('Exact independently pinned snapshot changed')
    snapshot = N.read(snapshot_path)
    N.require_snapshot(snapshot)
    if N.digest(snapshot) != snapshot_sha:
        raise RuntimeError('Canonical native snapshot differs')
    proof_pin = R.check_proof(stage, generation, runtime, snapshot_sha)
    prior_frontier = frontier()
    if prior_frontier != list(range(args.batch)):
        raise RuntimeError('Only the next missing fixed native unit may be created')
    prior = {str(bid): unit_record(bid) for bid in prior_frontier}
    recheck(snapshot, con, runtime, proof_pin)
    controller_pin = N.identity(Path(__file__).resolve())
    start = {'status': 'ONE_MISSING_NATIVE_UNIT_STARTED', 'batch': args.batch,
             'snapshot': snapshot_pin, 'snapshot_sha256': snapshot_sha,
             'runtime_proof': proof_pin, 'resources': resources,
             'prior_frontier': prior_frontier, 'prior_units': prior,
             'controller_code': controller_pin}
    N._write_immutable(start_path, start)
    start_pin = N.identity(start_path)
    # Do not replace, intercept or abbreviate any of compute_new's own checks.
    # It independently computes twice, compares all typed cells and failure
    # counters, rechecks sources three times, then atomically seals admission.
    admitted = N.compute_new(snapshot, generation, con, args.batch)
    recheck(snapshot, con, runtime, proof_pin)
    created = unit_record(args.batch)
    if created['admission'] != admitted:
        raise RuntimeError('Created unit does not match exact native admission')
    if frontier() != list(range(args.batch + 1)):
        raise RuntimeError('Unexpected resulting native admission frontier')
    if {str(bid): unit_record(bid) for bid in prior_frontier} != prior:
        raise RuntimeError('Existing admitted unit bytes changed')
    if N.identity(start_path) != start_pin or N.identity(Path(__file__).resolve()) != controller_pin:
        raise RuntimeError('Controller/start changed during creation')
    result = {'status': 'ONE_NATIVE_FINANCIAL_UNIT_DOUBLE_REPLAY_ADMITTED', 'batch': args.batch,
              'unit': created, 'prior_units_unchanged': prior,
              'snapshot': snapshot_pin, 'snapshot_sha256': snapshot_sha,
              'runtime_proof': proof_pin, 'resources': resources,
              'controller_code': controller_pin, 'start': {'path': str(start_path), **start_pin},
              'observed_frontier': list(range(args.batch + 1)),
              'prior_units_replayed_or_newly_qualified': False,
              'successful_external_guard_required': True,
              'external_guard_receipt': str(base / 'logs' / f'{tag}.receipt.json'),
              'new_financial_units_created': 1, 'publication': False,
              'source_quality_certified': False, 'gate_promotion': False,
              'seconds': round(time.monotonic() - started, 3),
              'qualification': 'Valid only with the matching successful external guard receipt (completed/exit0/no cap or stop), exact controller command/code/start/unit identities, and final runtime context success. The named unit alone was created by unchanged native compute_new including its full two source derivations, typed/schema/population/receipt-counter comparison and original source rechecks. Earlier units were left byte-identical, not rederived or newly qualified here. No merge/publication/full-generation admission. Original R23 build must still validate every unit and all whole85 merge/evidence/publication requirements.'}
    N._write_immutable(output, result)
print(json.dumps({'status': result['status'], 'batch': args.batch, 'receipt': str(output),
                  'identity': N.identity(output), 'publication': False, 'gate_promotion': False}), flush=True)
