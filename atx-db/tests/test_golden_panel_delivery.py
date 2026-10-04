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
    for change in ({"period":"2020-12-31"},{"source_accession":"amendment"},{"source_sha256":"b"*64},{"field":"tax_dtl"},{"available_at":cutoff}):
        assert S.notes_admission({**row,**change},proof,inventory,cutoff)[0] is None
    assert S.notes_admission(row,{**proof,"selected_external_revenue":{}},inventory,cutoff)[0] is None
    automatic={"corroboration":{**proof["corroboration"],"basis":"automatic explicit list"}}
    assert S.notes_admission(row,automatic,inventory,cutoff)[0] is None
    assert S.notes_admission({**row,"field":"bseg_rev_hhi"},proof,inventory,cutoff)[0] is None


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
    # ONLY fixture dispatch is replaced; production has no injectable adapter.
    monkeypatch.setattr(P,"admit_plan",lambda plan,con:(admitted,{"ea_actuals":"synthetic pending"}))
    plan=tmp_path/"plan.json";P.C.write_json_atomic(plan,{"start":"2020-01-02","end":"2020-01-06"})
    stage=tmp_path/"golden"
    assert P.build(plan,stage,"fixture",max_new_units=1)["status"]=="continuation"
    assert not (stage/"current.json").exists()
    result=P.build(plan,stage,"fixture")
    assert result["status"]=="diagnostic_published"
    assert result["gate_status"]["source_admission"]=="FAIL"
    rows=list(P.load_golden("2020-01-02","2020-01-06",["session_date","security_id","me"],stage=stage,batch_size=2))
    assert sum(len(batch) for batch in rows)==3
    P.C.write_json_atomic(plan,{"start":"2020-01-03","end":"2020-01-06"})
    with pytest.raises(ValueError,match="stale restart|original full daily"):
        P.build(plan,stage,"fixture")
    with pytest.raises(ValueError,match="plan changed"):
        list(P.load_golden("2020-01-02","2020-01-06",["me"],stage=stage))
