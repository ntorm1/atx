"""Offline native adapter fixtures; fixture review evidence certifies no sources."""
import json
from dataclasses import asdict

import duckdb
import pytest
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import private_tier1_family_sources as F
from tests.test_text_features_native import native


def write(root, name, blob):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return str(path.resolve())


def reviewed(root, name, native, generation, operation):
    spec = {'interface_version': F.VERSION, 'root': str(root.resolve()),
            'generation': str(generation.resolve()), 'manifest_pin': F.pin(generation/'manifest.json'),
            'native': native}
    validation, _, dependencies, _ = operation(spec, generation)
    code = F._code(name)
    review = {'schema': 'atx.tier1-family-review/v1', 'status': 'scoped_pass',
              'family': name, 'interface_version': F.VERSION,
              'binding_sha256': F.digest(F.binding(spec)), 'code_pins': code}
    path = write(root, 'synthetic-review.json', json.dumps(review).encode())
    spec['independent_review'] = {'path': path, 'expected_sha256': F.pin(path)['sha256']}
    spec['dependency_pins'] = {**code, **dependencies, str(generation/'manifest.json'): spec['manifest_pin'], path: F.pin(path)}
    spec['expected_validation_sha256'] = F.digest(validation)
    return spec


def connection(root):
    con = duckdb.connect(str(root/'consumer.duckdb'))
    con.execute("SET memory_limit='128MB'; SET threads=1; SET TimeZone='UTC'")
    return con


def text_admission(tmp_path, native, con):
    from atx_db.alpha_panel import filing_text_native as N, text_features_native as T
    source, output, plan = native
    generation = T.publish_native(source, output, plan, N.digest(plan), unit_size=2)
    spec = reviewed(tmp_path, 'text_features',
                    {'source_root': str(source), 'expected_plan_sha256': N.digest(plan)},
                    generation, F._text)
    return F.admit('text_features', spec, con)


def test_text_many_batches_constant_checks_and_frozen_binding(tmp_path, native, monkeypatch):
    from atx_db.alpha_panel import filing_text_native as N
    with connection(tmp_path) as con:
        admission = text_admission(tmp_path, native, con)
        expected = pq.read_table(admission['products']['text/features.parquet']).to_pylist()
        calls = []
        original = N.check_plan
        def counted(*args, **kwargs):
            calls.append(1)
            return original(*args, **kwargs)
        monkeypatch.setattr(N, 'check_plan', counted)
        rows = F.iter_rows(admission, 'text/features.parquet', con, batch_size=1)
        first = next(rows)
        # Mutating the caller's dictionaries cannot redirect the suspended read
        # or erase the independent final dependency checks.
        admission['validation']['dependencies'].clear()
        admission['dependency_pins'].clear()
        admission['products']['text/features.parquet'] = tmp_path/'not-admitted.parquet'
        assert [first, *rows] == expected
        assert len(expected) == 6 and len(calls) == 2


def test_text_early_close_checks_original_source_mutation(tmp_path, native, monkeypatch):
    from atx_db.alpha_panel import filing_text_native as N
    with connection(tmp_path) as con:
        admission = text_admission(tmp_path, native, con)
        calls = []
        original = N.check_plan
        def counted(*args, **kwargs):
            calls.append(1)
            return original(*args, **kwargs)
        monkeypatch.setattr(N, 'check_plan', counted)
        rows = F.iter_rows(admission, 'text/features.parquet', con, batch_size=1)
        next(rows)
        source = native[0]/'1-2018.htm'
        source.write_bytes(source.read_bytes()+b' changed')
        with pytest.raises(ValueError):
            rows.close()
        assert len(calls) == 2


def terminal(root):
    from tests.test_mna_exact_v2 import fixture
    from atx_db.alpha_panel import mna_publish_v2 as P
    args, extra, expected = fixture(stock=True)
    candidate, document, source_pin, vendor, target_pin, economic, economic_pin = args
    sink = pa.BufferOutputStream()
    pq.write_table(pa.Table.from_pylist([asdict(candidate)]), sink)
    original = sink.getvalue().to_pybytes()
    key = f'{candidate.security_id}/{candidate.last_session}/{candidate.available_at.isoformat()}'
    url = 'https://www.sec.gov/Archives/edgar/data/1/000000000120000001/filing.txt'
    receipt = P._json({'url': url, 'sha256': source_pin.sha256, 'status': 200,
                      'fetched_at': str(source_pin.fetched_at), 'bytes': len(document), 'error': None})
    manifests = {'legacy-candidate': b'independent-offline-original'}
    contract = P._json({'schema': P.SCHEMA, 'admission_id': 'offline-native-fixture',
        'expected_code': P.publisher_code_pins(), 'candidate_inventory': [asdict(candidate)],
        'source_pins': [asdict(source_pin)], 'candidate_clock_rule': 'legacy-stage-documented-UTC',
        'candidate_original_sha256': F.pin(write(root, 'candidates.parquet', original))['sha256'],
        'candidate_source_links': {key: [source_pin.source_id]}, 'gate_handcheck': 'unmeasured',
        'input_manifests_sha256': {k: F.pin(write(root, k+'.json', b))['sha256'] for k,b in manifests.items()},
        'source_receipts': {source_pin.source_id: {'sha256': F.pin(write(root, 'receipt.json', receipt))['sha256'], 'url': url}},
        'exact_admissions': {key: expected}, 'auxiliary_clock_inventory': {}})
    exact = {key: {'vendor': vendor, 'target_pin': target_pin, 'target_economic': economic,
                   'target_economic_pin': economic_pin, **extra}}
    kw = {'candidate_original': original, 'input_manifests': manifests,
          'receipts': {source_pin.source_id: receipt}, 'exact_inputs': exact}
    sha = F.pin(write(root, 'contract.json', contract))['sha256']
    generation = P.publish_value_only(root, contract, sha, {source_pin.source_id: document}, (source_pin,), **kw)
    encoded = json.loads(P._json({'target_pin': asdict(target_pin), 'target_economic_pin': asdict(economic_pin),
                               'acquirer_pin': asdict(extra['acquirer_pin']), 'acquirer_economic_pin': asdict(extra['acquirer_economic_pin'])}))
    encoded.update(vendor=write(root, 'vendor.parquet', vendor), target_economic=write(root, 'target.html', economic),
                   acquirer_economic=write(root, 'acquirer.html', extra['acquirer_economic']))
    native = {'contract_path': str(root/'contract.json'), 'expected_contract_sha256': sha,
              'candidate_original': str(root/'candidates.parquet'),
              'objects': {source_pin.source_id: write(root, 'filing.txt', document)},
              'input_manifests': {'legacy-candidate': str(root/'legacy-candidate.json')},
              'receipts': {source_pin.source_id: str(root/'receipt.json')}, 'exact_inputs': {key: encoded},
              'bulk_clocks': {}, 'clock_receipts': {}, 'handcheck_original': None}
    return reviewed(root, 'delisting_v2', native, generation, F._terminal)


def test_terminal_native_value_and_consumption_drift(tmp_path):
    spec = terminal(tmp_path)
    with connection(tmp_path) as con:
        admission = F.admit('delisting_v2', spec, con)
        row, = list(F.iter_rows(admission, 'events.parquet', con))
        assert row['proceeds_per_share'] == 15 and row['dlret'] == .5 and row['dlret_exact']
        assert not admission['source_quality_certified']
        with F.consuming(admission, con) as products:
            assert con.execute('SELECT dlret FROM read_parquet(?)', [str(products['events.parquet'])]).fetchone() == (.5,)
        path = spec['native']['objects'][next(iter(spec['native']['objects']))]
        with open(path, 'ab') as stream:
            stream.write(b' changed')
        with pytest.raises(ValueError, match='dependency changed'):
            list(F.iter_rows(admission, 'events.parquet', con))


def test_prerequisite_and_complete_inventory_refusal(tmp_path):
    spec = terminal(tmp_path)
    with connection(tmp_path) as con:
        spec['dependency_pins'].pop(spec['native']['candidate_original'])
        with pytest.raises(ValueError, match='complete native dependency inventory'):
            F.admit('delisting_v2', spec, con)
        review = json.loads(open(spec['independent_review']['path']).read())
        review['status'] = 'pending'
        path = write(tmp_path, 'synthetic-review.json', json.dumps(review).encode())
        spec['independent_review']['expected_sha256'] = F.pin(path)['sha256']
        spec['dependency_pins'][path] = F.pin(path)
        with pytest.raises(ValueError, match='prerequisite pending'):
            F.admit('delisting_v2', spec, con)


def test_task13_native_distribution_and_mutation(tmp_path, monkeypatch):
    from tests.test_alpha_panel_distributions import admitted_fixture, declaration
    from atx_db.alpha_panel import common as C, distributions as D
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT', str(tmp_path/'lake'))
    original = write(tmp_path, 'release.txt', declaration().encode())
    catalog = tmp_path/'documents.jsonl'
    record = {'source_path': original, 'source_sha256': F.pin(original)['sha256'], 'stored_sha256': F.pin(original)['sha256'],
              'storage_encoding': 'plain', 'source_doc': 'release.txt', 'fetched_at': '2021-01-03T21:00:00Z',
              'source_url': 'https://www.sec.gov/Archives/fixture', 'accession': '0000000001-21-000001',
              'available_at': '2021-01-03T21:00:00Z', 'security_id': 1, 'cik': 1,
              'security_class': 'Class A common stock', 'currency': 'USD',
              'link_basis': 'explicit native class to fixture security', 'link_tier': 'fixture'}
    catalog.write_text(json.dumps(record)+'\n')
    admission, sha = admitted_fixture(tmp_path, catalog)
    result = D.build(catalog, [], admission_path=admission, expected_sha256=sha)
    generation = tmp_path/'lake/corporate_actions/distributions_products'/result['product_id']
    native = {'admission_path': str(admission), 'expected_admission_sha256': sha}
    spec = reviewed(tmp_path, 'distributions', native, generation, lambda s,g: F._task13('distributions', s,g))
    with connection(tmp_path) as con:
        admitted = F.admit('distributions', spec, con)
        row, = list(F.iter_rows(admitted, 'distributions.parquet', con))
        assert row['amount'] == .25 and row['indicated_annual_dividend_rate'] is None
        assert not admitted['source_quality_certified']
        with open(original, 'ab') as stream:
            stream.write(b' changed')
        with pytest.raises(ValueError, match='dependency changed'):
            F.admit('distributions', spec, con)
