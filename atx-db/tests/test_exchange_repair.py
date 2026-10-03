"""Frozen-panel FINRA exchange repairs align codes, market and rank-date eligibility."""
import datetime as dt
import pyarrow as pa
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, security_master as S, identity_v3 as V, indexes as X, market_index as M
from tests.test_universe_stage import make_lake, CAL


def test_sc_bzx_supported_unknown_stale_and_same_day_unmapped(tmp_path, monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    make_lake(tmp_path)
    path=tmp_path/'panel/year=2021/panel-01.parquet'
    rows=pq.read_table(path).to_pylist()
    for r in rows:
        r.update(ni_ttm=1.0,ni_q=1.0,ret=0.01,ret_guarded=False,is_issuer_primary=True,member_equity=True)
        if r['security_id'] in (2,4,5,11):r['exchange']=None
    pq.write_table(pa.Table.from_pylist(rows),path)
    names=[dict(security_id=s,dissemination_date=d,market_class=k,issue_name='Operating Inc')
           for s,d,k in ((2,dt.date(2021,5,25),'SC'),(4,dt.date(2021,5,25),'BZX'),
                         (5,dt.date(2021,5,25),'UNSUPPORTED'),(11,dt.date(2021,4,1),'SC'),
                         (11,CAL[0],'SC'))]
    pq.write_table(pa.Table.from_pylist(names),tmp_path/'security_master/finra_names.parquet')
    con=C.connect(memory='150MB',threads=1)
    got=con.execute(f"SELECT security_id,exchange FROM ({S.listed_panel_sql(path.as_posix())}) WHERE session_date=DATE '{CAL[0]}'").fetchall()
    got=dict(got)
    assert got[2]=='XNAS' and got[4]=='BATS' and got[5] is None and got[11] is None and got[1]=='XNYS'
    V.codes()
    assert con.execute(f"SELECT DISTINCT exchcd FROM read_parquet('{tmp_path.as_posix()}/security_master/share_exchange_history.parquet') WHERE security_id=2").fetchall()==[(3,)]
    # price/shares screens are independent: lift price for BZX; give unsupported line shares.
    for r in rows:
        if r['security_id']==4:r['raw_close']=5.0
    pq.write_table(pa.Table.from_pylist(rows),path)
    liq=tmp_path/'market/liquidity/year=2021/liquidity.parquet';liq.parent.mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist([dict(session_date=d,security_id=s,adv_63=1e7) for d in CAL for s in range(1,15)]),liq)
    M.load_universe(con,{})
    market={r[0] for r in con.execute(f"SELECT security_id FROM u WHERE session_date=DATE '{CAL[0]}'").fetchall()}
    ranked={r['security_id'] for r in X.snapshot(con,CAL[0])}
    assert {2,4}<=market and {2,4}<=ranked and 5 not in market and 11 not in market
    assert 5 not in ranked and 11 not in ranked
    con.close()


def test_market_holdout_filters_before_aggregation(tmp_path, monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    make_lake(tmp_path)
    path=tmp_path/'panel/year=2021/panel-01.parquet'
    rows=pq.read_table(path).to_pylist()
    for r in rows:r.update(ret=0.01,ret_guarded=False,is_issuer_primary=True,member_equity=True)
    # A held-out row with an extreme return must never reach the aggregation table.
    later=dict(rows[0],session_date=dt.date(2023,1,3),ret=999.0)
    pq.write_table(pa.Table.from_pylist(rows+[later]),path)
    con=C.connect(memory='150MB',threads=1)
    M.load_universe(con,{})
    assert con.execute("SELECT max(session_date) FROM u").fetchone()[0] < dt.date(2023,1,1)
    M.build_index(con,{})
    out=(tmp_path/'market/index_returns.parquet').as_posix()
    assert con.execute(f"SELECT count(*) FROM read_parquet('{out}') WHERE period_end>=DATE '2023-01-01'").fetchone()[0]==0
    import pytest
    with pytest.raises(ValueError,match='D6'):M.load_universe(con,{},dt.date(2023,1,1))
    con.close()
