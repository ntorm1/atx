"""Annual history/source-duration integration independent of Task3's retained clock regressions."""
import datetime as dt
from tests.test_alpha_panel_compustat_std import World,_build
from tests.test_alpha_panel_ratios import World as RatioWorld,SESSIONS
from atx_db.alpha_panel import ratios as R
from atx_db.alpha_panel import compustat_std as CS
import duckdb
import pyarrow.parquet as pq

D=dt.date;T=dt.datetime


def test_v11_missing_discontinued_and_extraordinary_facts_do_not_certify_ib(monkeypatch,tmp_path):
    w=World(tmp_path);pe=D(2020,3,31);clock=T(2020,5,1)
    w.ev(1,'q',clock,pe,'Q1',ni_q=20,cat={'do_ttm':0})
    w.fact(1,'NetIncomeLoss',D(2020,1,1),pe,20,'q')
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    w.write('fundamentals_v11')
    CS.build('fundamentals_v11','earn_x','cstd',buckets=2,build_date=D(2026,10,2))
    row=pq.read_table(tmp_path/'cstd/fundq.parquet').to_pylist()[0]
    assert row['niq']==20 and row['ibq'] is None
    assert row['ibq_src'] is None


def test_delayed_and_restated_reported_total_equity_preserves_prior_cutoff(monkeypatch,tmp_path):
    w=World(tmp_path);pe=D(2020,12,31);t0=T(2021,2,20);t1=T(2022,2,20);t2=T(2022,3,20)
    w.ev(1,'base',t0,pe,'FY','10-K',at=1000,lt=600)
    w.ev(1,'later',t1,D(2021,12,31),'FY','10-K')
    w.ev(1,'amend',t2,D(2021,12,31),'FY','10-K/A')
    w.hist(1,'teq',pe,400,t1,'later');w.hist(1,'teq',pe,410,t2,'amend')
    _build(monkeypatch,w)
    rows=sorted([r for r in pq.read_table(tmp_path/'cstd/funda.parquet').to_pylist() if r['datadate']==pe],key=lambda r:r['available_at'])
    assert [(r['available_at'],r['teq']) for r in rows]==[(t0,None),(t1,400),(t2,410)]
    with duckdb.connect() as con:
        inp=CS.Inputs(tmp_path/'fund_x',tmp_path/'earn_x',tmp_path/'cstd/_work/facts.parquet')
        # Read the build's actual prepared facts path from its work inventory.
        inp.facts=next((tmp_path/'cstd/_work').glob('*facts*.parquet'),None)
        CS.derive_subset(con,inp,'true',cutoff=t1)
        assert con.execute("SELECT teq FROM a_out WHERE datadate=DATE '2020-12-31'").fetchall()==[(None,)]

def test_direct_annual_source_is_not_quarter_delta_and_catalog_comparative_has_own_clock(monkeypatch,tmp_path):
    w=World(tmp_path);pe=D(2020,12,31);t0=T(2021,2,20);t1=T(2022,2,20)
    w.ev(1,'base',t0,pe,'FY','10-K',sale_ttm=400,sale_q=100,cat={'txditc':0,'re':10})
    w.ev(1,'later',t1,D(2021,12,31),'FY','10-K',sale_ttm=500,sale_q=100)
    for name,val in [('sale_q',100),('annual_sale_ttm',400),('txditc',0),('re',10)]:w.hist(1,name,pe,val,t0,'base')
    for name,val in [('sale_q',90),('annual_sale_ttm',390),('txditc',138703000),('re',15)]:w.hist(1,name,pe,val,t1,'later')
    w.fact(1,'NetIncomeLoss',D(2020,1,1),pe,20,'base')
    _,q,a=_build(monkeypatch,w)
    aa=sorted([r for r in pq.read_table(tmp_path/'cstd/funda.parquet').to_pylist() if r['datadate']==pe],key=lambda r:r['available_at'])
    assert [(r['sale'],r['txditc'],r['re']) for r in aa]==[(400,0,10),(390,138703000,15)]
    assert all(r['source_period_start']==D(2020,1,1) and r['source_full_year'] for r in aa)
    assert aa[0]['source_period_available_at']==t0

def test_source_duration_does_not_infer_full_year_from_adjacent_ends(monkeypatch,tmp_path):
    w=World(tmp_path);pe=D(2021,12,31);clock=T(2022,2,20)
    w.ev(1,'short',clock,pe,'FY','10-K',sale_ttm=100)
    w.fact(1,'NetIncomeLoss',D(2021,7,1),pe,20,'short')
    _,_,a=_build(monkeypatch,w)
    assert a['short']['source_period_start']==D(2021,7,1)
    assert a['short']['source_period_days']==184 and a['short']['source_full_year'] is False

def test_ratio_rejects_future_source_duration_clock_and_short_annual_period(tmp_path):
    w=RatioWorld(tmp_path)
    for year in (2018,2019,2020):
        end=D(year,12,31);clock=T(year+1,2,20)
        w.q(1,end,4,str(year),clock,form='10-K',saleq=100,atq=1000)
        w.a(1,end,str(year),clock,sale=400,ni=20,at=1000)
    w.fa[-1]['source_period_available_at']=T(2022,1,1)
    w.write(SESSIONS);con=duckdb.connect();R.build_splits(con,w.root)
    R.build_state_bucket(con,w.root/'compustat_std','true',tmp_path/'state.parquet')
    assert con.execute("SELECT count(*) FROM alag l JOIN a USING(arid) WHERE datadate=DATE '2020-12-31'").fetchone()[0]==0
    # Even after availability the actual short period fails certification.
    w.fa[-1]['source_period_available_at']=T(2021,2,20)
    w.fa[-1]['source_period_start']=D(2020,7,1);w.fa[-1]['source_full_year']=False
    w.write(SESSIONS);R.build_state_bucket(con,w.root/'compustat_std','true',tmp_path/'state2.parquet')
    assert con.execute("SELECT count(*) FROM alag l JOIN a USING(arid) WHERE datadate=DATE '2020-12-31'").fetchone()[0]==0
    con.close()
