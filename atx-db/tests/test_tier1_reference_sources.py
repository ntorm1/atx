"""Synthetic full French originals/publication; actual source gates stay open."""
import datetime as dt
import hashlib
import json
import duckdb
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import private_tier1_reference_sources as S
from atx_db.alpha_panel import private_tier1_reference_replay as N
from atx_db.alpha_panel import private_tier1_admission as A, reference as R


def test_complete_french_publication_reader_replay_and_whole_consumption(tmp_path):
    root=tmp_path/'originals';root.mkdir();legacy=tmp_path/'legacy';legacy.mkdir()
    records=[];members=[];products={};schemes={};clock=dt.datetime(2026,2,1,tzinfo=dt.UTC)
    sources={name:key for key,name in R.FRENCH_FACTOR_FILES.items()}
    sources.update({f'Siccodes{scheme}.zip':scheme for scheme in R.SICCODE_SCHEMES})
    for name,key in sources.items():
        directory=root/name.removesuffix('.zip');directory.mkdir()
        if isinstance(key,int):text=' 1 Test Test industry\n  1000-1999 Range\n 2 Other Other industry'
        else:
            date='20200102' if key.endswith('daily') else '202001'
            text='using the 202601 CRSP database\n,Mkt-RF,SMB,HML,RMW,CMA,RF\n'+date+',1,2,3,4,5,.01\n'
        member=directory/'original.txt';member.write_bytes(text.encode('latin-1'));members.append(member)
        records.append({'key':name,'kind':'french_zip','url':R.FRENCH_BASE+name,'http_status':200,
            'file':directory.relative_to(root).as_posix(),'members':{'original.txt':A.pin(member)},
            'fetched_at':clock.isoformat(),'zip_deleted':True,'bytes':123,
            'sha256':hashlib.sha256(('deleted original '+name).encode()).hexdigest()})
        if isinstance(key,int):schemes[key]=(text,clock)
        else:
            products[key+'.parquet']=legacy/(key+'.parquet')
            pq.write_table(N.french_table(text,clock,key),products[key+'.parquet'])
    products['siccodes.parquet']=legacy/'siccodes.parquet'
    pq.write_table(N.sic_table(schemes),products['siccodes.parquet'])
    ledger=root/'receipts.jsonl';ledger.write_text(''.join(json.dumps(row)+'\n' for row in records))
    native={str(ledger):A.pin(ledger),**A.code(('private_tier1_reference_replay','private_tier1_factor_replay',
        'private_tier1_admission','reference','classification','shortflow_common','common')),
        **{str(path):A.pin(path) for path in [*members,*products.values()]}}
    plan=tmp_path/'plan.json'
    S.C.write_json_atomic(plan,{'schema':'atx.tier1-french-source-plan/v1','source_root':str(root),
        'ledger':'receipts.jsonl','product_root':str(legacy),'native_dependency_pins':native})
    execution=tmp_path/'execution.json';S.C.write_json_atomic(execution,S.EXECUTION)
    spec={'interface_version':S.VERSION,'plan_path':str(plan),'expected_plan_sha256':A.pin(plan)['sha256'],
        'execution_contract':str(execution),'expected_execution_contract_sha256':A.pin(execution)['sha256'],
        'expected_code_sha256':A.digest(S.code_pins()),'stage_root':str(tmp_path/'private-reference')}
    def reviewed(current,path):
        S.C.write_json_atomic(path,{'schema':'atx.tier1-french-review/v1','status':'scoped_pass',
            'interface_version':S.VERSION,'binding_sha256':A.digest(S.binding(current)),'code_pins':S.code_pins()})
        current['independent_review']={'path':str(path),'expected_sha256':A.pin(path)['sha256']}
        current['dependency_pins']={**native,**S.code_pins(),str(plan):A.pin(plan),
            str(execution):A.pin(execution),str(path):A.pin(path)}
    reviewed(spec,tmp_path/'producer-review.json')
    with duckdb.connect(str(tmp_path/'reader.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        result=S.materialize(spec,con,'fixture')
        assert not result['publication'] and not result['source_quality_certified']
        assert not (tmp_path/'private-reference'/'current.json').exists()
        generation=tmp_path/'private-reference'/'generations'/'fixture'
        # Separately run original native replay to establish the test's expected
        # full validator identity; never take a publisher's asserted result SHA.
        candidate=dict(native)
        for path in products.values():candidate.pop(str(path))
        candidate.update({str(generation/name):identity for name,identity in result['product_pins'].items()})
        independent=N.replay_french_sources(con,root,ledger,
            {name:generation/name for name in S.PRODUCTS},tmp_path/'independent',candidate)
        final={**spec,'generation':str(generation),'manifest_pin':result['manifest_pin'],
            'product_pins':result['product_pins'],'expected_validation_sha256':A.digest(independent)}
        reviewed(final,tmp_path/'final-reader-review.json')
        admission=S.admit(final,con)
        assert len(admission['products'])==7 and len(admission['validation']['sources'])==14
        assert all(not row['original_zip_bytes_admitted'] for row in admission['validation']['sources'].values())
        with S.consuming(admission,con) as files:
            assert con.execute('SELECT count(*) FROM '+A.sql([files['ff5_daily.parquet']])).fetchone()[0]==1
        with pytest.raises(ValueError,match='native dependency bytes changed'):
            with S.consuming(admission,con):
                members[0].write_bytes(b'changed original during bounded consumption')
                raise RuntimeError('downstream terminated early')
        assert not (tmp_path/'private-reference'/'current.json').exists()

def test_complete_fred_original_receipts_replay_and_last_failure(tmp_path):
    from atx_db.alpha_panel import shortflow_common as C
    root=tmp_path/'fred';root.mkdir();legacy=tmp_path/'legacy';legacy.mkdir()
    clock='2026-02-01T00:00:00+00:00';records=[];members=[];series={};receipts={}
    page=''.join('/series/'+sid+'\n' for sid in sorted(R.H10))
    release=json.dumps([{'Months':[{'Dates':['20200106']}]}])
    keys=['h10_release_page','h10_release_dates',*R.all_fred_series()]
    for key in keys:
        body=(page if key=='h10_release_page' else release if key=='h10_release_dates'
            else 'observation_date,'+key+'\n2020-01-02,2\n2020-01-03,.\n2020-01-02,3\n').encode()
        path=root/(key+'.original');path.write_bytes(body);members.append(path)
        rec={'key':key,'kind':'page' if key=='h10_release_page' else 'frb_release_dates'
            if key=='h10_release_dates' else 'fred_series','http_status':200,'file':path.name,
            'url':R.RELEASE_PAGE_URL if key=='h10_release_page' else R.H10_DATES_URL
            if key=='h10_release_dates' else C.full_url(R.FRED_URL,{'id':key,'cosd':R.FRED_START}),
            **A.pin(path),'fetched_at':clock,'last_modified':None}
        if key=='h10_release_page':rec['listed_series']=sorted(R.H10)
        if key in R.all_fred_series():
            series[key]=R.parse_fred_csv(body.decode(),key);receipts[key]=rec
        records.append(rec)
    tables=N.fred_tables(series,[dt.date(2020,1,6)],receipts);products={}
    for name,table in tables.items():
        products[name]=legacy/name;pq.write_table(table,products[name])
    ledger=root/'receipts.jsonl';ledger.write_text(''.join(json.dumps(row)+'\n' for row in records))
    pins={str(ledger):A.pin(ledger),**A.code(('private_tier1_reference_replay','private_tier1_factor_replay',
        'private_tier1_admission','reference','classification','shortflow_common','common')),
        **{str(path):A.pin(path) for path in [*members,*products.values()]}}
    plan=tmp_path/'fred-plan.json'
    S.C.write_json_atomic(plan,{'schema':'atx.tier1-fred-source-plan/v1','source_root':str(root),
        'ledger':'receipts.jsonl','product_root':str(legacy),'native_dependency_pins':pins})
    execution=tmp_path/'fred-execution.json'
    S.C.write_json_atomic(execution,{**S.EXECUTION,'version':S.FRED_VERSION})
    spec={'interface_version':S.FRED_VERSION,'plan_path':str(plan),'expected_plan_sha256':A.pin(plan)['sha256'],
        'execution_contract':str(execution),'expected_execution_contract_sha256':A.pin(execution)['sha256'],
        'expected_code_sha256':A.digest(S.code_pins()),'stage_root':str(tmp_path/'private-fred')}
    def reviewed(current,path):
        S.C.write_json_atomic(path,{'schema':'atx.tier1-fred-review/v1','status':'scoped_pass',
            'interface_version':S.FRED_VERSION,'binding_sha256':A.digest(S.binding(current)),'code_pins':S.code_pins()})
        current['independent_review']={'path':str(path),'expected_sha256':A.pin(path)['sha256']}
        current['dependency_pins']={**pins,**S.code_pins(),str(plan):A.pin(plan),str(execution):A.pin(execution),str(path):A.pin(path)}
    reviewed(spec,tmp_path/'fred-producer-review.json')
    with duckdb.connect(str(tmp_path/'replay.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        result=N.replay_fred_sources(con,root,ledger,products,tmp_path/'expected',pins)
        assert len(result['sources'])==len(keys) and not result['source_quality_certified']
        assert result['validation']['vix_daily.parquet']['rows']==2
        assert tables['fx_daily.parquet'].schema.names==list(R.FX_COLUMNS)
        assert tables['vix_daily.parquet'].to_pylist()[0]['available_at']==R.vix_available_at(dt.date(2020,1,2))
        published=S.materialize(spec,con,'fixture')
        generation=tmp_path/'private-fred'/'generations'/'fixture'
        candidate=dict(pins)
        for path in products.values():candidate.pop(str(path))
        candidate.update({str(generation/name):identity for name,identity in published['product_pins'].items()})
        independent=N.replay_fred_sources(con,root,ledger,{name:generation/name for name in S.FRED_PRODUCTS},tmp_path/'independent',candidate)
        final={**spec,'generation':str(generation),'manifest_pin':published['manifest_pin'],
            'product_pins':published['product_pins'],'expected_validation_sha256':A.digest(independent)}
        reviewed(final,tmp_path/'fred-final-review.json')
        admitted=S.admit(final,con)
        with S.consuming(admitted,con) as files:
            assert con.execute('SELECT count(*) FROM '+A.sql([files['vix_daily.parquet']])).fetchone()[0]==2
        assert not (tmp_path/'private-fred'/'current.json').exists()
        with ledger.open('a') as output:output.write(json.dumps({'key':'VIXCLS','http_status':403})+'\n')
        pins[str(ledger)]=A.pin(ledger)
        with pytest.raises(ValueError,match='missing or failed: VIXCLS'):
            N.replay_fred_sources(con,root,ledger,products,tmp_path/'failed',pins)
