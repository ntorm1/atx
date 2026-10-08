"""Offline fixture pipeline catches publication/SQL/clock boundary regressions."""
import datetime as dt
import hashlib

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C, ea_actuals as EA, earnings_calendar as EC
from atx_db.alpha_panel import earnings_calendar_v2 as EV, events_sources as ES
from atx_db.alpha_panel import ea_publication as P


def publish(root, stage, name, rows, schema=None):
    path = root/stage/name
    path.parent.mkdir(parents=True,exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows,schema=schema),path)
    C.write_json_atomic(root/stage/'manifest.json',{'status':'complete','files':C.output_hashes(root/stage)})
    return path


def test_cached_parse_calendar_features_no_future_clock(tmp_path, monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    blob = b'''<html><p>USD in millions, except per-share amounts</p><table>
    <tr><th>Three months ended</th></tr><tr><td></td><td>March 31, 2020</td><td>March 31, 2019</td></tr>
    <tr><td>Net revenues</td><td>120</td><td>100</td></tr>
    <tr><td>Net income</td><td>12</td><td>10</td></tr>
    <tr><td>Diluted earnings per share</td><td>.50</td><td>.40</td></tr></table></html>'''.replace(b'>.50<',b'>0.50<').replace(b'>.40<',b'>0.40<')
    at = dt.datetime(2020,4,30,18)
    filings = [{'cik':1,'accession':'ann','form':'8-K','filing_date':at.date(),'available_at':at,
                'acceptance_utc':at,'acceptance_clock':'file_utc','vintage_risk':None,'is_amendment':False,'report_date':None},
               {'cik':1,'accession':'q','form':'10-Q','filing_date':dt.date(2020,5,1),'available_at':dt.datetime(2020,5,1,18),
                'acceptance_utc':dt.datetime(2020,5,1,18),'acceptance_clock':'file_utc','vintage_risk':None,
                'is_amendment':False,'report_date':dt.date(2020,3,31)}]
    publish(tmp_path,'sec_filings','filings.parquet',filings)
    publish(tmp_path,'sec_filings','eight_k_items.parquet',[{'cik':1,'accession':'ann','item':'2.02'}])
    publish(tmp_path,'sec_filings','issuer_profile.parquet',[{'cik':1,'fiscal_year_end':'1231'}])
    v1 = {f.name:None for f in EC.OUT_SCHEMA}
    v1.update(cik=1,accession='ann',form='8-K',filing_date=at.date(),announcement_utc=at,
              available_at=at,acceptance_clock='file_utc',fiscal_period_end=dt.date(1999,1,1))
    publish(tmp_path,'earnings_calendar','announcements.parquet',[v1],EC.OUT_SCHEMA)
    sessions = [dt.date(2020,4,30),dt.date(2020,5,1),dt.date(2020,5,4)]
    pq.write_table(pa.table({'session_date':pa.array(sessions,type=pa.date32())}),tmp_path/'calendar.parquet')
    publish(tmp_path,'panel','year=2020/part.parquet',[{'session_date':d,'security_id':10,'cik':1,'member_equity':True} for d in sessions])
    publish(tmp_path,'prices','year=2020/part.parquet',[{'session_date':d,'security_id':10,'ret':.01,'ret_guarded':False} for d in sessions])
    catalog = publish(tmp_path,'catalog','part.parquet',[{'cik':1,'accession':'ann','form':'8-K','is_amendment':False,
                    'source_doc':'ex99.htm','doc_sha256':hashlib.sha256(blob).hexdigest(),'filing_date':at.date(),
                    'doc_source':'landed_v2','qids':[]}])
    monkeypatch.setattr(ES,'catalog_path',lambda:catalog)
    monkeypatch.setattr(ES,'open_store',lambda:object())
    monkeypatch.setattr(ES,'read_document',lambda row,store:blob)
    partial=EA.parse_stage(limit_new_documents=0)
    assert partial['status']=='partial_cache_only' and partial['published'] is False
    assert not (tmp_path/'ea_actuals/manifest.json').exists()
    parsed = EA.parse_stage()
    assert parsed['rows']==1 and parsed['qualified_all']==1
    parse_sha = C.sha256_file(tmp_path/'ea_actuals/releases_manifest.json')
    EV.phase_parse()
    EV.build_stage()
    calendar = pq.read_table(P.file('earnings_calendar_v2','announcements.parquet')).to_pylist()
    assert len(calendar)==1 and calendar[0]['fiscal_period_end']==dt.date(2020,3,31)
    result = EA.build_features()
    assert result['filing_lag_rows']==1
    returns = pq.read_table(P.file('ea_actuals','reaction_returns.parquet')).to_pylist()
    assert returns[0]['ea_ret_3d']>0
    assert returns[0]['available_at']==dt.datetime(2020,5,4,22)
    lag = pq.read_table(P.file('ea_actuals','filing_lags.parquet')).to_pylist()[0]
    assert lag['ea_lag_days']==1 and lag['available_at']==dt.datetime(2020,5,1,18)
    assert C.sha256_file(tmp_path/'ea_actuals/releases_manifest.json')==parse_sha
    first=C.read_json(tmp_path/'ea_actuals/manifest.json')
    old_generation=first['generation']
    old_outputs={name:C.sha256_file(tmp_path/'ea_actuals'/name) for name in first['files']}
    old_price=first['input_manifests_sha256']['prices']['sha256']
    publish(tmp_path,'prices','year=2020/part.parquet',[{'session_date':d,'security_id':10,'ret':.02,'ret_guarded':False} for d in sessions])
    EA.build_features()
    second=C.read_json(tmp_path/'ea_actuals/manifest.json')
    assert second['input_manifests_sha256']['prices']['sha256']!=old_price
    assert second['input_manifests_sha256']['prices']['sha256']==C.sha256_file(tmp_path/'prices/manifest.json')
    assert second['parse_lineage']['sha256']==parse_sha
    assert second['parse_input_manifests_sha256']==first['parse_input_manifests_sha256']
    pointer=(tmp_path/'ea_actuals/manifest.json').read_bytes()
    with monkeypatch.context() as m:
        def interrupted(*args): raise RuntimeError('injected interruption')
        m.setattr(EA,'build_reactions',interrupted)
        with pytest.raises(RuntimeError,match='injected interruption'): EA.build_features()
    assert (tmp_path/'ea_actuals/manifest.json').read_bytes()==pointer
    EA.verify_input('ea_actuals',required_phase=('features',))
    # A parse switches phase without certifying any older feature products.
    EA.parse_stage()
    parsed_manifest=C.read_json(tmp_path/'ea_actuals/manifest.json')
    assert set(parsed_manifest['products'])=={'releases.parquet','precision_sample.parquet'}
    with pytest.raises(RuntimeError,match='phase'):
        EA.verify_input('ea_actuals',required_phase=('features',))
    with pytest.raises(RuntimeError,match='phase'):
        P.file('ea_actuals','ann_features.parquet')
    assert all(C.sha256_file(tmp_path/'ea_actuals'/name)==sha for name,sha in old_outputs.items())
    P.activate('ea_actuals',old_generation)
    EA.verify_input('ea_actuals',required_phase=('features',))
    assert C.read_json(tmp_path/'ea_actuals/manifest.json')['generation']==old_generation
    source=(tmp_path/'sec_filings/manifest.json').read_bytes()
    changed=C.read_json(tmp_path/'sec_filings/manifest.json'); changed['snapshot']='changed'
    C.write_json_atomic(tmp_path/'sec_filings/manifest.json',changed)
    with pytest.raises(RuntimeError,match='SEC filing snapshots incompatible'): EA.build_features()
    (tmp_path/'sec_filings/manifest.json').write_bytes(source)
    # FPI admission checks identity and classifier, not just physical file hash.
    C.write_json_atomic(EV._tmp()/'fpi_parse_receipt.json',{**C.read_json(EV._tmp()/'fpi_parse_receipt.json'),'classifier_version':'obsolete'})
    with pytest.raises(RuntimeError,match='stale FPI'): EV.build_stage()


def test_missing_calendar_clock_never_promotes_primary():
    row = {f.name:None for f in EC.OUT_SCHEMA}
    row.update(cik=1,accession='x',source='8k_202',fiscal_period_end=dt.date(2020,3,31),
               announcement_utc=dt.datetime(2020,4,30,18),available_at=dt.datetime(2020,4,30,18),
               acceptance_clock='unresolved_conservative')
    result = EV._merge_cik([row],EC.SessionCalendar([dt.date(2020,4,30)]))[0]
    assert result['available_at'] is None and result['announcement_utc'] is None
    assert not result['is_primary'] and result['timing']=='unknown'


def test_fiscal_label_cannot_use_latest_fiscal_calendar():
    result = EV.classify_release('Third quarter 2020 financial results',dt.date(2020,11,1),9)
    assert result['period_end'] is None and result['period_basis']=='label_fiscal_calendar_unqualified'
