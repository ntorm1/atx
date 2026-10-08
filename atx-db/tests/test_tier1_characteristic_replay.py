"""Explicit-calendar native characteristic repair orchestration, synthetic only."""
import datetime as dt
import duckdb
from atx_db.alpha_panel import private_tier1_characteristic_replay as T


def test_explicit_calendar_repairs_preserve_prior_member_market(tmp_path):
    calendar=tmp_path/'calendar.parquet'
    with duckdb.connect(str(tmp_path/'native.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute('CREATE TABLE cal(session_date DATE)')
        dates=[dt.date(2020,1,2),dt.date(2020,1,3),dt.date(2020,1,6)]
        con.executemany('INSERT INTO cal VALUES(?)',[(date,) for date in dates])
        con.execute("COPY cal TO '"+calendar.as_posix()+"' (FORMAT PARQUET)")
        con.execute('CREATE TABLE panel(session_date DATE,security_id BIGINT,member BOOLEAN,ret DOUBLE,ret_guarded BOOLEAN)')
        con.executemany('INSERT INTO panel VALUES(?,?,true,?,false)',
            [(date,security,security*.01) for date in dates for security in (1,2)])
        con.execute('CREATE TABLE repairs(security_id BIGINT,repair_date DATE,repair_sidx BIGINT)')
        con.execute('INSERT INTO repairs VALUES(2,?,2)',[dates[1]])
        query=T.input_query('panel',None,calendar,'repairs',columns=('session_date','security_id','member','ret','ret_guarded'))
        con.execute('CREATE TABLE projected AS '+query)
        # No future repair exclusion; repair day masks the return and sets guard.
        assert con.execute('SELECT ret,ret_guarded,source_repaired_window FROM projected WHERE security_id=2 ORDER BY session_date').fetchall()==[
            (.02,False,False),(None,True,True),(.02,False,True)]
        rows=con.execute(T.clean_market_query('panel','repairs',calendar)+' ORDER BY session_date').fetchall()
        assert rows==[(dates[0],None,0,0),(dates[1],.01,1,1),(dates[2],.01,1,1)]
        assert con.execute("SELECT current_setting('memory_limit'),current_setting('threads')").fetchone()==('122.0 MiB',1)
