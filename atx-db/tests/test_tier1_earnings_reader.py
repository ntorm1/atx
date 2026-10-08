"""Native parser and original population receipt replay on retained bytes only."""
import datetime as dt
from collections import Counter
import hashlib
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import ea_actuals as E, private_tier1_earnings as T


def test_native_parser_clock_and_missing_document_population(tmp_path):
    root=tmp_path;(root/'sec_filings').mkdir()
    fields={'cik':[1,2],'accession':['a','b'],'filing_date':[dt.date(2021,5,1)]*2,
        'available_at':pa.array([dt.datetime(2021,5,1,20),dt.datetime(2021,5,1,20)],type=pa.timestamp('us')),
        'acceptance_clock':['sec_acceptance_utc','unresolved'], 'vintage_risk':['','unresolved'],
        'is_amendment':[False,False],'form':['8-K','8-K']}
    pq.write_table(pa.table(fields),root/'sec_filings/filings.parquet')
    pq.write_table(pa.table({'cik':[1,2],'accession':['a','b'],'item':['2.02','2.02']}),root/'sec_filings/eight_k_items.parquet')
    blob=b'<html><p>Results for the quarter ended March 31, 2021. Revenue was $12 million. Net income was $2 million.</p></html>'
    sha=hashlib.sha256(blob).hexdigest()
    catalog=[{'cik':1,'accession':'a','form':'8-K','is_amendment':False,'doc_sha256':sha,'source_doc':'a.htm'},
        {'cik':2,'accession':'b','form':'8-K','is_amendment':False,'doc_sha256':'missing','source_doc':'b.htm'}]
    with duckdb.connect(str(tmp_path/'parser.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        rows=list(T.parsed_rows(con,root,catalog,{sha:blob,'missing':None}))
    expected=E.parse_release(blob,dt.date(2021,5,1));actual=E.merge_extracts([expected])
    for field in E.FIELDS: assert rows[0].get(field)==actual.get(field)
    assert rows[0]['available_at']==E.qualified_clock(dict(zip(fields, [fields[k][0].as_py() if isinstance(fields[k],pa.Array) else fields[k][0] for k in fields])))
    assert rows[1]['available_at'] is None and rows[1]['reason']=='no_cached_document'
    assert rows[1]['_missing_objects']==1


def test_streamed_population_receipt_and_sample_match_complete_native_order():
    rows=[{'cik':index%7,'accession':str(index),'filing_date':dt.date(2021,1,1),
        'reason':'no_cached_document','_missing_objects':int(index%3==0)} for index in range(350)]
    evidence=T.ParseEvidence()
    assert list(evidence.observe(iter(rows)))==rows
    expected=sorted(rows,key=lambda row:hashlib.sha256((str(row['cik'])+'/'+row['accession']+'/t9-precision-200-v1').encode()).hexdigest())[:200]
    assert evidence.selected()==expected
    assert evidence.rows==350 and len(evidence.sample)==200
    assert evidence.reasons['missing_or_corrupt_object']==sum(row['_missing_objects'] for row in rows)


def test_receipt_rejects_omitted_missing_sources_and_sample_key():
    rows=[{'cik':1,'accession':'a','reason':'no_cached_document','_missing_objects':1}]
    sample=rows
    receipt={'rows':1,'qualified_any':0,'qualified_all':0,'reasons':{'no_cached_document':1,'missing_or_corrupt_object':1},
        'precision_sample':{'rows':1,'selection':'lowest SHA256(cik/accession/t9-precision-200-v1), all original 2.02 releases 2020-2025, independent of extraction success',
            'key_sha256':hashlib.sha256(b'1/a').hexdigest(),'manual_reviewed':0}}
    T.check_parse_receipt(receipt,rows,sample)
    receipt['reasons'].pop('missing_or_corrupt_object')
    with pytest.raises(ValueError,match='population counters'):T.check_parse_receipt(receipt,rows,sample)


def test_full_original_audit_equation_equivalence_all_populations_and_years(tmp_path,monkeypatch):
    from atx_db.alpha_panel import common as C,compustat_std as S,ea_publication as P
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    calendar=[];feature_rows=[];ann=[];fund=[];cov=[]
    for year in range(2020,2026):
        session=dt.date(year,1,3);prior=dt.date(year,1,2);period=dt.date(year-1,12,31)
        calendar.extend([prior,session]);clock=dt.datetime(year,1,1,20)
        feature_rows.append({'cik':1,'period_end':period,'sue_eps_ann':.1,'sue_rev_ann':.2,'available_at':clock})
        ann.append({'cik':1,'session_date':session,'source':'8k_202','is_primary':True,
            'announcement_utc':clock,'available_at':clock,'fiscal_period_end':period})
        fund.append({'cik':1,'accession':str(year),'clock_basis':'fsds_accepted_utc'})
        cov.append({'cik':1,'accession':str(year),'year':year,'datadate':period,'filed':session,
            'clock_utc':clock,'available_at':clock,'form':'10-Q'})
        for stage in ('panel','universe'):
            dest=tmp_path/stage/f'year={year}'/'data.parquet';dest.parent.mkdir(parents=True)
            pq.write_table(pa.table({'session_date':[session],'security_id':[1],'cik':[1],
                'member_equity':[True],'univ_all_common':[True],'univ_r3000p':[True]}),dest)
    pq.write_table(pa.table({'session_date':calendar}),tmp_path/'calendar.parquet')
    for stage in ('ea_actuals','earnings_calendar_v2','compustat_std','fundamentals_v11'):(tmp_path/stage).mkdir()
    features=tmp_path/'ea_actuals'
    pq.write_table(pa.Table.from_pylist(feature_rows),features/'ann_features.parquet')
    for name in ('filing_lags.parquet','reaction_returns.parquet'):pq.write_table(pa.table({'x':pa.array([],type=pa.int64())}),features/name)
    pq.write_table(pa.Table.from_pylist(ann),tmp_path/'earnings_calendar_v2/announcements.parquet')
    pq.write_table(pa.Table.from_pylist(fund),tmp_path/'fundamentals_v11/events.parquet')
    C.write_json_atomic(tmp_path/'compustat_std/manifest.json',{'fund_stage':'fundamentals_v11'})
    C.write_json_atomic(features/'manifest.json',{'gate_status':{},'phase':'features'})
    C.write_json_atomic(tmp_path/'earnings_calendar_v2/manifest.json',{'products':{'announcements.parquet':'announcements.parquet'}})
    monkeypatch.setattr(E,'verify_input',lambda stage,*a,**kw:{'path':str(tmp_path/stage/'manifest.json'),'sha256':'0'*64})
    monkeypatch.setattr(S,'member_ciks',lambda con:None)
    def coverage(con,out):
        con.register('fixture_cov',pa.Table.from_pylist(cov));con.execute('CREATE OR REPLACE TEMP TABLE cov AS SELECT * FROM fixture_cov');return {}
    monkeypatch.setattr(S,'coverage_gate',coverage)
    original_connect=C.connect
    def bounded(*args,**kwargs):return original_connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='audit.duckdb')
    monkeypatch.setattr(C,'connect',bounded)
    native=E.audit_stage()
    with original_connect(memory='128MB',threads=1,temp_dir=tmp_path/'reader-scratch',db_file='reader.duckdb') as con:
        actual=T.audit_result(con,tmp_path,features,{'products':{'fundq':tmp_path/'compustat_std/fundq.parquet',
            'qualified_fundamental_leaf':tmp_path/'fundamentals_v11'}},native['input_integrity'])
    assert native==actual
    assert len(actual['coverage'])==18 and set(actual['rdq'])=={str(y) for y in range(2020,2026)}
    assert actual['gate_status']['precision_200_manual']=='FAIL_unreviewed'
