import datetime as dt
import json
import duckdb
import pyarrow as pa
import pytest

from atx_db.alpha_panel import factors as F
from atx_db.alpha_panel import _factor_sources as S
from atx_db.alpha_panel import _factor_native as N
from atx_db.alpha_panel._factor_io import Generation,publisher_lock


def fixture_con():
    con=duckdb.connect()
    con.execute('CREATE TABLE t6_funda(cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seq DOUBLE,ceq DOUBLE,"at" DOUBLE,lt DOUBLE,txditc DOUBLE,pstk DOUBLE,sale DOUBLE,cogs DOUBLE,xsga DOUBLE,xint DOUBLE,revt DOUBLE,xrd DOUBLE,rect DOUBLE,invt DOUBLE,xpp DOUBLE,drc DOUBLE,drlt DOUBLE,ap DOUBLE,xacc DOUBLE,dltt DOUBLE,dlc DOUBLE,sic BIGINT)')
    con.execute('CREATE TABLE t6_fundq(cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seqq DOUBLE,ceqq DOUBLE,atq DOUBLE,ltq DOUBLE,pstkq DOUBLE,ibq DOUBLE)')
    con.execute('CREATE TABLE t6_universe(session_date DATE,security_id BIGINT,cik BIGINT,me DOUBLE,exchcd_like INTEGER,univ_all_common BOOLEAN,univ_r3000p BOOLEAN)')
    con.execute('CREATE TABLE t6_prices(session_date DATE,security_id BIGINT,ret DOUBLE,ret_guarded BOOLEAN,close DOUBLE)')
    con.execute('CREATE TABLE t6_calendar(session_date DATE)')
    con.execute('CREATE TABLE t6_rf(freq VARCHAR,session_date DATE,rf DOUBLE)')
    con.execute('CREATE TABLE t6_cap_quality('+N.EMPTY['cap_quality']+')')
    con.execute('CREATE TABLE t6_eg_training('+N.EMPTY['eg_training']+')')
    return con


def test_annual_latest_visible_vintage_and_prior_year():
    con=fixture_con()
    con.execute("INSERT INTO t6_funda(cik,datadate,available_at,accession,seq,\"at\",sale,cogs) VALUES (1,'2020-12-31','2021-03-01','a',10,20,100,50),(1,'2020-12-31','2021-07-01','a',100,200,1000,500),(1,'2019-12-31','2020-03-01','b',8,10,80,40)")
    out=list(N.rows(con,S.annual_sql(dt.date(2021,6,30),dt.datetime(2021,6,30,22,tzinfo=dt.UTC))))
    assert len(out)==1
    assert out[0]['be']==10
    assert out[0]['investment']==1
    assert out[0]['op']==5
    con.close()


def test_quarterly_roe_uses_visible_previous_quarter():
    con=fixture_con()
    con.execute("INSERT INTO t6_fundq(cik,datadate,available_at,accession,seqq,ibq) VALUES(1,'2020-12-31','2021-02-01','a',10,1),(1,'2021-03-31','2021-05-01','b',20,2),(1,'2021-03-31','2021-08-01','b',200,20)")
    result=list(N.rows(con,S.quarterly_sql(dt.datetime(2021,6,1,tzinfo=dt.UTC))))
    assert result[0]['roe']==pytest.approx(.2)
    con.close()


def test_native_issuer_return_requires_independent_quality():
    con=fixture_con()
    con.execute("INSERT INTO t6_universe VALUES('2022-02-28',1,77159,711930800,1,true,true)")
    con.execute("INSERT INTO t6_prices VALUES('2022-03-01',1,.1,false,35)")
    out=N.issuer_period(con,dt.date(2022,3,1),dt.date(2022,3,1),dt.date(2022,2,28))
    assert out[0]['certified'] is False
    # Boolean assertions without independent audit/evidence/clock are insufficient.
    con.execute("INSERT INTO t6_cap_quality(security_id,session_date,available_at,certified,membership_certified) VALUES(1,'2022-02-28','2022-02-27',true,true)")
    assert N.formation_issuers(con,dt.date(2022,2,28))[0]['certified'] is False
    con.close()


def test_native_return_filter_precedes_aggregation():
    with pytest.raises(ValueError):
        S.issuer_period_sql(dt.date(2022,12,30),dt.date(2023,1,2),dt.date(2022,12,29),dt.datetime.now(dt.UTC))


def test_generation_resume_hashes_and_publisher_lock(tmp_path):
    schema=pa.schema([('x',pa.int64())])
    with publisher_lock(tmp_path):
        with pytest.raises(RuntimeError):
            with publisher_lock(tmp_path):
                pass
        g=Generation(tmp_path,'fixture',{'input':'abc'},schema)
        g.write('u1',[{'x':1}])
        assert g.done('u1')
        with pytest.raises(ValueError,match='incomplete'):
            g.publish(['u1','u2'],{})
        manifest=g.publish(['u1'],{})
        first=manifest.read_bytes()
    with publisher_lock(tmp_path):
        g2=Generation(tmp_path,'fixture',{'input':'abc'},schema)
        assert g2.done('u1')
        assert g2.publish(['u1'],{}).read_bytes()==first
        (g2.path/'u1.parquet').write_bytes(b'broken')
        with pytest.raises(ValueError,match='stale'):
            g2.done('u1')


def test_eg_training_outcomes_need_their_own_visible_clock():
    cutoff=dt.datetime(2022,1,1,tzinfo=dt.UTC)
    training=[]
    # Thirty historical fits, with independent full-rank cross-sections.
    for ordinal in range(2019*12,2019*12+30):
        month=dt.date(ordinal//12,ordinal%12+1,1)
        fit=dt.datetime.combine(month,dt.time(22),dt.UTC)
        for i,(q,c,d) in enumerate([(0,0,0),(1,0,0),(0,1,0),(0,0,1),(1,1,1),(2,3,1)]):
            training.append({'cik':i,'fit_month':month,'fit_cutoff':fit,'outcome_period_end':month-dt.timedelta(days=200),
                'outcome_available_at':fit-dt.timedelta(days=1),'predictor_available_at':fit-dt.timedelta(days=400),
                'log_q':q,'cop':c,'droe':d,'me':10,'dia':q+c+d})
    predictors=[{'cik':1,'available_at':cutoff-dt.timedelta(days=1),'log_q':1,'cop':1,'droe':1}]
    assert F.eg_forecast(training,predictors,cutoff)[1] is not None
    for r in training:
        r['outcome_available_at']=cutoff+dt.timedelta(days=1)
    assert F.eg_forecast(training,predictors,cutoff)[1] is None


def test_bab_original_history_minimum_and_shrinkage():
    import numpy as np
    x=np.sin(np.arange(1000))*.01
    assert F.bab_beta(list(x),list(x))==pytest.approx(1)
    assert F.bab_beta(list(x[:200]),list(x[:200])) is None


def test_native_produces_full_month_window_without_missing_history_backfill(tmp_path,monkeypatch):
    con=fixture_con()
    monkeypatch.setattr(N.C,'connect',lambda **kw:con)
    monkeypatch.setattr(N,'setup',lambda con,spec:{'fixture':'offline'})
    monkeypatch.setattr(N,'setup_check',lambda spec:None)
    spec={'release':{},'sources':{'eg_training':None}}
    manifest=N.native_factors(spec,tmp_path)
    obj=json.loads(manifest.read_text())
    assert len(obj['files'])==120
    assert obj['rows']==120
    assert obj['gate_status']['real_benchmark_gates']=='FAIL_unmeasured'


def test_native_risk_publishes_latest_primitives_with_missing_factor_history(tmp_path,monkeypatch):
    import hashlib
    import pyarrow.parquet as pq
    con=fixture_con()
    con.execute('CREATE TABLE t6_liquidity(security_id BIGINT,session_date DATE,available_at TIMESTAMP,adv_21 DOUBLE,adv_63 DOUBLE,spread_cs_21 DOUBLE,amihud_21 DOUBLE)')
    con.execute('CREATE TABLE t6_borrow(security_id BIGINT,session_date DATE,cutoff_utc TIMESTAMPTZ,si_to_io DOUBLE,si_shares DOUBLE,ftd_quantity DOUBLE,on_threshold_list BOOLEAN,si_to_io_available_at TIMESTAMPTZ,si_available_at TIMESTAMPTZ,ftd_available_at TIMESTAMPTZ,threshold_available_at TIMESTAMPTZ,ftd_settlement_date DATE,threshold_list_date DATE)')
    con.execute('CREATE TABLE t6_industry(cik BIGINT,available_at TIMESTAMP,ff49_no BIGINT)')
    con.execute('CREATE TABLE t6_ratios_state(cik BIGINT,available_at TIMESTAMP,be_q0 DOUBLE,at_q0 DOUBLE,at_q4 DOUBLE,debt_at DOUBLE,gprof DOUBLE,datadate DATE)')
    con.execute("INSERT INTO t6_calendar VALUES('2025-01-02'),('2025-01-03')")
    con.execute("INSERT INTO t6_universe VALUES('2025-01-03',1,1,100,1,true,true)")
    con.execute("INSERT INTO t6_liquidity VALUES(1,'2025-01-01','2025-01-01',1000000,2000000,.01,.2)")
    fdir=tmp_path/'own'
    fdir.mkdir()
    pq.write_table(pa.Table.from_pylist([],schema=N.FACTOR_SCHEMA),fdir/'f.parquet')
    fm={'stage':'factors','status':'complete','config_sha256':F.config_hash(),'files':{'f.parquet':{'bytes':(fdir/'f.parquet').stat().st_size,'sha256':hashlib.sha256((fdir/'f.parquet').read_bytes()).hexdigest()}}}
    manifest=fdir/'manifest.json'
    manifest.write_text(json.dumps(fm))
    monkeypatch.setattr(N.C,'connect',lambda **kw:con)
    monkeypatch.setattr(N,'setup',lambda con,spec:{'fixture':'offline'})
    monkeypatch.setattr(N,'setup_check',lambda spec:None)
    spec={'release':{},'own_factors':{'manifest':str(manifest),'sha256':hashlib.sha256(manifest.read_bytes()).hexdigest()}}
    output=N.native_risk(spec,tmp_path/'risk',dt.date(2025,1,3),dt.date(2025,1,3))
    published=json.loads(output.read_text())
    assert published['rows']==1
    assert published['gate_status']['exposure_coverage']=='FAIL'
    table=pq.read_table(output.parent/next(iter(published['files']))).to_pylist()
    assert table[0]['adv_21']==1000000
    assert table[0]['beta_252'] is None
    assert table[0]['size'] is None


def test_benchmark_cli_exact_mapping_and_missing_month_failure(tmp_path,monkeypatch):
    import hashlib
    import pyarrow.parquet as pq
    own_dir=tmp_path/'own'
    own_dir.mkdir()
    own_rows=[{'freq':'M','session_date':dt.date(y,m,28),'available_at':dt.datetime(y,m,28,22,tzinfo=dt.UTC),
        'n_universe':100,'n_qualified':100,'missing_reason':None,
        **{k:m/100+y/10000 for k in F.FACTORS}} for y in range(2013,2023) for m in range(1,13)]
    pq.write_table(pa.Table.from_pylist(own_rows,schema=N.FACTOR_SCHEMA),own_dir/'f.parquet')
    benchmark_dir=tmp_path/'benchmark'
    benchmark_dir.mkdir()
    refs=[{'month':r['session_date'],**{k:r[k]*100 for k in F.FACTORS}} for r in own_rows[:-1]]
    pq.write_table(pa.Table.from_pylist(refs),benchmark_dir/'ref.parquet')
    def manifest(directory,stage,file):
        path=directory/'manifest.json'
        output=directory/file
        obj={'stage':stage,'status':'complete','config_sha256':F.config_hash(),'files':{file:{'bytes':output.stat().st_size,'sha256':hashlib.sha256(output.read_bytes()).hexdigest()}}}
        path.write_text(json.dumps(obj))
        return {'manifest':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'stage':stage}
    own=manifest(own_dir,'factors','f.parquet')
    ref=manifest(benchmark_dir,'benchmark','ref.parquet')
    spec={'own_factors':own,'benchmarks':[{**ref,'file':'ref.parquet','units':'percent','date_column':'month','columns':{k:k for k in F.FACTORS}}]}
    con=duckdb.connect()
    monkeypatch.setattr(N.C,'connect',lambda **kw:con)
    artifact=N.validate_benchmarks(spec,tmp_path/'validation')
    assert artifact['gates']['MktRF']['missing_months']==1
    assert artifact['gates']['MktRF']['rho'] is None
    assert (tmp_path/'validation'/'validation.json').exists()


def test_native_setup_binds_final_manifests_and_price_parts(tmp_path):
    import hashlib
    import pyarrow.parquet as pq
    con=fixture_con()
    con.execute('CREATE TABLE t6_fixture_ratios(cik BIGINT,available_at TIMESTAMP)')
    con.execute('CREATE TABLE t6_fixture_liquidity(security_id BIGINT,session_date DATE,available_at TIMESTAMP)')
    con.execute('CREATE TABLE t6_fixture_borrow(security_id BIGINT,session_date DATE,cutoff_utc TIMESTAMPTZ)')
    con.execute('CREATE TABLE t6_fixture_industry(cik BIGINT,available_at TIMESTAMP)')
    def stage(name,tables):
        directory=tmp_path/name
        directory.mkdir()
        files={}
        for file,table in tables.items():
            path=directory/file
            pq.write_table(con.execute('SELECT * FROM '+table).to_arrow_table(),path)
            files[file]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        manifest=directory/'manifest.json'
        manifest.write_text(json.dumps({'stage':name,'status':'complete','files':files}))
        return {'stage':name,'manifest':str(manifest),'sha256':hashlib.sha256(manifest.read_bytes()).hexdigest(),'files':list(files)}
    std=stage('compustat_std_v11',{'funda.parquet':'t6_funda','fundq.parquet':'t6_fundq'})
    ratios=stage('ratios_v11',{'fund_state.parquet':'t6_fixture_ratios'})
    universe=stage('universe',{'universe.parquet':'t6_universe'})
    history=stage('prices_history',{'history.parquet':'t6_prices'})
    current=stage('prices',{'prices.parquet':'t6_prices'})
    fund=stage('fundamentals_v11',{'fixture.parquet':'t6_funda'})
    spec={'config_sha256':F.config_hash(),'release':{v['stage']:{'manifest':v['manifest'],'sha256':v['sha256']} for v in [std,ratios,universe,history,fund]},
        'sources':{'prices':{'parts':[history,current]},'universe':universe,
        'funda':{**std,'files':['funda.parquet']},'fundq':{**std,'files':['fundq.parquet']},
        'ratios_state':ratios,'calendar':stage('calendar',{'calendar.parquet':'t6_calendar'}),
        'cap_quality':None,'eg_training':None,'rf':stage('rf',{'rf.parquet':'t6_rf'}),
        'liquidity':stage('market_liquidity',{'liquidity.parquet':'t6_fixture_liquidity'}),
        'borrow':stage('borrow_proxy',{'borrow.parquet':'t6_fixture_borrow'}),
        'industry':stage('classification',{'industry.parquet':'t6_fixture_industry'})}}
    # Remove the initial fixture tables so production setup defines all views.
    for name in ('prices','universe','funda','fundq','calendar','rf','cap_quality','eg_training'):
        con.execute('DROP TABLE t6_'+name)
    identities=N.setup(con,spec)
    assert identities['prices']['prices_history']==history['sha256']
    assert identities['cap_quality']=='explicitly_unavailable'
    assert con.execute('SELECT count(*) FROM t6_cap_quality').fetchone()[0]==0
    con.close()
