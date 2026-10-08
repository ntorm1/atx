"""Separately admitted R23 runtime; the frozen native release protocol is retained.

This adapter grants no financial admission from the successful resource diagnostic.
Caller pins are mandatory for every build, consumer validation and pointer action.
"""
from __future__ import annotations

import argparse
import contextlib
from pathlib import Path
import uuid

from . import fund_batch_release as N
from . import privatefund_runtime_resources as R

VERSION = 'fund-release-runtime-r23-v1'
CONTRACT = {
    'version': VERSION, 'native_version': N.VERSION,
    'semantic_producer': N.SEMANTIC_STAGE, 'required_batches': list(N.REQUIRED),
    'preparation_units': N.PREPARATION_UNITS,
    'duckdb': {'memory': '256MB', 'threads': 1, 'file_backed': True, 'timezone': 'UTC'},
    'guard': {'maximum_bytes': R.GIB, 'initial_actual_gib': 0.8},
    'minimum_free_disk_bytes': 40 * R.GIB,
    'native_protocol': 'complete frozen fund-batch-release-v1; no omitted replay or gates',
    'runtime_proof': 'outside exact native generation inventory; caller-pinned code and contract',
    'readiness': 'NOT_PRODUCTION_READY', 'source_quality_certified': False,
}


def contract_path():
    return Path(__file__).parents[3]/'docs/datasets/fund_release_runtime.execution.json'


def code_inventory():
    return {str(p.resolve()): N.identity(p) for p in (Path(__file__), Path(R.__file__))}


def load_runtime(contract, expected_contract_sha256, expected_runtime_code_sha256):
    contract = N._absolute(contract)
    if not N._sha(expected_contract_sha256) or not N._sha(expected_runtime_code_sha256):
        raise ValueError('independent caller runtime contract and code SHA256 pins required')
    pin = N.identity(contract)
    if pin['sha256'] != expected_contract_sha256 or N.read(contract) != CONTRACT:
        raise ValueError('missing/stale/unsupported execution contract')
    code = code_inventory()
    if N.digest(code) != expected_runtime_code_sha256:
        raise ValueError('actual runtime code identity mismatch')
    result = {'version': VERSION, 'contract_path': str(contract), 'contract_identity': pin,
              'contract': CONTRACT, 'code': code}
    recheck_runtime(result)
    return result


def recheck_runtime(runtime):
    if N.identity(runtime['contract_path']) != runtime['contract_identity'] or N.read(runtime['contract_path']) != CONTRACT:
        raise ValueError('execution contract changed during work')
    if code_inventory() != runtime['code']:
        raise ValueError('runtime code changed during work')


def proof_path(stage, generation):
    return Path(stage)/'runtime-provenance'/f'{generation.name}.json'


def check_proof(stage, generation, runtime, expected_snapshot_sha256, create=False):
    if not N._sha(expected_snapshot_sha256):
        raise ValueError('independent caller expected native snapshot SHA256 required')
    binding = N.read(generation/'snapshot.json')['binding'] if (generation/'snapshot.json').exists() else None
    if binding:
        for folder in (binding['extract_root'], binding['prep_root'], str(stage)):
            if Path(runtime['contract_path']).is_relative_to(Path(folder).resolve()):
                raise ValueError('execution contract must be outside mutable native directories')
    path = proof_path(stage, generation)
    expected = {'runtime': runtime, 'generation': generation.name,
                'native_snapshot_sha256': expected_snapshot_sha256}
    if path.exists():
        if N.read(path) != expected:
            raise ValueError('stale generation runtime provenance')
    elif create:
        N._write_immutable(path, expected)
    else:
        raise ValueError('missing caller-admitted runtime provenance')
    if (generation/'snapshot.json').exists() and N.digest(N.read(generation/'snapshot.json')) != expected_snapshot_sha256:
        raise ValueError('native snapshot differs from independent caller pin')
    return N.identity(path)


@contextlib.contextmanager
def connection(stage, runtime):
    recheck_runtime(runtime)
    guard = R.guard_identity()
    R.check_disk(Path(stage).parent)
    con = R.connect(stage)
    try:
        actual = con.execute("SELECT current_setting('memory_limit'), current_setting('threads'), current_setting('TimeZone')").fetchone()
        # DuckDB displays the binary conversion of its decimal 256MB allowance.
        if con.execute("SELECT current_setting('memory_limit')::VARCHAR").fetchone()[0] != '244.1 MiB' or actual[1:] != (1, 'UTC'):
            raise ValueError('actual runtime DuckDB settings differ from execution contract')
        databases = con.execute('PRAGMA database_list').fetchall()
        if not any(row[2] and Path(row[2]).is_file() for row in databases):
            raise ValueError('runtime DuckDB connection must be file-backed')
        yield con, {'guard': guard, 'duckdb_actual': list(actual),
                    'database': str(next(row[2] for row in databases if row[2]))}
    finally:
        con.close()
        recheck_runtime(runtime)
        R.guard_identity()
        R.check_disk(Path(stage).parent)


def build(binding_path, generation_id, runtime, expected_snapshot_sha256, selected=N.REQUIRED,
          max_new_batches=None, extract_admission=None, expected_admission_sha256=None, force_full_replay=False):
    if any(type(i) is not int or i not in N.REQUIRED for i in selected) or max_new_batches is not None and (type(max_new_batches) is not int or max_new_batches < 0):
        raise ValueError('invalid bounded work selection')
    binding_path = N._absolute(binding_path)
    binding = N.read(binding_path); stage = Path(binding['root'])/N.STAGE
    for folder in (binding['extract_root'], binding['prep_root'], str(stage)):
        if Path(runtime['contract_path']).is_relative_to(Path(folder).resolve()):
            raise ValueError('execution contract must be outside mutable native directories')
    if not N._sha(expected_snapshot_sha256):
        raise ValueError('independent caller expected native snapshot SHA256 required')
    with connection(stage, runtime) as (con, resources), N.writer_lock(stage):
        if (stage/'pointer-transaction.json').exists():
            raise ValueError('unfinished pointer transaction; recover first')
        generation = N._generation(stage, generation_id)
        if (generation/'manifest.json').exists():
            raise ValueError('immutable completed generation')
        snapshot = N.freeze(binding_path, con, extract_admission=extract_admission,
                            expected_admission_sha256=expected_admission_sha256, force_full_replay=force_full_replay)
        if N.digest(snapshot) != expected_snapshot_sha256:
            raise ValueError('native snapshot differs from independent caller pin')
        if (generation/'snapshot.json').exists():
            if N.read(generation/'snapshot.json') != snapshot:
                raise ValueError('stale generation')
        elif generation.exists() and any(generation.iterdir()):
            raise ValueError('unbound candidate directory')
        proof_pin = check_proof(stage, generation, runtime, expected_snapshot_sha256, create=True)
        if not (generation/'snapshot.json').exists():
            N.C.write_json_atomic(generation/'snapshot.json', snapshot)
        # Attempts retain actual settings separately from exact native inventory.
        audit = proof_path(stage, generation).parent/'attempts'/generation.name
        N._write_immutable(audit/(uuid.uuid4().hex+'.json'), resources)
        units = {}; new = 0
        for bid in N.REQUIRED:
            unit = N.admit_unit(snapshot, generation, con, bid)
            if unit is None and bid in selected and (max_new_batches is None or new < max_new_batches):
                recheck_runtime(runtime)
                unit = N.compute_new(snapshot, generation, con, bid); new += 1
                recheck_runtime(runtime)
            if unit is not None:
                units[str(bid)] = unit
        N.recheck(snapshot, con)
        _recheck_proof(stage, generation, runtime, expected_snapshot_sha256, proof_pin)
        if set(units) != {str(i) for i in N.REQUIRED}:
            return {'status': 'partial_cache_only', 'published': False,
                    'missing_batches': [i for i in N.REQUIRED if str(i) not in units]}
        leaf = N.merge(snapshot, generation, con, units)
        N.check_merged(snapshot, generation, con, units)
        evidence = N.native_evidence(snapshot, con, leaf)
        N.C.write_json_atomic(generation/'validation.json', evidence)
        N.C.write_json_atomic(leaf/'manifest.json', N.leaf_manifest(snapshot, generation))
        N.recheck(snapshot, con)
        for bid in N.REQUIRED:
            N.admit_unit(snapshot, generation, con, bid)
        manifest = {'stage': N.STAGE, 'schema': 'atx.fundamentals-release/v1', 'status': 'complete',
                    'contract': N.CONTRACT, 'semantic_producer': N.SEMANTIC_STAGE, 'source_backed_only': True,
                    'readiness': 'NOT_PRODUCTION_READY', 'source_quality_certified': False,
                    'snapshot_sha256': N.digest(snapshot), 'snapshot': N.identity(generation/'snapshot.json'),
                    'files': N._output_identities(generation), 'validation': N.identity(generation/'validation.json'),
                    'native_manifest': N.identity(leaf/'manifest.json'),
                    'units': {str(i): N.identity(generation/'units'/f'batch-{i:04d}'/'admission.json') for i in N.REQUIRED},
                    'gate_status': N.gate_status(evidence)}
        N.C.write_json_atomic(generation/'manifest.json', manifest)
        manifest_pin = N.identity(generation/'manifest.json')
        N.validate_generation(stage, generation, con, extract_admission, expected_admission_sha256, force_full_replay)
        _recheck_proof(stage, generation, runtime, expected_snapshot_sha256, proof_pin)
        N._publish_pointer(stage, generation, manifest_pin)
        return {'status': 'candidate_published', 'published': True, 'readiness': 'NOT_PRODUCTION_READY'}


def _recheck_proof(stage, generation, runtime, snapshot_sha, pin):
    recheck_runtime(runtime)
    R.guard_identity()
    R.check_disk(Path(stage).parent)
    if check_proof(stage, generation, runtime, snapshot_sha) != pin:
        raise ValueError('runtime provenance changed during work')


def consume(command, root, runtime, expected_snapshot_sha256, extract_admission=None,
            expected_admission_sha256=None, force_full_replay=False):
    if command not in ('validate', 'recover', 'rollback'):
        raise ValueError('unknown consumer command')
    if not N._sha(expected_snapshot_sha256):
        raise ValueError('independent caller expected native snapshot SHA256 required')
    stage = N._absolute(root)/N.STAGE
    with connection(stage, runtime) as (con, resources):
        # The original public validator is reused without any overrides.
        if command == 'validate':
            with N.writer_lock(stage):
                generation = N.resolve_current(stage)
                pin = check_proof(stage, generation, runtime, expected_snapshot_sha256)
                manifest = N.validate_generation(stage, generation, con, extract_admission,
                                                 expected_admission_sha256, force_full_replay)
                _recheck_proof(stage, generation, runtime, expected_snapshot_sha256, pin)
                return manifest
        # Frozen recover/rollback expose no before-finish hook: reproduce their
        # exact sequencing and insert only the separate runtime identity check.
        with N.writer_lock(stage):
            if command == 'recover':
                tx = N.read(stage/'pointer-transaction.json')
                if set(tx) != {'version', 'current', 'previous'} or tx.get('version') != N.VERSION:
                    raise ValueError('unsupported pointer transaction')
                generation = N._transaction_target(stage, tx['current'])
                if tx['previous'] is not None:
                    N._transaction_target(stage, tx['previous'])
                pin = check_proof(stage, generation, runtime, expected_snapshot_sha256)
                N.validate_generation(stage, generation, con)
                N._transaction_target(stage, tx['current'])
                if tx['previous'] is not None:
                    N._transaction_target(stage, tx['previous'])
                _recheck_proof(stage, generation, runtime, expected_snapshot_sha256, pin)
                N._finish_pointer(stage, tx)
            else:
                if (stage/'pointer-transaction.json').exists():
                    raise ValueError('unfinished pointer transaction')
                prior = N.read(stage/'previous.json'); generation = N._generation(stage, prior['generation'])
                if N.identity(generation/'manifest.json') != prior['manifest']:
                    raise ValueError('rollback identity mismatch')
                pin = check_proof(stage, generation, runtime, expected_snapshot_sha256)
                N.validate_generation(stage, generation, con)
                _recheck_proof(stage, generation, runtime, expected_snapshot_sha256, pin)
                N._publish_pointer(stage, generation, prior['manifest'])
            return {'status': command, 'generation': generation.name, 'resources': resources}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('contract', 'build', 'validate', 'recover', 'rollback'))
    parser.add_argument('--execution-contract', type=Path)
    parser.add_argument('--expected-contract-sha256'); parser.add_argument('--expected-runtime-code-sha256')
    parser.add_argument('--expected-snapshot-sha256'); parser.add_argument('--bindings', type=Path)
    parser.add_argument('--generation'); parser.add_argument('--root', type=Path)
    parser.add_argument('--max-new-batches', type=int)
    parser.add_argument('--extract-admission', type=Path); parser.add_argument('--expected-admission-sha256')
    parser.add_argument('--force-full-replay', action='store_true')
    args = parser.parse_args(argv)
    if args.command == 'contract':
        import json
        print(json.dumps(CONTRACT, indent=2)); return
    if args.execution_contract is None:
        parser.error('caller-pinned execution contract required')
    runtime = load_runtime(args.execution_contract, args.expected_contract_sha256, args.expected_runtime_code_sha256)
    options = {'extract_admission': args.extract_admission, 'expected_admission_sha256': args.expected_admission_sha256,
               'force_full_replay': args.force_full_replay}
    if args.command == 'build':
        if args.bindings is None or args.generation is None:
            parser.error('build requires bindings and generation')
        result = build(args.bindings, args.generation, runtime, args.expected_snapshot_sha256,
                       max_new_batches=args.max_new_batches, **options)
    else:
        if args.root is None:
            parser.error('consumer command requires root')
        result = consume(args.command, args.root, runtime, args.expected_snapshot_sha256, **options)
    print(result)


if __name__ == '__main__':
    main()
