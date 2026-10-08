"""Real native SQL/portfolio/regression replay on bounded synthetic sources."""
import datetime as dt
import math
import duckdb
import pytest
from atx_db.alpha_panel import _factor_native as N, private_tier1_factor_replay as T


def test_complete_native_unit_replay_keeps_failures_and_refuses_tampering(tmp_path):
    import pyarrow as pa
    import pyarrow.parquet as pq
    date=dt.date(2020,1,2)
    rows=[{'freq':'D','session_date':date,'available_at':dt.datetime(2020,1,2,22,tzinfo=dt.UTC),
        'n_universe':1,'n_qualified':0,'missing_reason':'missing_qualified_history_or_required_portfolio_leg',
        **{name:None for name in T.F.FACTORS}} for _ in range(131)]
    actual=tmp_path/'published.parquet';pq.write_table(pa.Table.from_pylist(rows,schema=N.FACTOR_SCHEMA),actual)
    with duckdb.connect(str(tmp_path/'units.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        result=T.compare_unit(con,iter(rows),N.FACTOR_SCHEMA,actual,tmp_path/'attempt'/'expected.parquet')
        assert result['rows']==131
        changed=[{**row,'n_qualified':1} if index==130 else row for index,row in enumerate(rows)]
        pq.write_table(pa.Table.from_pylist(changed,schema=N.FACTOR_SCHEMA),actual)
        with pytest.raises(ValueError,match='complete native factor/risk unit replay differs'):
            T.compare_unit(con,iter(rows),N.FACTOR_SCHEMA,actual,tmp_path/'failed-attempt'/'expected.parquet')
        assert (tmp_path/'failed-attempt'/'expected.parquet').exists()


def test_native_factor_and_risk_daily_clock_equivalence_file_backed(tmp_path):
    with duckdb.connect(str(tmp_path/'native.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        definitions={
            'funda':'cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seq DOUBLE,ceq DOUBLE,"at" DOUBLE,lt DOUBLE,txditc DOUBLE,pstk DOUBLE,sale DOUBLE,cogs DOUBLE,xsga DOUBLE,xint DOUBLE,revt DOUBLE,xrd DOUBLE,rect DOUBLE,invt DOUBLE,xpp DOUBLE,drc DOUBLE,drlt DOUBLE,ap DOUBLE,xacc DOUBLE,dltt DOUBLE,dlc DOUBLE,sic BIGINT',
            'fundq':'cik BIGINT,datadate DATE,available_at TIMESTAMP,accession VARCHAR,seqq DOUBLE,ceqq DOUBLE,atq DOUBLE,ltq DOUBLE,pstkq DOUBLE,ibq DOUBLE',
            'universe':"session_date DATE,security_id BIGINT,cik BIGINT,me DOUBLE,exchcd_like INTEGER,univ_all_common BOOLEAN,univ_r3000p BOOLEAN,available_at TIMESTAMP DEFAULT TIMESTAMP '1900-01-01'",
            'prices':'session_date DATE,security_id BIGINT,ret DOUBLE,ret_guarded BOOLEAN,close DOUBLE',
            'calendar':'session_date DATE','rf':'freq VARCHAR,session_date DATE,rf DOUBLE,available_at TIMESTAMPTZ',
            'cap_quality':N.EMPTY['cap_quality'],'eg_training':N.EMPTY['eg_training'],
            'member_equity':'session_date DATE,security_id BIGINT',
            'liquidity':'security_id BIGINT,session_date DATE,available_at TIMESTAMP,adv_21 DOUBLE,adv_63 DOUBLE,spread_cs_21 DOUBLE,amihud_21 DOUBLE',
            'borrow':'security_id BIGINT,session_date DATE,cutoff_utc TIMESTAMPTZ,si_to_io DOUBLE,si_shares DOUBLE,ftd_quantity DOUBLE,on_threshold_list BOOLEAN,si_to_io_available_at TIMESTAMPTZ,si_available_at TIMESTAMPTZ,ftd_available_at TIMESTAMPTZ,threshold_available_at TIMESTAMPTZ,ftd_settlement_date DATE,threshold_list_date DATE',
            'industry':'cik BIGINT,available_at TIMESTAMP,ff49_no BIGINT',
            'ratios_state':'cik BIGINT,available_at TIMESTAMP,be_q0 DOUBLE,at_q0 DOUBLE,at_q4 DOUBLE,debt_at DOUBLE,gprof DOUBLE,datadate DATE'}
        for name,definition in definitions.items():con.execute('CREATE TABLE t6_'+name+'('+definition+')')
        dates=[dt.date(2022,1,3)+dt.timedelta(days=index) for index in range(361)]
        dates=[date for date in dates if date.weekday()<5]
        for index,date in enumerate(dates):
            con.execute('INSERT INTO t6_calendar VALUES(?)',[date])
            con.execute('INSERT INTO t6_universe(session_date,security_id,cik,me,exchcd_like,univ_all_common,univ_r3000p) VALUES(?,1,1,100,1,true,true)',[date])
            con.execute('INSERT INTO t6_prices VALUES(?,1,?,false,10)',[date,.01*math.sin(index*.17)+.001])
            con.execute("INSERT INTO t6_cap_quality VALUES(1,?,TIMESTAMP '1900-01-01',TIMESTAMP '1900-01-01',true,true,'fixture','independently_verified','e','s','m')",[date])
            con.execute("INSERT INTO t6_rf VALUES('D',?,.0001,?)",[date,dt.datetime.combine(date,dt.time(22),dt.UTC)])
        # Only pure native source kernels are used; no producer/function/global
        # override and no source qualification claim follows this fixture.
        rows=[];spec={'sources':{'eg_training':{'fixture':True}}}
        for year,month in sorted({(date.year,date.month) for date in dates}):
            rows.extend(T.month_rows(con,spec,dates,year,month))
        daily={row['session_date']:row for row in rows if row['freq']=='D'}
        assert set(daily)==set(dates)
        assert daily[dates[1]]['MktRF']==pytest.approx(.01*math.sin(.17)+.001-.0001)
        assert daily[dates[0]]['MktRF'] is None
        con.execute('CREATE TABLE t6_own_factors(freq VARCHAR,session_date DATE,available_at TIMESTAMPTZ,MktRF DOUBLE,SMB DOUBLE,HML DOUBLE,RMW DOUBLE,CMA DOUBLE)')
        con.executemany('INSERT INTO t6_own_factors VALUES(?,?,?,?,?,?,?,?)',[(row['freq'],row['session_date'],row['available_at'],row['MktRF'],row['SMB'],row['HML'],row['RMW'],row['CMA']) for row in rows if row['freq']=='D'])
        result=list(T.risk_day_rows(con,dates[-1],dates))
        assert len(result)==1 and result[0]['beta_252']==pytest.approx(1)
        assert result[0]['ivol_capm_252']<1e-12
        assert result[0]['available_at']==N._cutoff(dates[-1])
        assert result[0]['ivol_ff5_252'] is None # missing original factor legs retained
