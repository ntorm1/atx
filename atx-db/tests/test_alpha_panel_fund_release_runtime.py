"""Offline orchestration equivalence and separate runtime trust boundaries."""
import contextlib
import copy
import json
from pathlib import Path
import types

import pytest

from atx_db.alpha_panel import fund_release_runtime as A
from atx_db.alpha_panel import fund_batch_release as N


@pytest.fixture
def runtime(tmp_path):
    path = tmp_path/'execution.json'
    path.write_text(json.dumps(A.CONTRACT), encoding='utf-8')
    return A.load_runtime(path, N.identity(path)['sha256'], N.digest(A.code_inventory()))


@pytest.mark.parametrize('bad', ['missing_contract_pin', 'missing_code_pin', 'stale_contract', 'stale_code', 'wrong_contract'])
def test_explicit_runtime_pins(tmp_path, bad):
    path = tmp_path/'execution.json'; path.write_text(json.dumps(A.CONTRACT), encoding='utf-8')
    contract_pin = N.identity(path)['sha256']; code_pin = N.digest(A.code_inventory())
    if bad == 'missing_contract_pin': contract_pin = None
    if bad == 'missing_code_pin': code_pin = None
    if bad == 'stale_contract': contract_pin = '0'*64
    if bad == 'stale_code': code_pin = '0'*64
    if bad == 'wrong_contract':
        doc = copy.deepcopy(A.CONTRACT); doc['duckdb']['memory'] = '128MB'
        path.write_text(json.dumps(doc), encoding='utf-8'); contract_pin = N.identity(path)['sha256']
    with pytest.raises(ValueError): A.load_runtime(path, contract_pin, code_pin)


def test_runtime_recheck_after_work(runtime, monkeypatch):
    inventory = copy.deepcopy(runtime['code']); next(iter(inventory.values()))['bytes'] += 1
    monkeypatch.setattr(A, 'code_inventory', lambda: inventory)
    with pytest.raises(ValueError, match='runtime code changed'): A.recheck_runtime(runtime)


def test_execution_contract_recheck(runtime):
    Path(runtime['contract_path']).write_text('{}', encoding='utf-8')
    with pytest.raises(ValueError, match='execution contract changed'): A.recheck_runtime(runtime)


def test_proof_missing_changed_and_exact_native_inventory(runtime, tmp_path):
    stage = tmp_path/'root'/N.STAGE; generation = N._generation(stage, 'new')
    snapshot = {'binding': {'extract_root': str(tmp_path/'extract'), 'prep_root': str(tmp_path/'prep')}}
    N.C.write_json_atomic(generation/'snapshot.json', snapshot); sha = N.digest(snapshot)
    with pytest.raises(ValueError, match='missing caller'): A.check_proof(stage, generation, runtime, sha)
    pin = A.check_proof(stage, generation, runtime, sha, create=True)
    assert {p.name for p in generation.iterdir()} == {'snapshot.json'}
    assert A.check_proof(stage, generation, runtime, sha) == pin
    with pytest.raises(ValueError, match='stale generation runtime'): A.check_proof(stage, generation, runtime, '0'*64)
    N.C.write_json_atomic(A.proof_path(stage, generation), {})
    with pytest.raises(ValueError, match='stale generation runtime'): A.check_proof(stage, generation, runtime, sha)


def setup_protocol(tmp_path, monkeypatch, runtime, mode='complete'):
    root = tmp_path/'root'; root.mkdir()
    binding = {'root': str(root), 'extract_root': str(root/'extract'), 'prep_root': str(root/'prep')}
    binding_path = tmp_path/'binding.json'; N.C.write_json_atomic(binding_path, binding)
    snapshot = {'binding': binding, 'frozen': 'synthetic'}; calls = []; admitted = set(range(84))
    con = types.SimpleNamespace(close=lambda: None)
    @contextlib.contextmanager
    def connection(*args): yield con, {'guard': 'synthetic', 'duckdb_actual': ['244.1 MiB', 1, 'UTC']}
    monkeypatch.setattr(A, 'connection', connection)
    monkeypatch.setattr(A.R, 'guard_identity', lambda: {'fixture': True})
    monkeypatch.setattr(A.R, 'check_disk', lambda *args: 40*A.R.GIB)
    monkeypatch.setattr(N.C, 'connect', lambda **kwargs: con)
    def freeze(*args, **kwargs): calls.append(('freeze', kwargs)); return snapshot
    def admit(snapshot, generation, con, bid):
        calls.append(('admit', bid))
        if bid not in admitted: return None
        N.C.write_json_atomic(generation/'units'/f'batch-{bid:04d}'/'admission.json', {'bid': bid})
        return {'bid': bid}
    def compute(snapshot, generation, con, bid):
        calls.append(('compute', bid))
        if mode == 'interrupt':
            N.C.write_json_atomic(generation/'units'/f'batch-{bid:04d}'/'attempt-failed'/'start.json', {'bid': bid})
            raise KeyboardInterrupt()
        admitted.add(bid)
        N.C.write_json_atomic(generation/'units'/f'batch-{bid:04d}'/'admission.json', {'bid': bid})
        return {'bid': bid}
    def merge(snapshot, generation, con, units):
        calls.append(('merge', len(units))); leaf = generation/N.SEMANTIC_STAGE; leaf.mkdir(); return leaf
    monkeypatch.setattr(N, 'freeze', freeze); monkeypatch.setattr(N, 'admit_unit', admit)
    monkeypatch.setattr(N, 'compute_new', compute); monkeypatch.setattr(N, 'merge', merge)
    monkeypatch.setattr(N, 'recheck', lambda *args, **kwargs: calls.append(('recheck',)))
    monkeypatch.setattr(N, 'check_merged', lambda *args: calls.append(('check_merged',)))
    monkeypatch.setattr(N, 'native_evidence', lambda *args: calls.append(('evidence',)) or {})
    monkeypatch.setattr(N, 'leaf_manifest', lambda *args: calls.append(('leaf_manifest',)) or {})
    monkeypatch.setattr(N, '_output_identities', lambda *args: calls.append(('outputs',)) or {})
    monkeypatch.setattr(N, 'gate_status', lambda *args: calls.append(('gates',)) or {'whole_dataset_acceptance': 'FAIL_PENDING'})
    monkeypatch.setattr(N, 'validate_generation', lambda *args, **kwargs: calls.append(('public_validate',)))
    monkeypatch.setattr(N, '_publish_pointer', lambda *args: calls.append(('publish',)))
    monkeypatch.setenv('ATX_GUARD_JOB', 'synthetic-for-native-orchestration-only')
    return binding_path, snapshot, calls, admitted


def test_full_native_build_phase_and_all85_replay_equivalence(tmp_path, monkeypatch, runtime):
    binding_path, snapshot, calls, admitted = setup_protocol(tmp_path, monkeypatch, runtime)
    native = N.build(binding_path, 'native', max_new_batches=1)
    native_calls = calls.copy(); calls.clear(); admitted.remove(84)
    adapted = A.build(binding_path, 'adapted', runtime, N.digest(snapshot), max_new_batches=1)
    assert adapted == native
    assert calls == native_calls
    assert [bid for phase, *tail in calls if phase == 'admit' for bid in tail] == list(range(85))*2
    assert calls[-2:] == [('public_validate',), ('publish',)]


def test_bounded_continuation_replays_prior_units_even_with_zero_new(tmp_path, monkeypatch, runtime):
    binding_path, snapshot, calls, _ = setup_protocol(tmp_path, monkeypatch, runtime)
    result = A.build(binding_path, 'partial', runtime, N.digest(snapshot), max_new_batches=0)
    assert result['missing_batches'] == [84] and not result['published']
    assert [row[1] for row in calls if row[0] == 'admit'] == list(range(85))
    assert not any(row[0] in ('merge', 'publish', 'compute') for row in calls)
    calls.clear()
    result = A.build(binding_path, 'partial', runtime, N.digest(snapshot), max_new_batches=1)
    assert result['published'] and ('compute', 84) in calls


def test_interruption_preserves_failed_attempt_and_runtime_proof(tmp_path, monkeypatch, runtime):
    binding_path, snapshot, calls, _ = setup_protocol(tmp_path, monkeypatch, runtime, 'interrupt')
    with pytest.raises(KeyboardInterrupt): A.build(binding_path, 'partial', runtime, N.digest(snapshot), max_new_batches=1)
    stage = Path(snapshot['binding']['root'])/N.STAGE; generation = N._generation(stage, 'partial')
    assert (generation/'units/batch-0084/attempt-failed/start.json').is_file()
    assert not (generation/'units/batch-0084/admission.json').exists()
    assert A.proof_path(stage, generation).is_file() and not (stage/'.writer.lock').exists()
    assert not any(row[0] == 'publish' for row in calls)


def test_snapshot_pin_refuses_before_any_native_unit(tmp_path, monkeypatch, runtime):
    binding_path, snapshot, calls, _ = setup_protocol(tmp_path, monkeypatch, runtime)
    with pytest.raises(ValueError, match='independent caller pin'):
        A.build(binding_path, 'partial', runtime, '0'*64, max_new_batches=1)
    assert calls == [('freeze', {'extract_admission': None, 'expected_admission_sha256': None, 'force_full_replay': False})]


@pytest.mark.parametrize('command', ['recover', 'rollback'])
def test_pointer_operations_native_equivalence_and_runtime_drift_before_finish(tmp_path, monkeypatch, runtime, command):
    root = tmp_path/'root'; stage = root/N.STAGE; gen = N._generation(stage, 'target')
    snapshot = {'binding': {'root': str(root), 'extract_root': str(root/'extract'), 'prep_root': str(root/'prep')}}
    N.C.write_json_atomic(gen/'snapshot.json', snapshot); N.C.write_json_atomic(gen/'manifest.json', {})
    target = {'generation': gen.name, 'manifest': N.identity(gen/'manifest.json')}
    tx = {'version': N.VERSION, 'current': target, 'previous': None}
    N.C.write_json_atomic(stage/'pointer-transaction.json' if command == 'recover' else stage/'previous.json', tx if command == 'recover' else target)
    pin = N.digest(snapshot); A.check_proof(stage, gen, runtime, pin, create=True)
    calls = []; con = object()
    @contextlib.contextmanager
    def connection(*args): yield con, {}
    monkeypatch.setattr(A, 'connection', connection)
    monkeypatch.setattr(A.R, 'guard_identity', lambda: {'fixture': True})
    monkeypatch.setattr(A.R, 'check_disk', lambda *args: 40*A.R.GIB)
    original_target = N._transaction_target
    def target_check(*args): calls.append('target'); return original_target(*args)
    monkeypatch.setattr(N, '_transaction_target', target_check)
    monkeypatch.setattr(N, 'validate_generation', lambda *args: calls.append('validate'))
    monkeypatch.setattr(N, '_finish_pointer', lambda *args: calls.append('finish'))
    monkeypatch.setattr(N, '_publish_pointer', lambda *args: calls.append('publish'))
    (N.recover if command == 'recover' else N.rollback)(stage, con)
    native_calls = calls.copy(); calls.clear()
    A.consume(command, root, runtime, pin)
    assert calls == native_calls
    calls.clear()
    def drift(*args):
        calls.append('validate'); Path(runtime['contract_path']).write_text('{}', encoding='utf-8')
    monkeypatch.setattr(N, 'validate_generation', drift)
    with pytest.raises(ValueError, match='execution contract changed'): A.consume(command, root, runtime, pin)
    assert 'finish' not in calls and 'publish' not in calls


def test_cli_requires_caller_runtime_before_resource_work():
    with pytest.raises(SystemExit): A.main(['build', '--generation', 'x'])


def test_guard_string_and_low_disk_do_not_grant_resource_admission(tmp_path, monkeypatch):
    monkeypatch.delenv('ATX_GUARD_JOB', raising=False)
    with pytest.raises(RuntimeError, match='memory guard required'): A.R.guard_identity()
    monkeypatch.setenv('ATX_GUARD_JOB', 'not-a-real-windows-job')
    with pytest.raises(RuntimeError, match='guard job unavailable'): A.R.guard_identity()
    monkeypatch.setattr(A.R.shutil, 'disk_usage', lambda *args: types.SimpleNamespace(free=40*A.R.GIB-1))
    with pytest.raises(RuntimeError, match='40GiB free disk'): A.R.check_disk(tmp_path)


def test_recovery_refuses_missing_runtime_proof_and_retains_journal(tmp_path, monkeypatch, runtime):
    root = tmp_path/'root'; stage = root/N.STAGE; generation = N._generation(stage, 'target')
    N.C.write_json_atomic(generation/'manifest.json', {})
    tx = {'version': N.VERSION, 'current': {'generation': 'target', 'manifest': N.identity(generation/'manifest.json')}, 'previous': None}
    N.C.write_json_atomic(stage/'pointer-transaction.json', tx)
    @contextlib.contextmanager
    def connection(*args): yield object(), {}
    monkeypatch.setattr(A, 'connection', connection)
    called = []; monkeypatch.setattr(N, 'validate_generation', lambda *args: called.append(True))
    with pytest.raises(ValueError, match='missing caller-admitted runtime'):
        A.consume('recover', root, runtime, '0'*64)
    assert N.read(stage/'pointer-transaction.json') == tx and called == []


def test_public_consumer_uses_native_complete_validator_with_explicit_admission(tmp_path, monkeypatch, runtime):
    root = tmp_path/'root'; stage = root/N.STAGE; generation = N._generation(stage, 'target')
    snapshot = {'binding': {'root': str(root), 'extract_root': str(root/'extract'), 'prep_root': str(root/'prep')}}
    N.C.write_json_atomic(generation/'snapshot.json', snapshot); N.C.write_json_atomic(generation/'manifest.json', {})
    N.C.write_json_atomic(stage/'current.json', {'generation': 'target', 'manifest': N.identity(generation/'manifest.json')})
    sha = N.digest(snapshot); A.check_proof(stage, generation, runtime, sha, create=True)
    con = object(); calls = []
    @contextlib.contextmanager
    def connection(*args): yield con, {}
    monkeypatch.setattr(A, 'connection', connection)
    monkeypatch.setattr(A.R, 'guard_identity', lambda: {'fixture': True})
    monkeypatch.setattr(A.R, 'check_disk', lambda *args: 40*A.R.GIB)
    def validation(*args): calls.append(args); return {'native_complete': True}
    monkeypatch.setattr(N, 'validate_generation', validation)
    result = A.consume('validate', root, runtime, sha, extract_admission=tmp_path/'admission.json',
                       expected_admission_sha256='1'*64, force_full_replay=True)
    assert result == {'native_complete': True}
    assert calls == [(stage.resolve(), generation, con, tmp_path/'admission.json', '1'*64, True)]


def test_mutable_execution_contract_location_refused_before_native_work(tmp_path, monkeypatch):
    root = tmp_path/'root'; root.mkdir(); stage = root/N.STAGE
    contract = stage/'runtime.json'; N.C.write_json_atomic(contract, A.CONTRACT)
    runtime = A.load_runtime(contract, N.identity(contract)['sha256'], N.digest(A.code_inventory()))
    binding = tmp_path/'binding.json'
    N.C.write_json_atomic(binding, {'root': str(root), 'extract_root': str(root/'extract'), 'prep_root': str(root/'prep')})
    with pytest.raises(ValueError, match='outside mutable native'):
        A.build(binding, 'new', runtime, '1'*64)
