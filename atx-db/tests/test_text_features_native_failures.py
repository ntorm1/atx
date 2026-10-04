"""Focused offline source, clock and complete-replay failure fixtures."""
import json

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C, filing_text_native as N, text_features_native as P
from tests.test_text_features_native import native


def test_changed_source_and_orphan_reject(native):
    root,output,plan=native
    generation=P.publish_native(root,output,plan,N.digest(plan))
    product=generation/'text/features.parquet'
    table=pq.read_table(product)
    rows=table.to_pylist()
    rows[0]['cik']=999999
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),product)
    manifest_path=generation/'manifest.json'
    manifest=json.loads(manifest_path.read_text())
    manifest['files']['text/features.parquet']=N.pin(product)
    C.write_json_atomic(manifest_path,manifest)
    sha=N.pin(generation/'manifest.json')['sha256']
    with pytest.raises(ValueError,match='replay differs'):
        P.read_native_generation(root,generation,sha,N.digest(plan))
    (generation/'orphan.json').write_text('{}')
    with pytest.raises(ValueError,match='orphan'):
        P.read_native_generation(root,generation,sha,N.digest(plan))
    (root/'1-2020.htm').write_text('changed original')
    with pytest.raises(ValueError,match='pinned source'):
        P.read_native_generation(root,generation,sha,N.digest(plan))


def test_source_review_and_clocks_reject(native):
    root,output,plan=native
    plan['source_review']['complete_submissions_census']=False
    with pytest.raises(ValueError,match='unqualified'):
        P.publish_native(root,output,plan,N.digest(plan))
    assert N.acceptance('2020-03-01T12:00:00') is None
    assert N.acceptance('2020-03-01T12:00:00-05:00').hour==17


def test_bad_receipt_rejects_even_matching_new_plan(native):
    root,output,plan=native
    path=root/'1-2020.receipt.json'
    receipt=json.loads(path.read_text())
    receipt['url']='https://example.invalid/fake'
    path.write_text(json.dumps(receipt))
    plan['files'][path.name]=N.pin(path)
    with pytest.raises(ValueError,match='receipt identity'):
        P.publish_native(root,output,plan,N.digest(plan))


def test_unknown_clock_and_amendment_inventory_remain_missing(native):
    root,output,plan=native
    path=root/'2.json'
    source=json.loads(path.read_text())
    rows=source['filings']['recent']
    rows['acceptanceDateTime'][2]='2020-03-01T12:00:00'
    for key,value in dict(accessionNumber='0000000002-20-000002',form='10-K/A',filingDate='2020-04-01',reportDate='2019-12-31',primaryDocument='amend.htm',acceptanceDateTime='2020-04-01T12:00:00Z').items():
        rows[key].append(value)
    path.write_text(json.dumps(source))
    receipt_path=root/'2.submissions.receipt.json'
    receipt=json.loads(receipt_path.read_text())
    receipt.update(N.pin(path))
    receipt_path.write_text(json.dumps(receipt))
    plan['files'][path.name]=N.pin(path)
    plan['files'][receipt_path.name]=N.pin(receipt_path)
    plan['expected_filing_records']['2020']=3
    plan['acceptance_proofs']=[p for p in plan['acceptance_proofs'] if not (p['cik']==2 and p['accession'].split('-')[1]=='20')]
    generation=P.publish_native(root,output,plan,N.digest(plan))
    records=pq.read_table(generation/'text/features.parquet').to_pylist()
    unknown=next(r for r in records if r['reason']=='unknown_acceptance_clock')
    amendment=next(r for r in records if r['reason']=='amendment_excluded')
    assert unknown['available_at'] is None and unknown['doc_words'] is None
    assert amendment['doc_words'] is None and amendment['business_sim_cosine'] is None
    assert len(records)==7
