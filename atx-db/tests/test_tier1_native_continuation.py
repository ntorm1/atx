"""Complete original bucket sequencing with fixture kernels and actual Parquet replay."""
import datetime as dt
from pathlib import Path
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import common as C,tier1_native_readers as T,private_tier1_accounting as K,private_tier1_admission as A


def test_continuation_replays_prior_and_all32_before_publication(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path));monkeypatch.setattr(A,'guard',lambda con:None);monkeypatch.setattr(A,'execution',lambda spec:{})
    pinfile=tmp_path/'independent-input';pinfile.write_text('fixed')
    pins={str(pinfile):A.pin(pinfile)}
    source={'version':T.VERSION,'kind':'standardized_v11','dependencies':pins,'fund_stage':'qualified/fundamentals_v11',
        'earn_stage':'calendar/generations/a','build_date':'2026-10-04','buckets':K.S.N_BUCKETS,
        'qualified_fundamental_leaf':str(tmp_path/'qualified/fundamentals_v11')}
    spec={'interface_version':T.VERSION,'lake_root':str(tmp_path),'generation':'compustat_std_v11/generations/test',
        'snapshot_sha256':A.digest(source),'dependency_pins':pins,'build_date':'2026-10-04'}
    calls=[]
    monkeypatch.setattr(K,'standard_sources',lambda spec,con:(source,None,None,None,None))
    def inputs(con,spec,source,fund,earn,fx,frozen,scratch):
        pq.write_table(pa.table({'x':[1]}),scratch/'facts.parquet');return None,{'rows':1}
    monkeypatch.setattr(K,'standard_inputs',inputs)
    def compute(con,inp,bid,dest):
        calls.append(bid)
        table=pa.table({'cik':[bid],'datadate':[dt.date(2021,3,31)],'available_at':[dt.datetime(2021,5,1)],'accession':[str(bid)]})
        for product in ('fundq','funda'):pq.write_table(table,dest/(product+'.parquet'))
    monkeypatch.setattr(K,'write_standard_bucket',compute)
    monkeypatch.setattr(K,'standard_evidence',lambda *args:{'original_gate':{'pass':False}})
    generation=tmp_path/spec['generation']
    with duckdb.connect(str(tmp_path/'continuation.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        partial=T.build('standardized_v11',spec,con,max_new_units=1)
        assert partial['status']=='partial_cache_only' and len(partial['missing_units'])==31
        assert not (generation/'manifest.json').exists() and not (generation.parent.parent/'current.json').exists()
        calls.clear()
        complete=T.build('standardized_v11',spec,con)
        assert calls[0]==0 and set(calls)==set(range(32))
        assert calls.count(0)==2 and all(calls.count(b)==3 for b in range(1,32))
        assert complete['source_quality_certified'] is False and complete['published']
        manifest=C.read_json(generation/'manifest.json')
        assert len(manifest['units'])==32 and manifest['gate_status']['original_gate']=='FAIL'
        assert T.resolve_current(generation.parent.parent)==generation
        admitted={**spec,'manifest_pin':A.pin(generation/'manifest.json'),'expected_validation_sha256':A.digest({'original_gate':{'pass':False},'facts':{'rows':1}})}
        start=C.read_json(generation/'units/bucket-0000/admission.json')['attempt']
        (generation/start/'start.json').write_text('{}')
        with pytest.raises(ValueError,match='at-compute'):T.validate('standardized_v11',admitted,con)
