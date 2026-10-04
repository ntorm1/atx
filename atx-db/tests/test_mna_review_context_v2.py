"""Remaining I2: original scope cannot be removed by selected quotations."""
from dataclasses import asdict, replace
import hashlib
import json
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import mna_exact_v2 as E, mna_price_native_v2 as B, mna_publish_v2 as P
from tests.test_mna_exact_v2 import fixture


@pytest.mark.parametrize('field', ['raw_basis_quote', 'conversion_basis_quote'])
@pytest.mark.parametrize('scope', ['It is false that ', 'Only if approval is granted, ',
                                   'This claim is disputed: ', 'The statements below are false. '])
def test_i2_complete_original_basis_scope_refuses(field, scope):
    args, _, _ = fixture()
    blob, pin = args[5], args[6]
    quote = getattr(pin, field)
    changed = blob.replace(quote.encode(), (scope + quote).encode())
    # Affirmative selected quote is unchanged; independent original SHA is new.
    pin = replace(pin, sha256=hashlib.sha256(changed).hexdigest())
    with pytest.raises(ValueError, match='statement context unsupported'):
        B.read_economic(changed, pin)


@pytest.mark.parametrize('field,acquirer', [('raw_basis_quote', False), ('conversion_basis_quote', True)])
def test_i2_exact_and_public_replay_keep_scoped_basis_null(tmp_path, field, acquirer):
    args, exact_kw, expected = fixture(stock=True)
    args = list(args)
    blob = exact_kw['acquirer_economic'] if acquirer else args[5]
    pin = exact_kw['acquirer_economic_pin'] if acquirer else args[6]
    quote = getattr(pin, field)
    blob = blob.replace(quote.encode(), ('It is false that ' + quote).encode())
    pin = replace(pin, sha256=hashlib.sha256(blob).hexdigest())
    if acquirer:
        exact_kw.update(acquirer_economic=blob, acquirer_economic_pin=pin)
        expected['acquirer_economic_pin'] = E._canonical(pin)
    else:
        args[5], args[6] = blob, pin
        expected['target_economic_pin'] = E._canonical(pin)
    with pytest.raises(ValueError, match='statement context unsupported'):
        E.replay_exact(*args, expected=expected, **exact_kw)
    candidate, document, docpin, vendor, target_pin, economic, economic_pin = args
    sink = pa.BufferOutputStream()
    pq.write_table(pa.Table.from_pylist([asdict(candidate)]), sink)
    original = sink.getvalue().to_pybytes()
    key = f'{candidate.security_id}/{candidate.last_session}/{candidate.available_at.isoformat()}'
    url = 'https://www.sec.gov/Archives/edgar/data/1/000000000120000001/filing.txt'
    receipt = P._json({'url': url, 'sha256': docpin.sha256, 'status': 200,
                      'fetched_at': str(docpin.fetched_at), 'bytes': len(document), 'error': None})
    manifests = {'legacy-candidate': b'original-manifest-fixture'}
    contract = P._json({'schema': P.SCHEMA, 'admission_id': 'independent-context-fixture',
        'expected_code': P.publisher_code_pins(), 'candidate_inventory': [asdict(candidate)],
        'source_pins': [asdict(docpin)], 'candidate_clock_rule': 'legacy-stage-documented-UTC',
        'candidate_original_sha256': hashlib.sha256(original).hexdigest(),
        'candidate_source_links': {key: [docpin.source_id]}, 'gate_handcheck': 'unmeasured',
        'input_manifests_sha256': {name: hashlib.sha256(value).hexdigest() for name,value in manifests.items()},
        'source_receipts': {docpin.source_id: {'sha256': hashlib.sha256(receipt).hexdigest(), 'url': url}},
        'exact_admissions': {key: expected}, 'auxiliary_clock_inventory': {}})
    sha = hashlib.sha256(contract).hexdigest()
    kw = {'candidate_original': original, 'input_manifests': manifests,
          'receipts': {docpin.source_id: receipt}, 'exact_inputs': {key: {
          'vendor': vendor, 'target_pin': target_pin, 'target_economic': economic,
          'target_economic_pin': economic_pin, **exact_kw}}}
    objects, pins = {docpin.source_id: document}, (docpin,)
    directory = P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw)
    row, = pq.read_table(directory/'events.parquet').to_pylist()
    assert row['dlret'] is None and row['proceeds_per_share'] is None
    assert not row['dlret_exact'] and not row['dlret_imputed']
    assert any('statement context unsupported' in reason for reason in row['reason'])
    P.validate_publication(directory, contract, sha, objects, pins, **kw)
    # Updating the product hash cannot turn the refused original into exact .5.
    row.update(dlret=.5, dlret_exact=True, proceeds_per_share=15.)
    path = directory/'events.parquet'
    pq.write_table(pa.Table.from_pylist([row], schema=P.publication_schemas()['events']), path)
    manifest = json.loads((directory/'manifest.json').read_text())
    manifest['files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (directory/'manifest.json').write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match='native output replay'):
        P.validate_publication(directory, contract, sha, objects, pins, **kw)
