"""Offline original SEC-shaped fixtures; never qualify actual coverage."""
import datetime as dt
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C, filing_text as FT, text_features as TF, tnic as LT
from atx_db.alpha_panel import filing_text_native as N, text_features_native as P, tnic_native as T


@pytest.fixture
def native(tmp_path):
    root=tmp_path/'sources'
    root.mkdir()
    members=[]
    originals=[]
    submissions=[]
    proofs=[]
    for cik in (1,2):
        rows={k:[] for k in ('accessionNumber','form','filingDate','reportDate','primaryDocument','acceptanceDateTime')}
        for year in (2018,2019,2020):
            accession=f'{cik:010d}-{year%100:02d}-000001'
            for key,value in dict(accessionNumber=accession,form='10-K',filingDate=f'{year}-03-01',reportDate=f'{year-1}-12-31',primaryDocument='annual.htm',acceptanceDateTime=f'{year}-03-01T12:00:00Z').items():
                rows[key].append(value)
            name=f'{cik}-{year}.htm'
            sentence='We manufacture specialized electronic products for commercial industrial customers. '
            body='\n'.join(['Item 1. Business',sentence*80,'Item 1A. Risk Factors',sentence*80,"Item 7. Management Discussion and Analysis",sentence*80,'Item 8. Financial Statements'])
            (root/name).write_text('<html><body><pre>'+body+'</pre></body></html>')
            url=FT.document_url(cik,accession,'annual.htm')
            receipt=f'{cik}-{year}.receipt.json'
            identity=N.pin(root/name)
            (root/receipt).write_text(json.dumps(dict(http_status=200,error=None,url=url,accession=accession,**identity)))
            originals.append(dict(cik=cik,accession=accession,path=name,receipt=receipt,url=url))
            index_path=f'{cik}-{year}.index.htm'
            (root/index_path).write_text(f'<html><div>Accepted</div><div>{year}-03-01 07:00:00</div></html>')
            index_receipt=f'{cik}-{year}.index.receipt.json'
            index_url=f"{FT.ARCHIVES}/{cik}/{accession.replace('-','')}/{accession}-index.htm"
            (root/index_receipt).write_text(json.dumps(dict(http_status=200,error=None,url=index_url,**N.pin(root/index_path))))
            proofs.append(dict(cik=cik,accession=accession,index_path=index_path,receipt=index_receipt))
        path=f'{cik}.json'
        (root/path).write_text(json.dumps(dict(cik=cik,filings=dict(recent=rows))))
        receipt=f'{cik}.submissions.receipt.json'
        url=f'https://data.sec.gov/submissions/CIK{cik:010d}.json'
        (root/receipt).write_text(json.dumps(dict(http_status=200,error=None,url=url,**N.pin(root/path))))
        submissions.append(dict(cik=cik,path=path,receipt=receipt,url=url))
        for year in (2019,2020):
            members.append(dict(session_date=dt.date(year,6,30),security_id=cik,cik=cik,member_equity=True))
    pq.write_table(pa.Table.from_pylist(members),root/'members.parquet')
    pq.write_table(pa.Table.from_pylist([dict(security_id=c,cik=c,valid_from=dt.date(2018,1,1),valid_to=dt.date(2026,12,31),link_tier='native') for c in (1,2)]),root/'identity.parquet')
    review=dict(complete_submissions_census=True,complete_native_members=True,accepted_identity=True,original_sec_records=True,free_commercial_use=True)
    for kind in ('submissions','members','identity'):
        name=kind+'-evidence.json'
        (root/name).write_text(json.dumps({'review':'synthetic offline evidence only'}))
        review[kind+'_evidence']=name
    (root/'native-manifest.json').write_text(json.dumps({'synthetic_native_complete_population':True}))
    plan=dict(schema='atx.text-native-source-plan/v1',runtime=N.runtime_identity(),acceptance_proofs=proofs,member_frequency='monthly',input_manifests_sha256={'native-manifest.json':N.pin(root/'native-manifest.json')['sha256']},source_review=review,members=['members.parquet'],identity=['identity.parquet'],submissions=submissions,originals=originals,
              expected_member_cells={'2019':2,'2020':2},expected_filing_records={'2018':2,'2019':2,'2020':2},
              files={p.name:N.pin(p) for p in root.iterdir()},code={str(Path(m.__file__).resolve()):N.pin(m.__file__) for m in (C,FT,TF,N,P,T,LT)})
    return root,tmp_path/'output',plan


def test_full_replay_and_gate_denominators(native):
    root,output,plan=native
    generation=P.publish_native(root,output,plan,N.digest(plan),unit_size=2)
    manifest=N.pin(generation/'manifest.json')['sha256']
    admission=P.read_native_generation(root,generation,manifest,N.digest(plan))
    rows=list(P.iter_native_rows(admission,'text/features.parquet'))
    assert len(rows)==6
    current=[r for r in rows if r['filing_date'].year==2020]
    assert all(r['business_sim_cosine']==pytest.approx(1) for r in current)
    assert all(r['doc_words']>0 and r['doc_fog']>0 for r in current)
    coverage=next(r for r in admission['coverage'] if r['year']==2020)
    assert coverage['member_cells']==2
    assert coverage['linked_10k_filers']==2
    assert coverage['text_gate']=='PASS'
    assert coverage['tnic_gate']=='FAIL'
    peers=list(P.iter_native_rows(admission,'classification_tnic/pairs.parquet'))
    assert not peers  # Missing SIC vintage never invents calibration.
    assert admission['status']=='FAIL'
    schema=pq.read_schema(generation/'text/features.parquet')
    assert pa.types.is_timestamp(schema.field('available_at').type)
    assert schema.field('available_at').type.tz=='UTC'
    assert pa.types.is_date(schema.field('prior_filing_date').type)
    assert pa.types.is_floating(schema.field('business_fog').type)


def test_missing_original_does_not_shrink_population(native):
    root,output,plan=native
    plan['originals']=[r for r in plan['originals'] if not (r['cik']==2 and r['accession'].split('-')[1]=='20')]
    generation=P.publish_native(root,output,plan,N.digest(plan))
    manifest=json.loads((generation/'manifest.json').read_text())
    coverage=next(r for r in manifest['coverage'] if r['year']==2020)
    assert coverage['member_cells']==2 and coverage['linked_10k_filers']==2
    assert coverage['text_cells']==1
    rows=pq.read_table(generation/'text/features.parquet').to_pylist()
    assert any(r['reason']=='original_primary_bytes_missing' for r in rows)


def test_interrupted_resume_is_authenticated(native):
    root,output,plan=native
    with pytest.raises(InterruptedError):
        P.publish_native(root,output,plan,N.digest(plan),unit_size=2,interrupt_after_units=1)
    assert not (output/'active.json').exists()
    checkpoint=next((output/'pending').glob('*/checkpoint.json'))
    with pytest.raises(ValueError,match='authenticated'):
        P.publish_native(root,output,plan,N.digest(plan),unit_size=2)
    generation=P.publish_native(root,output,plan,N.digest(plan),unit_size=2,expected_checkpoint_sha256=N.pin(checkpoint)['sha256'])
    assert generation.exists()
