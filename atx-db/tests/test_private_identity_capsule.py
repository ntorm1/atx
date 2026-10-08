"""Synthetic physical controls; no actual source or diagnostic-arm admission."""
import copy
import json
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import zipfile

import pytest

from atx_db.alpha_panel import private_identity_capsule as C


def native_text():
    return {
        'identity_links.py': '''from __future__ import annotations
def reconstruct(ir, inputs):
    return dict(tickers=ir.read_sec_ticker_snapshot(inputs.company_tickers.path),)
def preparation(work, pin):
    work.mkdir(parents=True, exist_ok=True)
    manifest_path = work / "preparation.json"
    return pin
''',
        'alpha_panel/identity_backfill.py': '''from __future__ import annotations
def build(con):
    con.execute("CREATE TABLE cand AS SELECT 1::BIGINT security_id")
    receipt = {}
    receipt["candidates"] = con.execute("SELECT count(*) FROM cand").fetchone()
    return receipt
''',
        'alpha_panel/identity_links.py': '''from __future__ import annotations
def validation(receipt):
        if not receipt["validation"]["pass"]:
            raise SystemExit("validation mode mismatch")
        return receipt
''',
        'alpha_panel/common.py': '''from __future__ import annotations
from pathlib import Path
PACKAGE_ROOT = Path(__file__).resolve().parents[3]  # .../atx-db
IDENTITY_R4_DIR = PACKAGE_ROOT / "data" / "research" / "identity_rehearsal" / "session8-phased-r4"
''',
        '_submissions_archive.py': '''from __future__ import annotations
_INDEX_VERSION = 1
class SubmissionsArchive:
    def _build_index(self) -> None:
        raise RuntimeError("synthetic native write must never be reached")
'''
    }


def test_switches_preserve_candidates_and_both_backfill_passes(monkeypatch, tmp_path):
    sources, patches = C.patched_sources(native_text())
    control = {'s1': False, 's2': False, 's4': False, 'preparation_binding': {'s1': False}}
    monkeypatch.setattr(C, 'current', lambda: copy.deepcopy(control))
    class IR:
        @staticmethod
        def read_sec_ticker_snapshot(path):
            return ('original-ticker',)
    class Input:
        class company_tickers:
            path = 'unchanged-directory'
    namespace = {}
    exec(sources['identity_links.py'], namespace)
    assert namespace['reconstruct'](IR, Input)['tickers'] == ('original-ticker',)
    assert namespace['preparation'](tmp_path / 'A', {})['diagnostic_capsule'] == {'s1': False}
    control['s1'] = True
    assert namespace['reconstruct'](IR, Input)['tickers'] == ()
    assert 'candidates' not in sources['identity_links.py']
    assert len(patches['identity_links.py']) == 3
    class Con:
        rows = 1
        calls = []
        def execute(self, sql):
            self.calls.append(sql)
            if sql.startswith('CREATE TABLE'):
                self.rows = 1
            elif sql == 'DELETE FROM cand':
                self.rows = 0
            return self
        def fetchone(self):
            return (self.rows,)
    namespace = {}
    exec(sources['alpha_panel/identity_backfill.py'], namespace)
    con = Con()
    assert namespace['build'](con)['candidates'] == (1,)
    control['s2'] = True
    for _ in range(2):  # backfill and the later combine both call native build
        assert namespace['build'](con)['candidates'] == (0,)
    assert con.calls.count('DELETE FROM cand') == 2
    namespace = {}
    exec(sources['alpha_panel/identity_links.py'], namespace)
    record = {'validation': {'seal': '2025-01-01', 'expected': {'rows': 2},
                             'got': {'rows': 1}, 'pass': False}}
    with pytest.raises(SystemExit, match='validation mode mismatch'):
        namespace['validation'](record)
    control['s4'] = True
    assert namespace['validation'](record) == record
    with pytest.raises(ValueError, match='exact native patch anchor'):
        C.patched_sources({**native_text(), 'identity_links.py': 'changed-native-source'})


def fixture_capsule(tmp_path, extra_identity=''):
    package = tmp_path / 'legacy' / 'src' / 'atx_db'
    sources = {**native_text(), '__init__.py': '', 'alpha_panel/__init__.py': '',
               'alpha_panel/private_identity_capsule.py': Path(C.__file__).read_text()}
    sources['identity_links.py'] += extra_identity
    for name, text in sources.items():
        path = package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    resource = package / 'typed-resource.json'
    resource.write_text('{"native":true}')
    spec = {'version': C.VERSION, 'project_package': str(package),
            'source_git_head': 'synthetic-fixture-only',
            'capsule_root': str(tmp_path / 'capsule'),
            'code_inventory': {name: C.pin(package / name) for name in sources},
            'resources': {'typed-resource.json': C.pin(resource)}}
    return C.create_capsule(spec), spec


def arm_spec(tmp_path, arm):
    originals = tmp_path / 'originals'
    originals.mkdir(exist_ok=True)
    roles = {}
    fixed = {}
    pins = {}
    for role in C.FIXED_INPUTS:
        root = originals / role
        tails = {'companyfacts': 'batch-0.parquet', 'fsds': 'sub/sub.parquet',
                 'finra_si': 'raw/si_1.csv'}
        path = root / tails[role] if role in tails else root
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(role)
        fixed[role] = str(root)
        roles[role] = [str(path)]
        pins[str(path)] = C.pin(path)
    alpha = originals / 'alpha'
    for role, relative in C.SEED_ROLES.items():
        path = alpha / relative
        if role not in ('calendar', 'short_interest'):
            if role == 'prices':
                path = path / '2020'
            path = path / 'part.parquet'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(role)
        roles[role] = [str(path)]
        pins[str(path)] = C.pin(path)
    documents = {}
    for role in ('native_receipt', 'ops_manifest', 'document_review', 'original_build'):
        path = originals / (role + '.json')
        path.write_text('{}')
        documents[role] = str(path)
        pins[str(path)] = C.pin(path)
    return {'arm': arm, 'fixed_inputs': fixed, 'input_pins': pins,
            'role_inventories': roles, 'original_alpha_root': str(alpha), **documents}


def test_checked_copy_same_code_roots_resources_resume_and_source_pins(tmp_path):
    capsule, original_spec = fixture_capsule(tmp_path)
    arms = {}
    for name in C.ARMS:
        arms[name] = C.bind_arm(capsule, arm_spec(tmp_path, name))
    controls = {name: item['control'] for name, item in arms.items()}
    assert len({item['same_code_sha256'] for item in controls.values()}) == 1
    assert len({json.dumps(item['common_binding_pin'], sort_keys=True) for item in controls.values()}) == 1
    assert controls['A']['rehearsal_root'] == controls['B1']['rehearsal_root']
    assert controls['B2']['rehearsal_root'] == controls['B12']['rehearsal_root']
    assert controls['A']['preparation_binding'] == controls['B1']['preparation_binding']
    assert controls['B2']['preparation_binding'] == controls['B12']['preparation_binding']
    assert controls['A']['preparation_binding'] != controls['B2']['preparation_binding']
    command = C.phase_command(capsule, arms['A'], 'identity', 'python')
    assert command[1] == '-I' and 'capsule' in command[2] and '--control-pin' in command
    seed = C.seed_arm(arms['A'])
    assert Path(seed['path']).is_file()
    assert Path(controls['A']['arm_root'], 'calendar.parquet').read_text() == 'calendar'
    changed = arm_spec(tmp_path, 'A')
    changed['fixed_inputs']['tickerhistory'] = str(tmp_path / 'wrong')
    with pytest.raises(ValueError, match='wrong-arm/resume'):
        C.bind_arm(capsule, changed)
    with pytest.raises(ValueError, match='escapes'):
        C.below(tmp_path / 'capsule', tmp_path / 'outside')
    missing = copy.deepcopy(original_spec)
    del missing['code_inventory']['__init__.py']
    missing['capsule_root'] = str(tmp_path / 'incomplete')
    with pytest.raises(ValueError, match='complete project Python'):
        C.create_capsule(missing)
    copy_path = Path(capsule['manifest']['capsule_root'], 'src/atx_db/typed-resource.json')
    copy_path.write_text('changed')
    with pytest.raises(ValueError, match='physical pin'):
        C.check({str(copy_path): capsule['manifest']['copied_inventory']['typed-resource.json']})


def test_consumption_finally_mutation_and_original_population_omission(tmp_path):
    path = tmp_path / 'original'
    path.write_text('original')
    pins = {str(path): C.pin(path)}
    with pytest.raises(ValueError, match='physical pin'):
        with C.consuming(pins):
            pins.clear()  # caller mutation cannot erase frozen final checks
            path.write_text('changed')
            raise RuntimeError('phase interrupted')
    capsule, _ = fixture_capsule(tmp_path)
    bound = C.bind_arm(capsule, arm_spec(tmp_path, 'A'))
    control = copy.deepcopy(bound['control'])
    control['role_inventories']['prices'] = []
    with pytest.raises(ValueError, match='complete original source role'):
        C._check_roles(control)
    # A missing complete native documentary closure cannot start an arm.
    with pytest.raises(KeyError):
        C._check_originals(bound['control'])


def test_native_original_documentary_closure_refuses_missing_required_doc(tmp_path):
    spec = arm_spec(tmp_path, 'A')
    pins = spec['input_pins']
    original_inputs = {}
    for name in ('ticker_history', 'companyfacts', 'submissions', 'company_tickers'):
        path = tmp_path / ('receipt-' + name)
        path.write_text(name)
        pins[str(path)] = C.pin(path)
        original_inputs[name] = {'path': str(path), 'sha256': pins[str(path)]['sha256'],
            'received_at': '2026-09-20T22:00:00', 'receipt_basis': 'original-physical'}
    document = tmp_path / 'original-document'
    document.write_text('original-document')
    pins[str(document)] = C.pin(document)
    submissions = Path(original_inputs['submissions']['path'])
    index = submissions.with_name(submissions.name + '.directory-v1-' + original_inputs['submissions']['sha256'] + '.sqlite3')
    cache_receipt = index.with_suffix('.sqlite3.sha256')
    index.write_text('source-shaped-independent-cache-pin')
    cache_receipt.write_text(C.pin(index)['sha256'])
    pins[str(index)], pins[str(cache_receipt)] = C.pin(index), C.pin(cache_receipt)
    review = {'version': 'identity_dated_document_review_v1', 'assertions': [
        {'document_path': str(document), 'document_sha256': pins[str(document)]['sha256']}]}
    Path(spec['document_review']).write_text(json.dumps(review))
    Path(spec['native_receipt']).write_text(json.dumps({'inputs': original_inputs}))
    export = Path(spec['ops_manifest']).parent / 'directory.parquet'
    export.write_text('complete-original-directory')
    pins[str(export)] = C.pin(export)
    Path(spec['ops_manifest']).write_text(json.dumps({'exports': [
        {'table': 'nasdaq_symbol_directory', 'file': export.name, **pins[str(export)]},
        {'table': 'originally-missing', 'file': None}]}))
    for name in ('document_review', 'native_receipt', 'ops_manifest'):
        pins[spec[name]] = C.pin(spec[name])
    build = {'pin': {'inputs': original_inputs,
        'ops_manifest_sha256': pins[spec['ops_manifest']]['sha256'],
        'document_review': {'path': spec['document_review'],
            'sha256': pins[spec['document_review']]['sha256'],
            'documents': {str(document): pins[str(document)]['sha256']}}}}
    Path(spec['original_build']).write_text(json.dumps(build))
    pins[spec['original_build']] = C.pin(spec['original_build'])
    C._check_originals(spec)
    del pins[str(document)]
    with pytest.raises(ValueError, match='required original reviewed document'):
        C._check_originals(spec)


def test_native_valid_cache_is_read_only_and_invalid_or_absent_cache_refuses(tmp_path):
    from atx_db import _submissions_archive as native
    source = tmp_path / 'originals' / 'submissions.zip'
    source.parent.mkdir()
    with zipfile.ZipFile(source, 'w') as archive:
        archive.writestr('CIK0000000001.json', '{"cik":1}')
    # Setup creates only synthetic native cache; actual originals are untouched.
    with native.SubmissionsArchive(source) as reader:
        index, receipt = reader.index_path, reader._receipt_path
        assert reader.read('CIK0000000001.json') == b'{"cik":1}'
    text = native_text()
    text['_submissions_archive.py'] = Path(native.__file__).read_text()
    modified, _ = C.patched_sources(text)
    target = tmp_path / 'copied_archive.py'
    target.write_text(modified['_submissions_archive.py'])
    module_spec = importlib.util.spec_from_file_location('synthetic_copied_archive', target)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    originals = {str(path): C.pin(path) for path in source.parent.iterdir()}
    with C.consuming(originals):
        with module.SubmissionsArchive(source) as reader:
            assert reader.read('CIK0000000001.json') == b'{"cik":1}'
    assert {str(path) for path in source.parent.iterdir()} == set(originals)
    # Corrupt cache keeps all native source/cache validation and refuses rebuild.
    with index.open('ab') as stream:
        stream.write(b'invalid-cache')
    before = {str(path): C.pin(path) for path in source.parent.iterdir()}
    with pytest.raises(ValueError, match='original writes refused'):
        module.SubmissionsArchive(source)
    C.check(before)
    assert {str(path) for path in source.parent.iterdir()} == set(before)
    index.unlink()
    receipt.unlink()
    before = {str(source): C.pin(source)}
    with pytest.raises(ValueError, match='original writes refused'):
        module.SubmissionsArchive(source)
    C.check(before)
    assert {str(path) for path in source.parent.iterdir()} == set(before)


def test_real_parent_traversal_refuses_before_copy_or_seed_write(tmp_path):
    capsule, spec = fixture_capsule(tmp_path)
    protected = tmp_path / 'protected'
    protected.mkdir()
    sentinel = protected / 'original.txt'
    sentinel.write_text('original-must-not-change')
    expected = C.pin(sentinel)
    escaped = Path(capsule['manifest']['capsule_root']) / '..' / 'protected' / 'escaped'
    with pytest.raises(ValueError, match='parent traversal'):
        C.below(capsule['manifest']['capsule_root'], escaped)
    changed = copy.deepcopy(spec)
    changed['capsule_root'] = str(escaped)
    with pytest.raises(ValueError, match='parent traversal'):
        C.create_capsule(changed)
    assert not (protected / 'escaped').exists()
    assert C.pin(sentinel) == expected
    arm = C.bind_arm(capsule, arm_spec(tmp_path, 'A'))
    control = copy.deepcopy(arm['control'])
    control['arm_root'] = str(Path(control['arm_root']) / '..' / '..' / '..' / 'protected')
    path = tmp_path / 'escaped-control.json'
    path.write_text(json.dumps(control))
    changed_arm = {'control_path': str(path), 'control_pin': C.pin(path)}
    with pytest.raises(ValueError, match='parent traversal'):
        C.seed_arm(changed_arm)
    assert sorted(path.name for path in protected.iterdir()) == ['original.txt']
    assert C.pin(sentinel) == expected


def test_fresh_process_uses_complete_copied_imports_and_final_mutation_refuses(tmp_path):
    extra = '''
if __name__ == '__main__':
    import json
    import sys
    from pathlib import Path
    control = _capsule_current()
    work = Path(control['work_root'])
    work.mkdir(parents=True, exist_ok=True)
    (work / 'wrapper_observed.json').write_text(json.dumps({
        'file': __file__, 'argv': sys.argv[1:], 'binding': control['preparation_binding']}))
    original = Path(control['fixed_inputs']['tickerhistory'])
    if original.read_text() == 'mutate-in-fixture':
        original.write_text('fixture-mutation')
'''
    capsule, _ = fixture_capsule(tmp_path, extra)
    spec = arm_spec(tmp_path, 'A')
    pins = spec['input_pins']
    originals = {}
    for name in ('ticker_history', 'companyfacts', 'submissions', 'company_tickers'):
        path = tmp_path / ('source-' + name)
        path.write_text(name)
        pins[str(path)] = C.pin(path)
        originals[name] = {'path': str(path), 'sha256': pins[str(path)]['sha256'],
            'received_at': '2026-09-20T22:00:00', 'receipt_basis': 'synthetic-original'}
    source = Path(originals['submissions']['path'])
    index = source.with_name(source.name + '.directory-v1-' + originals['submissions']['sha256'] + '.sqlite3')
    index.write_text('independently-pinned-protocol-fixture')
    index_receipt = index.with_suffix('.sqlite3.sha256')
    index_receipt.write_text(C.pin(index)['sha256'])
    pins[str(index)], pins[str(index_receipt)] = C.pin(index), C.pin(index_receipt)
    doc = tmp_path / 'document'
    doc.write_text('original')
    pins[str(doc)] = C.pin(doc)
    review = {'version': 'identity_dated_document_review_v1', 'assertions': [
        {'document_path': str(doc), 'document_sha256': pins[str(doc)]['sha256']}]}
    Path(spec['document_review']).write_text(json.dumps(review))
    Path(spec['ops_manifest']).write_text(json.dumps({'exports': [{'table': 'originally-missing', 'file': None}]}))
    Path(spec['native_receipt']).write_text(json.dumps({'inputs': originals}))
    for field in ('document_review', 'ops_manifest', 'native_receipt'):
        pins[spec[field]] = C.pin(spec[field])
    Path(spec['original_build']).write_text(json.dumps({'pin': {'inputs': originals,
        'ops_manifest_sha256': pins[spec['ops_manifest']]['sha256'],
        'document_review': {'path': spec['document_review'],
            'sha256': pins[spec['document_review']]['sha256'],
            'documents': {str(doc): pins[str(doc)]['sha256']}}}}))
    pins[spec['original_build']] = C.pin(spec['original_build'])
    arm = C.bind_arm(capsule, spec)
    command = C.phase_command(capsule, arm, 'prepare:vendor', sys.executable)
    # Deliberately hostile shared PYTHONPATH must not override copied project imports.
    env = {**os.environ, 'PYTHONPATH': str(tmp_path / 'untrusted-shared'),
           'PYTHONDONTWRITEBYTECODE': '1'}
    unguarded = dict(env)
    unguarded.pop('ATX_GUARD_JOB', None)
    refused = subprocess.run(command, env=unguarded, capture_output=True, text=True, timeout=20)
    assert refused.returncode != 0 and 'external ATX_GUARD_JOB required' in refused.stderr
    assert not Path(arm['control']['work_root']).exists()
    out_of_order = subprocess.run(C.phase_command(capsule, arm, 'combine', sys.executable),
                                 env=env, capture_output=True, text=True, timeout=20)
    assert out_of_order.returncode != 0 and 'original alpha stage sequence requires' in out_of_order.stderr
    assert not Path(arm['control']['arm_root'], 'capsule_receipts').exists()
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    observed = json.loads(Path(arm['control']['work_root'], 'wrapper_observed.json').read_text())
    assert Path(observed['file']) == Path(capsule['manifest']['capsule_root'], 'src/atx_db/identity_links.py')
    assert observed['argv'] == ['prepare', '--work', arm['control']['work_root'],
        '--receipt', arm['control']['native_receipt'], '--step', 'vendor']
    assert observed['binding'] == arm['control']['preparation_binding']
    # New separately named capsule with a physically pinned deliberate fixture mutation.
    fixture = tmp_path / 'mutating'
    fixture.mkdir()
    capsule2, _ = fixture_capsule(fixture, extra)
    spec2 = copy.deepcopy(spec)
    spec2['arm'] = 'B2'
    ticker = Path(spec2['fixed_inputs']['tickerhistory'])
    ticker.write_text('mutate-in-fixture')
    spec2['input_pins'][str(ticker)] = C.pin(ticker)
    arm2 = C.bind_arm(capsule2, spec2)
    failed = subprocess.run(C.phase_command(capsule2, arm2, 'prepare:vendor', sys.executable),
                            env=env, capture_output=True, text=True, timeout=20)
    assert failed.returncode != 0 and 'capsule physical pin differs' in failed.stderr
    assert not Path(arm2['control']['arm_root'], 'capsule_receipts').exists()


def test_a_b2_changed_original_input_refuses_common_capsule_binding(tmp_path):
    capsule, _ = fixture_capsule(tmp_path)
    first = C.bind_arm(capsule, arm_spec(tmp_path, 'A'))
    common = Path(first['control']['common_binding_path'])
    original = C.pin(common)
    changed = copy.deepcopy(arm_spec(tmp_path, 'B2'))
    old_root = Path(changed['fixed_inputs']['companyfacts'])
    alternate = tmp_path / 'different-companyfacts'
    alternate.mkdir()
    file = alternate / 'batch-0.parquet'
    file.write_text('independently-pinned-but-different-source')
    changed['fixed_inputs']['companyfacts'] = str(alternate)
    changed['role_inventories']['companyfacts'] = [str(file)]
    del changed['input_pins'][str(old_root / 'batch-0.parquet')]
    changed['input_pins'][str(file)] = C.pin(file)
    with pytest.raises(ValueError, match='capsule-wide original input binding differs'):
        C.bind_arm(capsule, changed)
    assert C.pin(common) == original
    assert not Path(capsule['manifest']['capsule_root'], 'arms', 'B2').exists()


def test_native_reconstruction_s1_only_removes_snapshot_evidence(tmp_path, monkeypatch):
    """Exact copied call expression feeds unchanged native typed reconstruction kernels.

    This does not invoke the resource-owning preparation producer or certify PIT.
    """
    import ast
    import datetime as dt
    from types import SimpleNamespace
    import duckdb
    from atx_db import identity_reconstruction as IR

    package = Path(IR.__file__).parent
    names = ('identity_links.py', 'alpha_panel/identity_backfill.py',
             'alpha_panel/identity_links.py', 'alpha_panel/common.py',
             '_submissions_archive.py')
    native = {name: (package / name).read_text(encoding='utf-8') for name in names}
    patched, _ = C.patched_sources(native)
    tree = ast.parse(patched['identity_links.py'])
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Attribute)
             and node.func.attr == 'reconstruct_in_batches']
    assert len(calls) == 1
    expressions = [kw.value for kw in calls[0].keywords if kw.arg == 'tickers']
    assert len(expressions) == 1
    ticker_expression = compile(ast.Expression(expressions[0]), '<exact-copied-native-tickers>', 'eval')
    control = {'s1': False}
    monkeypatch.setattr(C, 'current', lambda: copy.deepcopy(control))
    ticker_file = tmp_path / 'company_tickers.json'
    ticker_file.write_text(json.dumps({'0': {'cik_str': 123, 'ticker': 'XYZ', 'title': 'Fixture'}}))
    inputs = SimpleNamespace(company_tickers=SimpleNamespace(path=ticker_file))
    scope = {'ir': IR, 'inputs': inputs, '_current': C.current}
    original_tickers = IR.read_sec_ticker_snapshot(ticker_file)
    assert original_tickers
    dates = [dt.date(2019, month, 15) for month in (1, 4, 7, 10)]
    counts = [203, 307, 409, 503]  # distinct nonquantized counts; native weights unchanged
    line = IR.VendorLine(1, 'fixture:1', dates[0], dates[-1], 4, 'XYZ',
                        (('XYZ', dates[0], dates[-1]),))
    con = duckdb.connect(str(tmp_path / 'native.duckdb'))
    try:
        con.execute("SET memory_limit='128MB'")
        con.execute('SET threads=1')
        con.execute("SET TimeZone='UTC'")
        IR.stage_share_runs(con, [IR.VendorShareRun(1, count, day, day)
                                  for day, count in zip(dates, counts)])
        assert IR.stage_share_facts(con, [IR.IssuerShareFact(
            original_tickers[0][0], 'dei:EntityCommonStockSharesOutstanding', day,
            count * 1000, f'fixture-{i}', '10-Q', day + dt.timedelta(days=5))
            for i, (day, count) in enumerate(zip(dates, counts))], batch=2) == 4
        results = []
        for disabled in (False, True):
            control['s1'] = disabled
            tickers = eval(ticker_expression, scope)
            assert tickers == (() if disabled else original_tickers)
            results.append(list(IR.reconstruct_in_batches(
                con, [line], tickers=tickers,
                ticker_observed_at=dt.datetime(2026, 1, 1), batch_size=1)))
        assert len(results[0]) == len(results[1]) == 1
        links_a, links_b = results[0][0].links, results[1][0].links
        assert links_a and links_b
        shares_a = [(link.vendor_id, link.cik, item) for link in links_a
                    for item in link.items if item.kind == IR.EV_SHARES]
        shares_b = [(link.vendor_id, link.cik, item) for link in links_b
                    for item in link.items if item.kind == IR.EV_SHARES]
        assert shares_a == shares_b and len(shares_a) == 4
        assert sum(item.kind == IR.EV_SYMBOL for link in links_a for item in link.items) == 1
        assert not any(item.kind == IR.EV_SYMBOL for link in links_b for item in link.items)
    finally:
        con.close()


def test_native_backfill_s2_preserves_typed_strict_name_population_both_passes(tmp_path, monkeypatch):
    """Replay exact native connection-taking SQL statements, not the 600MB build entrypoint."""
    import ast
    import duckdb
    from atx_db.alpha_panel import identity_backfill as BF

    package = Path(BF.__file__).parents[1]
    targets = ('identity_links.py', 'alpha_panel/identity_backfill.py',
               'alpha_panel/identity_links.py', 'alpha_panel/common.py', '_submissions_archive.py')
    sources = {name: (package / name).read_text(encoding='utf-8') for name in targets}
    patched, _ = C.patched_sources(sources)
    build = next(n for n in ast.parse(patched['alpha_panel/identity_backfill.py']).body
                 if isinstance(n, ast.FunctionDef) and n.name == 'build')
    sql_statements = [n for n in build.body if isinstance(n, ast.Expr)
                      and isinstance(n.value, ast.Call)
                      and isinstance(n.value.func, ast.Attribute)
                      and isinstance(n.value.func.value, ast.Name)
                      and n.value.func.value.id == 'con' and n.value.func.attr == 'execute']
    assert len(sql_statements) == 9  # fail closed if native sequencing changes
    deletes = [n for n in build.body if isinstance(n, ast.If)
               and isinstance(n.test, ast.Subscript)]
    assert len(deletes) == 1
    switch = compile(ast.Module(body=deletes, type_ignores=[]), '<exact-copied-native-s2>', 'exec')
    name_branch = next(n for n in build.body if isinstance(n, ast.If)
                       and isinstance(n.test, ast.Call)
                       and isinstance(n.test.func, ast.Attribute)
                       and n.test.func.attr == 'exists')
    name_expression = next(n.value for n in name_branch.body if isinstance(n, ast.Assign)
                           and any(isinstance(t, ast.Name) and t.id == 'name_union' for t in n.targets))
    name_code = compile(ast.Expression(name_expression), '<exact-native-name-union>', 'eval')
    assert C.STAGES['backfill'] == C.STAGES['combine']
    control = {'s2': False}
    monkeypatch.setattr(C, 'current', lambda: copy.deepcopy(control))
    con = duckdb.connect(str(tmp_path / 'backfill.duckdb'))
    try:
        con.execute("SET memory_limit='128MB'")
        con.execute('SET threads=1')
        con.execute("SET TimeZone='UTC'")
        def physical(name, query):
            path = tmp_path / (name + '.parquet')
            con.execute(f"COPY ({query}) TO '{path.as_posix()}' (FORMAT PARQUET)")
            return path.as_posix()
        links = physical('snapshot', """SELECT * FROM (VALUES
            (1::BIGINT,'123','P','current_ticker_verified',NULL::DATE,TIMESTAMP '2026-09-20'),
            (1::BIGINT,'123','P','current_ticker_verified',NULL::DATE,TIMESTAMP '2026-09-20'),
            (2::BIGINT,'222','J','current_ticker_verified',NULL::DATE,TIMESTAMP '2026-09-20'),
            (2::BIGINT,'223','J','current_ticker_verified',NULL::DATE,TIMESTAMP '2026-09-20'),
            (3::BIGINT,'333','P','dated_share_evidence',NULL::DATE,TIMESTAMP '2020-01-01'),
            (4::BIGINT,'444','P','current_ticker_verified',DATE '2026-01-01',TIMESTAMP '2026-09-20'))
            t(perm_security_id,cik,link_primary,link_basis,valid_until,available_at)""")
        strict = physical('strict', """SELECT 9::BIGINT security_id,999::BIGINT cik,
            DATE '2026-09-01' start,DATE '2026-09-02' end_incl,'P' AS "primary",
            'strict' tier,'dated_share_evidence' basis,TIMESTAMP '2020-01-01' available_at""")
        prices = physical('prices', """SELECT * FROM (VALUES
            (1::BIGINT,DATE '2026-09-01'),(1::BIGINT,DATE '2026-09-02'),
            (2::BIGINT,DATE '2026-09-01'),(2::BIGINT,DATE '2026-09-02'))t(security_id,session_date)""")
        cal = physical('calendar', "SELECT * FROM (VALUES(DATE '2026-09-01'),(DATE '2026-09-02'))t(session_date)")
        sub = physical('sub', "SELECT 123::BIGINT cik,DATE '2019-01-01' filed,'10-K' form")
        cf = physical('facts', "SELECT 123::BIGINT cik,DATE '2019-01-01' filed_date,'10-Q' form,'us-gaap' taxonomy")
        names_path = Path(physical('names', """SELECT 8::BIGINT security_id,888::BIGINT cik,
            DATE '2026-09-01' session_date,TIMESTAMP '2020-01-01' available_at"""))
        forms = ', '.join(f"'{form}'" for form in BF.PERIODIC)
        scope = dict(con=con, links=links, strict=strict, prices=prices, cal=cal,
                     sub=sub, cf=cf, forms=forms, names_path=names_path,
                     RUN_GAP_SESSIONS=BF.RUN_GAP_SESSIONS, ALIVE_FROM=BF.ALIVE_FROM,
                     COMBINED_FROM=BF.COMBINED_FROM, _current=C.current)
        for disabled in (False, True):
            control['s2'] = disabled
            for phase in ('backfill', 'combine'):
                scope['name_union'] = eval(name_code, scope) if phase == 'combine' else ''
                counts = None
                for index, statement in enumerate(sql_statements):
                    exec(compile(ast.Module(body=[statement], type_ignores=[]), '<exact-native-backfill-sql>', 'exec'), scope)
                    if index == 0:
                        exec(switch, scope)
                        counts = con.execute('SELECT count(*),count(DISTINCT security_id),count(DISTINCT cik) FROM cand').fetchone()
                assert counts == ((0, 0, 0) if disabled else (4, 2, 3))
                assert con.execute('SELECT DISTINCT security_id FROM cand ORDER BY 1').fetchall() == ([] if disabled else [(1,)])
                assert con.execute("SELECT security_id,cik,session_date FROM comb_days WHERE tier='strict' ORDER BY ALL").fetchall() == [
                    (9, 999, __import__('datetime').date(2026, 9, day)) for day in (1, 2)]
                assert con.execute("SELECT count(*) FROM comb_days WHERE tier='name'").fetchone()[0] == (1 if phase == 'combine' else 0)
                assert con.execute("SELECT count(*) FROM comb_days WHERE tier='backfill'").fetchone()[0] == (0 if disabled else 4)
    finally:
        con.close()

