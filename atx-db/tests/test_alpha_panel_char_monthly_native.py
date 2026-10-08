import datetime as dt
import json
import duckdb
import pytest
from atx_db.alpha_panel import char_monthly as M,char_monthly_sources as N


@pytest.fixture
def con():
    con=duckdb.connect(config={"memory_limit":"128MB","threads":"1"})
    con.execute("SET TimeZone='UTC'")
    yield con
    con.close()


def fixture(con,date=dt.date(2022,6,30),invalid_me=None):
    tau=N.clock(date);source=tau-dt.timedelta(days=10)
    days=[date-dt.timedelta(days=i) for i in range(35) if (date-dt.timedelta(days=i)).weekday()<5][-21:]
    days=sorted(days)
    if date not in days:days.append(date)
    con.execute("CREATE TABLE cm_calendar(session_date DATE)")
    con.executemany("INSERT INTO cm_calendar VALUES(?)",[(d,) for d in days])
    con.execute("CREATE TABLE cm_universe(session_date DATE,security_id BIGINT,cik BIGINT,me DOUBLE,univ_all_common BOOLEAN,univ_r3000p BOOLEAN,available_at TIMESTAMPTZ)")
    con.executemany("INSERT INTO cm_universe VALUES(?,?,?,?,true,true,?)",[(date,1,10,50,tau),(date,2,10,50 if invalid_me is None else invalid_me,tau)])
    con.execute("CREATE TABLE cm_member_equity(session_date DATE,security_id BIGINT,member_equity BOOLEAN,available_at TIMESTAMPTZ,source_available_at TIMESTAMPTZ)")
    con.executemany("INSERT INTO cm_member_equity VALUES(?,?,true,?,?)",[(date,sid,source,source) for sid in (1,2)])
    con.execute("CREATE TABLE cm_funda(cik BIGINT,datadate DATE,fyear INTEGER,accession VARCHAR,available_at TIMESTAMPTZ,curcd VARCHAR,\"at\" DOUBLE,seq DOUBLE,pstk DOUBLE,txditc DOUBLE,sale DOUBLE,cogs DOUBLE,ni DOUBLE,ib DOUBLE)")
    con.execute("INSERT INTO cm_funda VALUES(10,DATE '2021-12-31',2021,'a',?,'USD',100,60,0,0,80,10,20,15)",[source])
    con.execute("CREATE TABLE cm_fundq(cik BIGINT,datadate DATE,fyearq INTEGER,fqtr INTEGER,accession VARCHAR,available_at TIMESTAMPTZ,curcd VARCHAR)")
    con.execute("CREATE TABLE cm_primitive_quality(cik BIGINT,datadate DATE,accession VARCHAR,item VARCHAR,available_at TIMESTAMPTZ,source_available_at TIMESTAMPTZ,snapshot_available_at TIMESTAMPTZ,certified BOOLEAN,unit VARCHAR)")
    con.executemany("INSERT INTO cm_primitive_quality VALUES(10,DATE '2021-12-31','a',?,?,?,?,true,'USD')",[(name,source,source,source) for name in ("at","seq","pstk","txditc","sale","cogs","ni","ib")])
    con.execute("CREATE TABLE cm_cap_quality(security_id BIGINT,session_date DATE,available_at TIMESTAMPTZ,source_available_at TIMESTAMPTZ,certified BOOLEAN,membership_certified BOOLEAN,currency VARCHAR)")
    con.executemany("INSERT INTO cm_cap_quality VALUES(?,?,?,?,true,true,'USD')",[(sid,date,source,source) for sid in (1,2)])
    con.execute("CREATE TABLE cm_sector(cik BIGINT,sector VARCHAR,available_at TIMESTAMPTZ)")
    con.execute("CREATE TABLE cm_prices(security_id BIGINT,session_date DATE,close DOUBLE,volume DOUBLE,ret DOUBLE)")
    con.executemany("INSERT INTO cm_prices VALUES(?,?,10,100,.01)",[(sid,d) for sid in (1,2) for d in days])
    con.execute("CREATE TABLE cm_price_quality(security_id BIGINT,session_date DATE,available_at TIMESTAMPTZ,source_available_at TIMESTAMPTZ,shares DOUBLE,close_adjusted DOUBLE,high_adjusted DOUBLE,price_certified BOOLEAN,return_certified BOOLEAN,corwin_schultz_spread DOUBLE)")
    con.executemany("INSERT INTO cm_price_quality VALUES(?,?,?,?,1000,10,11,true,true,.01)",[(sid,d,source,source) for sid in (1,2) for d in days])
    con.execute("CREATE TABLE cm_daily_factors(session_date DATE,available_at TIMESTAMPTZ,rf DOUBLE,mkt_rf DOUBLE,smb DOUBLE,hml DOUBLE,q_size DOUBLE,q_ia DOUBLE,q_roe DOUBLE)")
    con.execute("CREATE TABLE cm_monthly_factors(month_end DATE,available_at TIMESTAMPTZ,rf DOUBLE,mkt_rf DOUBLE,smb DOUBLE,hml DOUBLE)")
    con.execute("CREATE TABLE cm_monthly_market(security_id BIGINT,month_end DATE,available_at TIMESTAMPTZ,source_available_at TIMESTAMPTZ,shares_adjusted DOUBLE,issuer_me DOUBLE,dividend_dollars DOUBLE,return_certified BOOLEAN,ret DOUBLE)")
    return date


def test_native_month_full_keys_natural_values_and_clock(con):
    date=fixture(con)
    out=M.native_month(con,date)
    assert out["rows"]==2
    rows=N.records(con,"SELECT be_me,gp_at,ni_me,quality_reasons,available_at,source_available_at FROM cm_output")
    assert all(r["be_me"]==.6 and r["gp_at"]==.7 and r["ni_me"]==.15 for r in rows)
    assert all(r["source_available_at"]<r["available_at"]==N.clock(date) for r in rows)
    assert "eq_dur" in json.loads(rows[0]["quality_reasons"])
    assert out["counts"]["univ_r3000p"]["fields"]["eq_dur"]=={"numerator":0,"denominator":2}


@pytest.mark.parametrize("bad",[0,-1,float("nan"),float("inf")])
def test_all_eligible_classes_invalidate_issuer_basis(con,bad):
    date=fixture(con,invalid_me=bad)
    cap,_=N.cap(con,10,date)
    assert cap["me"] is None


def test_null_eligible_class_invalidates_no_drop(con):
    date=fixture(con)
    con.execute("UPDATE cm_universe SET me=NULL WHERE security_id=2")
    assert N.cap(con,10,date)[0]["me"] is None


def test_delayed_restatement_and_equal_cutoff_exclusion(con):
    date=fixture(con)
    a,_=N.accounting(con,10,date,"funda")
    assert a[0]["at"]==100
    con.execute("INSERT INTO cm_funda SELECT cik,datadate,fyear,'future',?,curcd,999,seq,pstk,txditc,sale,cogs,ni,ib FROM cm_funda",[N.clock(date)])
    again,_=N.accounting(con,10,date,"funda")
    assert again==a
    con.execute("UPDATE cm_primitive_quality SET source_available_at=? WHERE item='at'",[N.clock(date)])
    assert N.accounting(con,10,date,"funda")[0][0]["at"] is None


def test_missing_history_native_and_original_coverage_grid(con):
    date=fixture(con,dt.date(2023,5,31))
    out=M.native_month(con,date)
    assert con.execute("SELECT count(*) FROM cm_output WHERE ret_12_1 IS NOT NULL OR ivol_capm_21d IS NOT NULL").fetchone()[0]==0
    receipt={date.isoformat():{"measurement":out}}
    evidence=M.coverage(receipt)
    assert len(evidence)==18
    assert evidence["2020/univ_r3000p"]["months"]==0
    gates=M.gates(evidence)
    assert gates["accounting_coverage"]["pass"] is False
    assert gates["price_coverage"]["pass"] is False
    assert gates["implemented_original153"]["denominator"]==153


def test_failclosed_unqualified_generation_and_reduced_window(con,tmp_path):
    with pytest.raises(ValueError,match="incomplete"):N.admit(tmp_path,{"version":"char_monthly_bindings_v1","products":{}},con)
    with pytest.raises(ValueError,match="cannot be reduced"):
        M.build(tmp_path,{},"fixture",dt.date(2021,1,1),dt.date(2025,12,31))
    with pytest.raises(ValueError,match="safe explicit"):
        M.build(tmp_path,{},"../escape",dt.date(2020,1,1),dt.date(2025,12,31))


def test_public_unit_rejects_null_publication_clock(con):
    date=fixture(con)
    M.native_month(con,date)
    con.execute("UPDATE cm_output SET available_at=NULL")
    with pytest.raises(RuntimeError,match="clock"):M.validate_unit(con,"cm_output",date)


def test_original_window_and_month_inventory_cannot_be_reduced():
    dates=[dt.date(y,m,28) for y in range(2020,2026) for m in range(1,13)]
    M.required_window(dt.date(2020,1,1),dt.date(2025,12,31),dates)
    with pytest.raises(ValueError,match="incomplete"):M.required_window(dt.date(2020,1,1),dt.date(2025,12,31),dates[1:])
    with pytest.raises(ValueError,match="reduced"):M.required_window(dt.date(2021,1,1),dt.date(2025,12,31),dates)


def test_public_generation_rejects_self_consistent_abbreviated_window(con,tmp_path,monkeypatch):
    from atx_db.alpha_panel.char_monthly_registry import FREEZE_SHA256
    generation=tmp_path/"candidate";generation.mkdir()
    manifest={"stage":M.STAGE,"schema":M.SCHEMA,"status":"complete","registry_sha256":FREEZE_SHA256,"source_quality_certified":False,"readiness":"NOT_PRODUCTION_READY"}
    snapshot={"source":{},"bindings":{},"registry_sha256":FREEZE_SHA256,"start":"2020-01-01","end":"2025-12-31"}
    (generation/"manifest.json").write_text(json.dumps(manifest))
    (generation/"snapshot.json").write_text(json.dumps(snapshot))
    monkeypatch.setattr(N,"admit",lambda *args:{})
    monkeypatch.setattr(N,"months",lambda *args:[dt.date(2022,6,30)])
    with pytest.raises(ValueError,match="inventory incomplete"):M.validate_generation(tmp_path,generation,con)


def test_r21_native_2023_trailing_predictor_and_strict_clocks(con):
    date=fixture(con,dt.date(2023,6,30));tau=N.clock(date);source=tau-dt.timedelta(days=1)
    for i in range(13):
        n=2022*12+5+i;y,m=divmod(n,12)
        end=(dt.date(y+1,1,1) if m==11 else dt.date(y,m+2,1))-dt.timedelta(days=1)
        while end.weekday()>=5:end-=dt.timedelta(days=1)
        if end!=date:con.execute('INSERT INTO cm_calendar VALUES(?)',[end])
        con.execute('INSERT INTO cm_monthly_market VALUES(1,?,?,?,1000,100,1,true,.01)',[end,source,source])
    bars,monthly,_=N.price_history(con,1,date)
    from atx_db.alpha_panel import char_monthly_kernels as K
    assert K.market('ret_12_1',bars,monthly,date,{})==pytest.approx(1.01**11-1)
    M.native_month(con,date)
    assert con.execute('SELECT ret_12_1 FROM cm_output WHERE security_id=1').fetchone()[0]==pytest.approx(1.01**11-1)
    con.execute('UPDATE cm_monthly_market SET source_available_at=? WHERE month_end=?',[tau,date])
    assert N.price_history(con,1,date)[1][-1]['ret'] is None
    con.execute('UPDATE cm_monthly_market SET source_available_at=?,available_at=? WHERE month_end=?',[source,tau,date])
    with pytest.raises(K.Missing,match='current_month'):K.market('ret_12_1',*N.price_history(con,1,date)[:2],date,{})
