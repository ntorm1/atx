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
    con.execute("SET memory_limit='128MB'")
    con.execute("SET threads=1")
    con.execute("SET TimeZone='UTC'")
    con.execute('CREATE TABLE t6_funda(cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seq DOUBLE,ceq DOUBLE,"at" DOUBLE,lt DOUBLE,txditc DOUBLE,pstk DOUBLE,sale DOUBLE,cogs DOUBLE,xsga DOUBLE,xint DOUBLE,revt DOUBLE,xrd DOUBLE,rect DOUBLE,invt DOUBLE,xpp DOUBLE,drc DOUBLE,drlt DOUBLE,ap DOUBLE,xacc DOUBLE,dltt DOUBLE,dlc DOUBLE,sic BIGINT)')
    con.execute('CREATE TABLE t6_fundq(cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seqq DOUBLE,ceqq DOUBLE,atq DOUBLE,ltq DOUBLE,pstkq DOUBLE,ibq DOUBLE)')
    con.execute("CREATE TABLE t6_universe(session_date DATE,security_id BIGINT,cik BIGINT,me DOUBLE,exchcd_like INTEGER,univ_all_common BOOLEAN,univ_r3000p BOOLEAN,available_at TIMESTAMP DEFAULT TIMESTAMP '1900-01-01')")
    con.execute('CREATE TABLE t6_prices(session_date DATE,security_id BIGINT,ret DOUBLE,ret_guarded BOOLEAN,close DOUBLE)')
    con.execute('CREATE TABLE t6_calendar(session_date DATE)')
    con.execute('CREATE TABLE t6_rf(freq VARCHAR,session_date DATE,rf DOUBLE)')
    con.execute('CREATE TABLE t6_cap_quality('+N.EMPTY['cap_quality']+')')
    con.execute('CREATE TABLE t6_eg_training('+N.EMPTY['eg_training']+')')
    con.execute('CREATE TABLE t6_member_equity(session_date DATE,security_id BIGINT)')
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
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2022-02-28',1,77159,711930800,1,true,true)")
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
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2025-01-03',1,1,100,1,true,true)")
    con.execute("INSERT INTO t6_liquidity VALUES(1,'2025-01-01','2025-01-01',1000000,2000000,.01,.2)")
    fdir=tmp_path/'own'
    fdir.mkdir()
    pq.write_table(pa.Table.from_pylist([],schema=N.FACTOR_SCHEMA),fdir/'f.parquet')
    fm={'stage':'factors','status':'complete','input_manifests_sha256':{},'input_identity':{'fixture':'offline','config_sha256':F.config_hash(),'kind':'factors','code':N.code_identity()},'config_sha256':F.config_hash(),'files':{'f.parquet':{'bytes':(fdir/'f.parquet').stat().st_size,'sha256':hashlib.sha256((fdir/'f.parquet').read_bytes()).hexdigest()}}}
    manifest=fdir/'manifest.json'
    manifest.write_text(json.dumps(fm))
    monkeypatch.setattr(N.C,'connect',lambda **kw:con)
    monkeypatch.setattr(N,'setup',lambda con,spec:{'fixture':'offline'})
    monkeypatch.setattr(N,'setup_check',lambda spec:None)
    spec={'release':{},'own_factors':{'manifest':str(manifest),'sha256':hashlib.sha256(manifest.read_bytes()).hexdigest()}}
    from atx_db.alpha_panel import risk_inputs as R
    specfile=tmp_path/'spec.json';specfile.write_text(json.dumps(spec))
    monkeypatch.setenv('ATX_GUARD_JOB','offline-fixture')
    monkeypatch.setattr('sys.argv',['risk_inputs','--native-spec',str(specfile),'--destination',str(tmp_path/'risk'),'--start','2025-01-03','--end','2025-01-03'])
    R.main()
    output=tmp_path/'risk'/'manifest.json'
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
    con.execute("SET memory_limit='128MB'")
    con.execute("SET threads=1")
    con.execute("SET TimeZone='UTC'")
    monkeypatch.setattr(N.C,'connect',lambda **kw:con)
    artifact=N.validate_benchmarks(spec,tmp_path/'validation')
    assert artifact['gates']['MktRF']['missing_months']==1
    assert artifact['gates']['MktRF']['rho'] is None
    assert (tmp_path/'validation'/'validation.json').exists()


@pytest.mark.parametrize('overlap',[False,True])
def test_native_setup_binds_final_manifests_and_price_parts(tmp_path,overlap):
    import hashlib
    import pyarrow.parquet as pq
    con=fixture_con()
    if overlap:
        con.execute("INSERT INTO t6_prices VALUES('2022-03-01',1,.1,false,1)")
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
    spec={'member_equity':None,'config_sha256':F.config_hash(),'release':{v['stage']:{'manifest':v['manifest'],'sha256':v['sha256']} for v in [std,ratios,universe,history,fund]},
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
    if overlap:
        with pytest.raises(ValueError,match='duplicate/overlapping'):
            N.setup(con,spec)
        con.close()
        return
    identities=N.setup(con,spec)
    assert identities['prices']['prices_history']==history['sha256']
    assert identities['source_specification']==spec['sources']
    assert identities['release_bindings']==spec['release']
    assert identities['cap_quality']=='explicitly_unavailable'
    assert con.execute('SELECT count(*) FROM t6_cap_quality').fetchone()[0]==0
    con.close()


def audit(con,security=1,clock='2022-02-27',cert=True):
    con.execute("INSERT INTO t6_cap_quality VALUES(?,DATE '2022-02-28',CAST(? AS TIMESTAMP),TIMESTAMP '2022-02-26',?,true,'fixture','independently_verified','e','s','m')",[security,clock,cert])


def test_review_complete_grid_missing_and_duplicate_returns():
    con=fixture_con()
    con.execute("INSERT INTO t6_calendar VALUES('2022-02-28'),('2022-03-01'),('2022-03-02')")
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2022-02-28',1,1,100,1,true,true),('2022-02-28',2,1,50,1,true,true)")
    audit(con);audit(con,2)
    con.execute("INSERT INTO t6_prices VALUES('2022-03-01',1,.1,false,1),('2022-03-02',1,.1,false,1),('2022-03-01',2,.1,false,1)")
    result=N.issuer_period(con,dt.date(2022,3,1),dt.date(2022,3,2),dt.date(2022,2,28))[0]
    assert result['certified'] and result['ret'] is None
    con.execute("INSERT INTO t6_prices VALUES('2022-03-02',2,.1,false,1)")
    assert N.issuer_period(con,dt.date(2022,3,1),dt.date(2022,3,2),dt.date(2022,2,28))[0]['ret']==pytest.approx(.21)
    con.execute("INSERT INTO t6_prices VALUES('2022-03-01',1,.1,false,1)")
    with pytest.raises(Exception,match='duplicate'):
        N.issuer_period(con,dt.date(2022,3,1),dt.date(2022,3,2),dt.date(2022,2,28))
    with pytest.raises(ValueError,match='duplicate/overlapping'):
        S.assert_price_keys(con)
    con.close()


def test_review_audit_latest_revocation_and_equal_clock():
    con=fixture_con()
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2022-02-28',1,1,100,1,true,true)")
    audit(con,clock='2022-02-26',cert=False)
    audit(con,clock='2022-02-27',cert=True)
    assert N.formation_issuers(con,dt.date(2022,2,28))[0]['certified']
    audit(con,clock='2022-02-28 23:00',cert=False)
    assert N.formation_issuers(con,dt.date(2022,2,28))[0]['certified']
    audit(con,clock='2022-02-28 12:00',cert=False)
    assert not N.formation_issuers(con,dt.date(2022,2,28))[0]['certified']
    audit(con,clock='2022-02-28 12:00',cert=True)
    with pytest.raises(Exception,match='ambiguous'):
        N.formation_issuers(con,dt.date(2022,2,28))
    con.close()


def test_review_resume_selected_members_and_mapping(tmp_path):
    schema=pa.schema([('x',pa.int64())])
    first={'sources':{'rf':{'sha256':'same','files':['a','b'],'frequency_files':{'D':'a','M':'b'}}}}
    second={'sources':{'rf':{'sha256':'same','files':['a','b'],'frequency_files':{'D':'b','M':'a'}}}}
    third={'sources':{'rf':{'sha256':'same','files':['b'],'frequency_files':{'D':'b','M':'b'}}}}
    with publisher_lock(tmp_path):
        g=Generation(tmp_path,'test',first,schema);g.write('u',[{'x':1}])
        h=Generation(tmp_path,'test',second,schema)
        j=Generation(tmp_path,'test',third,schema)
        assert len({g.token,h.token,j.token})==3 and not h.done('u') and not j.done('u')


def test_review_factor_lineage_rejects_release_and_selection_mutation():
    identity={'sources':'exact','config_sha256':F.config_hash(),'kind':'factors','code':N.code_identity()}
    own={'input_identity':identity,'input_manifests_sha256':{'universe':'old'}}
    spec={'release':{'universe':{'sha256':'old'}}}
    N.require_factor_lineage(own,spec,{'sources':'exact'})
    with pytest.raises(ValueError,match='release lineage'):
        N.require_factor_lineage(own,{'release':{'universe':{'sha256':'new'}}},{'sources':'exact'})
    with pytest.raises(ValueError,match='selected-source'):
        N.require_factor_lineage(own,spec,{'sources':'changed'})
    with pytest.raises(ValueError,match='release lineage'):
        N.require_factor_lineage({},spec,{'sources':'exact'})


def test_review_coverage_independent_missing_output_and_r3000p():
    con=fixture_con()
    con.execute('CREATE TABLE risk_output(session_date DATE,security_id BIGINT,beta DOUBLE)')
    for y in range(2020,2026):
        con.execute("INSERT INTO t6_calendar VALUES(?)",[dt.date(y,3,2)])
        con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES(?,1,1,100,1,true,true),(?,2,2,100,1,true,true)",[dt.date(y,3,2)]*2)
        con.execute("INSERT INTO t6_member_equity VALUES(?,1),(?,2)",[dt.date(y,3,2)]*2)
        con.execute('INSERT INTO risk_output VALUES(?,1,1),(?,2,1)',[dt.date(y,3,2)]*2)
    cov,gate=N.coverage_gate(con,'risk_output',dt.date(2020,1,1),dt.date(2025,12,31),True,'beta IS NOT NULL')
    assert gate=='PASS'
    con.execute("DELETE FROM risk_output WHERE session_date='2022-03-02' AND security_id=2")
    cov,gate=N.coverage_gate(con,'risk_output',dt.date(2020,1,1),dt.date(2025,12,31),True,'beta IS NOT NULL')
    assert gate=='FAIL'
    target=next(r for r in cov if r.get('year')==2022 and r['universe']=='r3000p')
    assert target['denominator']==2 and target['output_cells']==1
    con.execute("UPDATE t6_universe SET univ_all_common=false WHERE security_id=2")
    cov,gate=N.coverage_gate(con,'risk_output',dt.date(2020,1,1),dt.date(2025,12,31),True,'beta IS NOT NULL')
    assert gate=='FAIL'
    _,gate=N.coverage_gate(con,'risk_output',dt.date(2020,1,1),dt.date(2025,12,31),False,'beta IS NOT NULL')
    assert gate=='FAIL'
    con.close()


def test_review_risk_lagged_issuer_me_and_component_clocks():
    con=fixture_con()
    con.execute('CREATE TABLE t6_liquidity(security_id BIGINT,session_date DATE,available_at TIMESTAMP,adv_21 DOUBLE,adv_63 DOUBLE,spread_cs_21 DOUBLE,amihud_21 DOUBLE)')
    con.execute('CREATE TABLE t6_borrow(security_id BIGINT,session_date DATE,cutoff_utc TIMESTAMPTZ,si_to_io DOUBLE,si_shares DOUBLE,ftd_quantity DOUBLE,on_threshold_list BOOLEAN,si_to_io_available_at TIMESTAMPTZ,si_available_at TIMESTAMPTZ,ftd_available_at TIMESTAMPTZ,threshold_available_at TIMESTAMPTZ,ftd_settlement_date DATE,threshold_list_date DATE)')
    con.execute('CREATE TABLE t6_industry(cik BIGINT,available_at TIMESTAMP,ff49_no BIGINT)')
    con.execute('CREATE TABLE t6_ratios_state(cik BIGINT,available_at TIMESTAMP,be_q0 DOUBLE,at_q0 DOUBLE,at_q4 DOUBLE,debt_at DOUBLE,gprof DOUBLE,datadate DATE)')
    con.execute("INSERT INTO t6_calendar VALUES('2022-02-28'),('2022-03-01')")
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2022-02-28',1,1,100,1,true,true),('2022-02-28',2,1,50,1,true,true),('2022-03-01',1,1,999,1,true,true),('2022-03-01',2,1,888,1,true,true)")
    audit(con);audit(con,2)
    con.execute("INSERT INTO t6_borrow VALUES(1,'2022-02-28','2022-02-27',7,100,10,true,'2022-03-04','2022-03-04','2022-03-04','2022-03-04','2022-02-28','2022-02-28')")
    cutoff=dt.datetime(2022,2,28,22,tzinfo=dt.UTC)
    out=list(N.rows(con,S.risk_primitives_sql(dt.date(2022,3,1),cutoff)))
    assert [r['me'] for r in out]==[150,150]
    assert all(out[0][k] is None for k in ('si_to_io','si_shares','ftd_quantity','on_threshold_list'))
    con.execute("UPDATE t6_borrow SET si_to_io_available_at='2022-02-28 22:00',si_available_at='2022-02-28 21:59'")
    row=next(r for r in N.rows(con,S.risk_primitives_sql(dt.date(2022,3,1),cutoff)) if r['security_id']==1)
    assert row['si_to_io'] is None and row['si_shares']==100
    con.execute("UPDATE t6_universe SET available_at='2022-02-28 22:00' WHERE session_date='2022-02-28' AND security_id=2")
    late=list(N.rows(con,S.risk_primitives_sql(dt.date(2022,3,1),cutoff)))
    assert all(r['me'] is None for r in late)
    assert not next(r for r in late if r['security_id']==2)['cap_qualified']
    con.close()


def test_review_eg_audit_vintages_do_not_double_me():
    con=fixture_con()
    con.execute("INSERT INTO t6_funda(cik,datadate,available_at,seq,\"at\",dltt,dlc) VALUES(1,'2021-12-31','2022-01-15',50,100,0,0)")
    con.execute("INSERT INTO t6_calendar VALUES('2021-12-31')")
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2021-12-31',1,1,100,1,true,true)")
    con.execute("INSERT INTO t6_cap_quality VALUES(1,'2021-12-31','2022-01-01','2021-12-31',true,true,'fixture','independently_verified','e','s','m')")
    cutoff=dt.datetime(2022,6,30,22,tzinfo=dt.UTC)
    def value():
        return list(N.rows(con,N.eg_predictors_sql(dt.date(2022,6,30),cutoff)))[0]['log_q']
    assert value()==pytest.approx(0)
    con.execute("INSERT INTO t6_cap_quality VALUES(1,'2021-12-31','2022-02-01','2021-12-31',true,true,'fixture','independently_verified','e','s','m')")
    assert value()==pytest.approx(0)
    con.execute("INSERT INTO t6_cap_quality VALUES(1,'2021-12-31','2022-03-01','2021-12-31',false,true,'revoked','independently_verified','e','s','m')")
    assert value() is None
    con.close()


def test_review_bab_market_retains_missing_eligible_line(monkeypatch):
    con=fixture_con()
    dates=[dt.date(2018,1,1)+dt.timedelta(days=i) for i in range(753)]
    con.executemany('INSERT INTO t6_calendar VALUES(?)',[(d,) for d in dates])
    con.executemany('INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES(?,?,?,100,1,true,true)',[(d,s,s) for d in dates for s in (1,2)])
    con.executemany("INSERT INTO t6_cap_quality VALUES(?,?,?, ?,true,true,'fixture','independently_verified','e','s','m')",[(s,d,dt.datetime(2017,1,1),dt.datetime(2017,1,1)) for d in dates for s in (1,2)])
    con.executemany('INSERT INTO t6_prices VALUES(?,?,.01,false,1)',[(d,s) for d in dates[1:] for s in (1,2) if (d,s)!=(dates[-1],2)])
    captures=[]
    monkeypatch.setattr(F,'bab_beta',lambda stock,market:captures.append((stock,market)))
    N.native_betas(con,dates[-1],dates)
    assert len(captures)==2 and all(market[-1] is None for stock,market in captures)
    assert captures[1][0][-1] is None
    con.close()


def test_review_signal_history_requires_complete_daily_support():
    con=fixture_con()
    con.execute("INSERT INTO t6_calendar VALUES('2022-03-01'),('2022-03-02'),('2022-03-31')")
    con.execute("INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES('2022-03-31',1,1,100,1,true,true)")
    con.execute("INSERT INTO t6_prices VALUES('2022-03-01',1,.1,false,1),('2022-03-31',1,.1,false,1)")
    assert N.signal_history(con,dt.date(2022,3,31),1,0)[1] is None
    con.execute("INSERT INTO t6_prices VALUES('2022-03-02',1,.1,false,1)")
    assert N.signal_history(con,dt.date(2022,3,31),1,0)[1]==pytest.approx(.331)
    con.execute("INSERT INTO t6_prices VALUES('2022-03-02',1,.1,false,1)")
    with pytest.raises(Exception,match='duplicate'):
        N.signal_history(con,dt.date(2022,3,31),1,0)
    con.close()
