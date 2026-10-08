"""Offline synthetic fixtures only; all connections file-backed128MB/one thread."""
import datetime as dt
import pytest
from atx_db.alpha_panel import golden_panel as P, golden_sources as S, golden_registry as R


@pytest.fixture
def connection(tmp_path):
    con = P.connect(tmp_path)
    yield con
    con.close()


@pytest.fixture
def small_registry(monkeypatch):
    full=R.registry()
    monkeypatch.setattr(R,"registry",lambda:{k:full[k] for k in ("me","ret","guid_surprise")})


def views(con):
    con.execute("CREATE TABLE gu(session_date DATE,security_id BIGINT,cik BIGINT,univ_all_common BOOLEAN,univ_r3000p BOOLEAN,exchcd_like INTEGER,me DOUBLE,available_at TIMESTAMPTZ)")
    con.execute("INSERT INTO gu VALUES ('2020-01-02',1,10,true,false,1,100,'2020-01-02 22:00:00+00'),('2020-01-03',1,10,true,true,1,200,'2020-01-03 22:00:00+00'),('2020-01-06',1,10,true,true,1,300,'2020-01-06 22:00:00+00')")
    con.execute("CREATE TABLE gp AS SELECT session_date,security_id,true member_equity,0.01::DOUBLE ret,false ret_guarded FROM gu")
    con.execute("CREATE TABLE gc AS SELECT DISTINCT session_date,lag(session_date) OVER(ORDER BY session_date) previous_session,max(session_date) OVER() month_end FROM gu")


def test_strict_gc1_replay_alignment_and_coverage(connection,tmp_path,small_registry):
    views(connection)
    query=P.query("daily",dt.date(2020,1,3),dt.date(2020,1,6),{})
    path=tmp_path/"unit.parquet"
    P.C.copy_to_parquet(connection,query,path)
    rows=connection.execute(f"SELECT session_date,me,ret,guid_surprise,guid_surprise_quality FROM {P.sql([path])} ORDER BY 1").fetchall()
    assert rows[0][1:4]==(None,None,None) # source at Jan2 22 == strict cutoff
    assert rows[1][1:4]==(200.0,.01,None)
    result=P.measure(connection,path,query)
    assert result["all_common_differences"]==0 and result["replay_differences"]==0
    assert len(result["coverage"])==9
    connection.execute("UPDATE gu SET me=999 WHERE session_date='2020-01-03'")
    with pytest.raises(ValueError,match="replay"):
        P.measure(connection,path,query)


def test_native_not_generic_admission(connection,tmp_path):
    with pytest.raises(ValueError,match="unsupported native"):
        S.admit("random_parquet",{"path":str(tmp_path/"x.parquet")},connection)
    excluded=S.admit("thirteenf_features",{},connection)
    assert excluded["status"]=="excluded" and not excluded["products"]
    with pytest.raises(ValueError,match="independently reviewed"):
        P.admit_plan({"schema":"atx.golden-bindings/v1"},connection)


def test_consumed_product_pins_cannot_be_resealed_from_changed_bytes(tmp_path):
    product=tmp_path/'immutable-native-product.parquet'
    product.write_bytes(b'original independently admitted native product')
    expected=S.pin(product)
    row={'status':'admitted_diagnostic','products':{'native':product},
         'dependency_pins':{str(product.resolve()):expected},'spec':{}}
    assert S.admitted_pins({'fixed_native':row})==row['dependency_pins']
    product.write_bytes(b'changed after native admission')
    with pytest.raises(ValueError,match='consumed native product'):
        S.admitted_pins({'fixed_native':row})
    row['dependency_pins']={}
    with pytest.raises(ValueError,match='consumed native product'):
        S.admitted_pins({'fixed_native':row})


def test_registry_complete_owner_and_no_economic_substitution():
    registry=R.registry()
    assert len(registry)>450
    assert {"owner_roic","owner_sale_q_yoy","at_gr1","op_at","std_saleq","ratio_bm","daily_mom_12_1"} <= registry.keys()
    config=R.evaluation_config()
    assert len(config["primary"])==13
    assert config["owner_directions"]["annual asset growth"]=="at_gr1"
    assert sum(r["name"]=="at_gr1" for r in config["primary"])==1
    assert registry['std_epsfxq']['unit']=='USD/share'
    assert 'ratio_piotroski_f' in registry and registry['score_be_me']['kind']=='normalized_score'


def test_normalized_scores_use_only_qualified_names_average_ties_and_max_clock(connection,monkeypatch):
    full=R.registry();score={**full['score_be_me'],'name':'score_me','source_columns':['me'],
        'bronze_to_gold':{'direction':1,'raw_field':'me'}}
    monkeypatch.setattr(R,'registry',lambda:{'me':full['me'],'score_me':score})
    connection.execute("CREATE TABLE gu AS SELECT d::DATE session_date,i::BIGINT security_id,i::BIGINT cik,true univ_all_common,true univ_r3000p,1 exchcd_like,ceil(i/2.0)::DOUBLE me,d::TIMESTAMPTZ+INTERVAL '22 hours' available_at FROM (VALUES (DATE '2020-01-03'),(DATE '2020-01-06')) dates(d) CROSS JOIN range(1,103) ids(i)")
    connection.execute('CREATE TABLE gp AS SELECT session_date,security_id,true member_equity,0.0::DOUBLE ret,false ret_guarded FROM gu')
    connection.execute('CREATE TABLE gc AS SELECT DISTINCT session_date,lag(session_date) OVER(ORDER BY session_date) previous_session,max(session_date) OVER() month_end FROM (SELECT DISTINCT session_date FROM gu)')
    connection.execute('CREATE TABLE gm(security_id BIGINT,session_date DATE,available_at TIMESTAMPTZ)')
    connection.execute("CREATE TABLE g_cap_quality AS SELECT security_id,session_date,available_at,true certified,true membership_certified,'USD' currency,available_at source_available_at FROM gu")
    connection.execute('CREATE TABLE g_prices(security_id BIGINT,session_date DATE,ret DOUBLE)')
    connection.execute('CREATE TABLE g_price_quality(security_id BIGINT,session_date DATE,return_certified BOOLEAN,source_available_at TIMESTAMPTZ,available_at TIMESTAMPTZ)')
    connection.execute('CREATE TABLE g_daily_factors(session_date DATE,rf DOUBLE,available_at TIMESTAMPTZ)')
    q=P.query('daily',dt.date(2020,1,6),dt.date(2020,1,6),{'char_monthly':{'status':'admitted_diagnostic'}})
    result=connection.execute('SELECT count(score_me),min(score_me),max(score_me),count(DISTINCT score_me_source_at),bool_and(score_me_qualified) FROM ('+q+')').fetchone()
    assert result[0]==102 and result[1]==pytest.approx(-100/101) and result[2]==pytest.approx(100/101)
    assert result[3:]==(1,True)


def test_notes_fail_closed_curated_and_amendment():
    cutoff=dt.datetime(2022,2,1,tzinfo=dt.UTC)
    accession, original=next(iter(S.curated_notes().items()))
    expected={**original,"period":"2021-12-31"}
    proof={"corroboration":{k:expected[k] for k in ("primary_sha256","role","concept","members","basis")}}
    expected["proof_sha256"]=S.digest(proof)
    row={"bseg_partition_source_adsh":accession,"source_accession":accession,"source_sha256":original["primary_sha256"],"period":"2021-12-31","available_at":cutoff-dt.timedelta(days=1),"field":"bseg_n"}
    inventory={accession:expected}
    assert S.notes_admission(row,proof,inventory,cutoff)==(len(original["members"]),"curated_reportable_count")
    native_empty={**proof,'external_revenue_selection':{}}
    native_inventory={accession:{**expected,'proof_sha256':S.digest(native_empty)}}
    assert S.notes_admission(row,native_empty,native_inventory,cutoff)==(len(original['members']),'curated_reportable_count')
    assert S.notes_admission(row,{**native_empty,'selected_external_revenue':{}},native_inventory,cutoff)[0] is None
    for change in ({"period":"2020-12-31"},{"source_accession":"amendment"},{"source_sha256":"b"*64},{"field":"tax_dtl"},{"available_at":cutoff}):
        assert S.notes_admission({**row,**change},proof,inventory,cutoff)[0] is None
    assert S.notes_admission(row,{**proof,"selected_external_revenue":{}},inventory,cutoff)[0] is None
    automatic={"corroboration":{**proof["corroboration"],"basis":"automatic explicit list"}}
    assert S.notes_admission(row,automatic,inventory,cutoff)[0] is None
    assert S.notes_admission({**row,"field":"bseg_rev_hhi"},proof,inventory,cutoff)[0] is None


def test_notes_native_vintage_mapping_preserves_ambiguity_and_strict_gc1(connection,monkeypatch):
    views(connection);full=R.registry()
    monkeypatch.setattr(R,'registry',lambda:{name:full[name] for name in ('bseg_n','bseg_rev_hhi','tax_dtl','foreign_sales_share')})
    connection.execute('CREATE TABLE gnotes(cik BIGINT,source_accession VARCHAR,period DATE,field VARCHAR,available_at TIMESTAMPTZ,value DOUBLE,diagnostic_value DOUBLE,qualified BOOLEAN,reason VARCHAR)')
    connection.execute("INSERT INTO gnotes VALUES(10,'original','2019-12-31','bseg_n','2020-01-03 22:00:00+00',5,5,true,'curated_reportable_count'),(10,'amendment','2019-12-31','bseg_n','2020-01-05 22:00:00+00',99,99,true,'curated_reportable_count'),(10,'tax','2019-12-31','tax_dtl','2020-01-03 22:00:00+00',NULL,7,false,'ambiguous_note_or_unverified_geography')")
    q=P.query('daily',dt.date(2020,1,6),dt.date(2020,1,6),{'fundamentals_notes':{'status':'admitted_diagnostic'}})
    assert connection.execute('SELECT bseg_n,bseg_n_qualified,tax_dtl,tax_dtl_qualified FROM ('+q+')').fetchone()==(5.0,True,7.0,False)
    # A newer visible unsupported amendment cannot fall back to an older pass.
    connection.execute("INSERT INTO gnotes VALUES(10,'unsupported','2019-12-31','bseg_n','2020-01-05 21:00:00+00',NULL,8,false,'unverified_native_amendment')")
    assert connection.execute('SELECT bseg_n,bseg_n_qualified FROM ('+q+')').fetchone()==(8.0,False)
    connection.execute("INSERT INTO gnotes VALUES(10,'conflict','2019-12-31','bseg_n','2020-01-05 21:00:00+00',6,6,true,'curated_reportable_count')")
    assert connection.execute('SELECT bseg_n,bseg_n_qualified FROM ('+q+')').fetchone()==(None,False)


def test_path_traversal_stale_pins_and_write_lock(tmp_path):
    with pytest.raises(ValueError): S.member(tmp_path,"../other")
    path=tmp_path/"input";path.write_text("original")
    pins={str(path.resolve()):S.pin(path)};S.check_pins(pins)
    path.write_text("changed")
    with pytest.raises(ValueError,match="stale"): S.check_pins(pins)
    with P.writer_lock(tmp_path):
        with pytest.raises(FileExistsError):
            with P.writer_lock(tmp_path): pass


def test_immutable_restart_atomic_pointer_stale_input_and_public_replay(tmp_path,monkeypatch,small_registry):
    fixture=tmp_path/"native";fixture.mkdir()
    con=P.connect(tmp_path/"source-db")
    try:
        views(con)
        for table,name in (("gu","universe"),("gp","panel"),("gc","calendar")):
            P.C.copy_to_parquet(con,f"SELECT * FROM {table}",fixture/(name+".parquet"))
    finally:con.close()
    admitted={"universe_release_v1":{"status":"admitted_diagnostic","products":{"universe_2020":fixture/"universe.parquet",
              "panel_2020":[fixture/"panel.parquet"],"calendar":[fixture/"calendar.parquet"]}}}
    # Synthetic native dispatch still supplies independently fixed actual-byte
    # identities; production admission derives these from its pinned manifest.
    admitted['universe_release_v1']['dependency_pins']={str(p.resolve()):S.pin(p) for p in fixture.glob('*.parquet')}
    # ONLY fixture dispatch is replaced; production has no injectable adapter.
    monkeypatch.setattr(P,"admit_plan",lambda plan,con:(admitted,{"ea_actuals":"synthetic pending"}))
    plan=tmp_path/"plan.json";P.C.write_json_atomic(plan,{"start":"2020-01-02","end":"2020-01-06","code":S.code_pins(),"sources":{}})
    stage=tmp_path/"golden"
    assert P.build(plan,stage,"fixture",max_new_units=1)["status"]=="continuation"
    assert not (stage/"current.json").exists()
    result=P.build(plan,stage,"fixture")
    assert result["status"]=="diagnostic_published"
    assert result["gate_status"]["source_admission"]=="FAIL"
    assert P.C.read_json(stage/'generations'/'fixture'/'snapshot.json')['source_pins']==admitted['universe_release_v1']['dependency_pins']
    rows=list(P.load_golden("2020-01-02","2020-01-06",["session_date","security_id","me"],stage=stage,batch_size=2))
    assert sum(len(batch) for batch in rows)==3
    P.C.write_json_atomic(plan,{"start":"2020-01-03","end":"2020-01-06"})
    with pytest.raises(ValueError,match="stale restart|original full daily"):
        P.build(plan,stage,"fixture")
    with pytest.raises(ValueError,match="plan changed|stale independent input/code pin"):
        list(P.load_golden("2020-01-02","2020-01-06",["me"],stage=stage))


def test_ratio_transaction_projection_preserves_typed_population_and_refuses_redirect(tmp_path):
    from tests.test_alpha_panel_ratios import World
    from atx_db.alpha_panel import tier1_native_readers as I
    w=World(tmp_path)
    date=dt.date(2020,6,1)
    for quarter,end in enumerate((dt.date(2019,6,30),dt.date(2019,9,30),dt.date(2019,12,31),dt.date(2020,3,31)),1):
        w.q(1,end,quarter,'q'+str(quarter),dt.datetime.combine(end+dt.timedelta(days=35),dt.time()),
            atq=100,ltq=40,saleq=25,niq=3)
    w.a(1,dt.date(2019,12,31),'annual',dt.datetime(2020,2,5),at=100,lt=40,sale=100,ni=12)
    w.link(1,1)
    # Duplicates must survive exact native projection, including failed cells.
    w.px.extend([{'session_date':date,'security_id':1,'close':10.0}]*2)
    w.sh.append({'session_date':date,'security_id':1,'shrout':100.0})
    w.write([date])
    generation=tmp_path/'ratios_v11'/'generations'/'fixture'
    generation.mkdir(parents=True)
    con=P.connect(tmp_path/'ratio-db')
    try:
        I.Q.build_splits(con,tmp_path)
        P.C.copy_to_parquet(con,'SELECT * FROM spl',generation/'split_events.parquet')
        I.Q.build_state_bucket(con,tmp_path/'compustat_std','true',generation/'fund_state.parquet')
        sessions="SELECT DATE '2020-06-01' AS session_date"
        cutoff='CAST(session_date AS TIMESTAMP)+INTERVAL 22 HOUR'
        expected=I.Q.consume_sql(I.Q.cells_sql(tmp_path,sessions,cutoff),
            I.A.sql([generation/'fund_state.parquet']),I.A.sql([generation/'split_events.parquet']))
        actual=I._ratio_projection(tmp_path,generation,sessions,cutoff)
        assert con.execute('DESCRIBE ('+actual+')').fetchall()==con.execute('DESCRIBE ('+expected+')').fetchall()
        con.execute('CREATE TEMP TABLE projection_actual AS '+actual)
        con.execute('CREATE TEMP TABLE projection_native AS '+expected)
        assert con.execute('SELECT count(*) FROM projection_actual').fetchone()[0]==2
        assert con.execute('SELECT count(*) FROM ((SELECT * FROM projection_actual EXCEPT ALL SELECT * FROM projection_native) UNION ALL (SELECT * FROM projection_native EXCEPT ALL SELECT * FROM projection_actual))').fetchone()[0]==0
    finally:
        con.close()
    # Path refusal uses no actual warehouse source reads or global overrides.
    lake=P.C.build_root().resolve()
    fixed=lake/I.K.RATIOS/'generations'/'fixture'
    admission={'status':'admitted_diagnostic','spec':{'lake_root':str(lake),
        'generation':fixed.relative_to(lake).as_posix()},
        'products':{'ratios_state':generation/'fund_state.parquet'}}
    with pytest.raises(ValueError,match='fixed admitted native ratio state'):
        I._admitted_ratio_sql(admission,sessions,cutoff)


