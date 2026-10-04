import datetime as dt
import hashlib
import json
from dataclasses import asdict

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel import mna_publish_v2 as P
from atx_db.alpha_panel import terminal_mna_v2 as T


def inputs():
    clock = dt.datetime(2020, 1, 3, 22, tzinfo=T.UTC)
    blob = b'<p>On January 3, 2020, Acme completed the merger with Buyer. each share of Acme common stock was converted into the right to receive USD12 in cash. On January 3, 2020, the cash consideration was paid.</p>'
    pin = N.NativePin("synthetic", hashlib.sha256(blob).hexdigest(), "0000000001-20-000001", 1, clock, clock, "retrieval_floor")
    candidates = [T.Candidate(1, dt.date(2020, 1, 3), clock), T.Candidate(2, dt.date(2020, 1, 3), clock, continued=True)]
    sink = pa.BufferOutputStream()
    pq.write_table(pa.Table.from_pylist([asdict(row) for row in candidates]), sink)
    candidate_original = sink.getvalue().to_pybytes()
    key = f"1/2020-01-03/{clock.isoformat()}"
    manifests = {"delisting/v1": b'{"fixture":"independently-pinned"}'}
    url = 'https://www.sec.gov/Archives/edgar/data/1/000000000120000001/filing.htm'
    receipt = P._json({'url': url, 'sha256': pin.sha256, 'status': 200, 'fetched_at': str(clock), 'bytes': len(blob), 'error': None})
    receipts = {pin.source_id: receipt}
    contract = P._json({"schema": P.SCHEMA, "admission_id": "independent-synthetic-fixture",
                        "expected_code": P.publisher_code_pins(), "candidate_inventory": [asdict(row) for row in candidates],
                        "source_pins": [asdict(pin)], "input_manifests_sha256": {name: hashlib.sha256(value).hexdigest() for name,value in manifests.items()},
                        "candidate_source_links": {key: [pin.source_id]}, "gate_handcheck": "unmeasured",
                        "candidate_original_sha256": hashlib.sha256(candidate_original).hexdigest(),
                        "candidate_clock_rule": "legacy-stage-documented-UTC",
                        "exact_admissions": {}, "auxiliary_clock_inventory": {},
                        "source_receipts": {pin.source_id: {"sha256": hashlib.sha256(receipt).hexdigest(), "url": url}}})
    return contract, hashlib.sha256(contract).hexdigest(), {pin.source_id: blob}, (pin,), candidate_original, manifests, receipts


def test_immutable_value_only_publish_native_replay_and_resume(tmp_path):
    contract, sha, objects, pins, original, manifests, receipts = inputs()
    kw = {"candidate_original": original, "input_manifests": manifests, "receipts": receipts}
    directory = P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw)
    rows = pq.read_table(directory / "events.parquet").to_pylist()
    assert len(rows) == 2 and all(row["dlret"] is None for row in rows)
    assert rows[1]["legacy_continued"] and "legacy_continuation_unqualified" in rows[1]["reason"]
    assert P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw) == directory
    P.validate_publication(directory, contract, sha, objects, pins, **kw)
    manifest = json.loads((directory/"manifest.json").read_text())
    assert manifest["gate_status"]["exact_dlret"]["status"] == "FAIL"


def test_no_selected_candidate_inventory_or_self_manifest_hash_receipt():
    contract, sha, objects, pins, original, manifests, receipts = inputs()
    value = json.loads(contract)
    value["candidate_inventory"] = value["candidate_inventory"][:1]
    changed = P._json(value)
    with pytest.raises(ValueError, match="population replay"):
        P.build_rows(changed, hashlib.sha256(changed).hexdigest(), objects, pins, candidate_original=original, input_manifests=manifests, receipts=receipts)
    with pytest.raises(ValueError, match="input-manifest"):
        P.build_rows(contract, sha, objects, pins, candidate_original=original, input_manifests={"delisting/v1": b"manufactured"}, receipts=receipts)


def test_updated_output_hash_still_cannot_certify_manufactured_rows(tmp_path):
    contract, sha, objects, pins, original, manifests, receipts = inputs()
    kw = {"candidate_original": original, "input_manifests": manifests, "receipts": receipts}
    directory = P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw)
    path = directory / "events.parquet"
    table = pq.read_table(path)
    data = table.to_pylist(); data[0]["dlret_exact"] = True
    pq.write_table(pa.Table.from_pylist(data, schema=table.schema), path)
    manifest = json.loads((directory/"manifest.json").read_text())
    manifest["files"][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (directory/"manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="native output replay"):
        P.validate_publication(directory, contract, sha, objects, pins, **kw)


def test_conditional_exact_native_path_publishes_and_publicly_replays(tmp_path):
    from tests.test_mna_exact_v2 import fixture
    args, exact_kw, expected = fixture(stock=True)
    candidate, document, docpin, vendor, target_pin, economic, economic_pin = args
    sink = pa.BufferOutputStream(); pq.write_table(pa.Table.from_pylist([asdict(candidate)]), sink)
    original = sink.getvalue().to_pybytes()
    key = f"{candidate.security_id}/{candidate.last_session}/{candidate.available_at.isoformat()}"
    url = 'https://www.sec.gov/Archives/edgar/data/1/000000000120000001/filing.txt'
    receipt = P._json({'url': url, 'sha256': docpin.sha256, 'status': 200,
                      'fetched_at': str(docpin.fetched_at), 'bytes': len(document), 'error': None})
    manifests = {'legacy-candidate': b'original-manifest-fixture'}
    contract = P._json({'schema': P.SCHEMA, 'admission_id': 'independent-original-fixture',
                        'expected_code': P.publisher_code_pins(), 'candidate_inventory': [asdict(candidate)],
                        'source_pins': [asdict(docpin)], 'candidate_clock_rule': 'legacy-stage-documented-UTC',
                        'candidate_original_sha256': hashlib.sha256(original).hexdigest(),
                        'candidate_source_links': {key: [docpin.source_id]}, 'gate_handcheck': 'unmeasured',
                        'input_manifests_sha256': {name: hashlib.sha256(blob).hexdigest() for name,blob in manifests.items()},
                        'source_receipts': {docpin.source_id: {'sha256': hashlib.sha256(receipt).hexdigest(), 'url': url}},
                        'exact_admissions': {key: expected}, 'auxiliary_clock_inventory': {}})
    exact_inputs = {key: {'vendor': vendor, 'target_pin': target_pin, 'target_economic': economic,
                         'target_economic_pin': economic_pin, **exact_kw}}
    kw = {'candidate_original': original, 'input_manifests': manifests, 'receipts': {docpin.source_id: receipt},
          'exact_inputs': exact_inputs}
    sha = hashlib.sha256(contract).hexdigest()
    directory = P.publish_value_only(tmp_path, contract, sha, {docpin.source_id: document}, (docpin,), **kw)
    rows = pq.read_table(directory/'events.parquet').to_pylist()
    assert rows[0]['proceeds_per_share'] == 15 and rows[0]['dlret'] == .5 and rows[0]['dlret_exact']
    assert rows[0]['legacy_continued']  # native conversion resolves heuristic without denominator removal
    P.validate_publication(directory, contract, sha, {docpin.source_id: document}, (docpin,), **kw)


def test_completed_pending_build_resumes_without_overwrite(tmp_path):
    contract, sha, objects, pins, original, manifests, receipts = inputs()
    kw = {'candidate_original': original, 'input_manifests': manifests, 'receipts': receipts}
    directory = P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw)
    pending = directory.with_name(directory.name+'.partial')
    assert directory.parent.resolve() == pending.parent.resolve() == tmp_path.resolve()
    directory.rename(pending)
    assert P.publish_value_only(tmp_path, contract, sha, objects, pins, **kw) == directory
    assert directory.exists() and not pending.exists()


def test_manual_correct_boolean_is_not_native_review_evidence():
    blob = P._json({'schema': 'atx.mna.native-handcheck/v1', 'reviewer': 'independent-fixture',
                    'records': [{'candidate_key': 'candidate', 'correct': True}]})
    pin = {'sha256': hashlib.sha256(blob).hexdigest(), 'candidate_keys': ['candidate']}
    with pytest.raises(ValueError, match='booleans/counts'):
        P._review_counts(pin, blob, {'candidate'}, {}, {})
