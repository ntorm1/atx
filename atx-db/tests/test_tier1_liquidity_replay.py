"""Exact original SQL oracle; no frozen globals/functions or actual-path IO."""
import ast
import datetime as dt
from pathlib import Path
import duckdb
from atx_db.alpha_panel import liquidity as L,market_common as M,common as C
from atx_db.alpha_panel import private_tier1_liquidity_replay as T


def original_sql_oracle(bucket,shares,calendar,b):
    # Native bucket_sql calls M.tmp.mkdir when formatting its global source path.
    # Render only its original SQL literal's allowlisted relation/constant holes;
    # do not call, replace or compile that frozen producer function.
    tree=ast.parse(Path(L.__file__).read_text())
    function=next(node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='bucket_sql')
    template=next(node.value for node in function.body if isinstance(node,ast.Return))
    holes={'M.bucket_path(b)':bucket,'shares':shares,'M.sub_filter(k)':'TRUE','M.calendar_sql()':calendar,
        'M.BUCKETS':M.BUCKETS,'b':b,'kc':repr(L.K),'MIN_PAIRS':L.MIN_PAIRS,
        'M.MARK':M.MARK,'C.WARMUP_START':C.WARMUP_START}
    result=[]
    for part in template.values:
        if isinstance(part,ast.Constant):result.append(part.value)
        else:
            assert isinstance(part,ast.FormattedValue) and part.conversion==-1 and part.format_spec is None
            result.append(str(holes[ast.unparse(part.value)]))
    return ''.join(result)


def test_explicit_liquidity_projection_complete_native_typed_equivalence(tmp_path):
    with duckdb.connect(str(tmp_path/'liquidity.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute('CREATE TABLE calendar(session_date DATE)')
        dates=[dt.date(2020,1,2)+dt.timedelta(days=index) for index in range(90)]
        con.executemany('INSERT INTO calendar VALUES(?)',[(date,) for date in dates])
        calendar='(SELECT session_date,CAST(row_number() OVER(ORDER BY session_date) AS INTEGER) sidx,lag(session_date) OVER(ORDER BY session_date) prev_session FROM calendar)'
        con.execute('CREATE TABLE buckets(security_id BIGINT,session_date DATE,sidx INTEGER,high DOUBLE,low DOUBLE,close DOUBLE,volume DOUBLE,dollar_volume DOUBLE,ret DOUBLE,ret_guarded BOOLEAN,fb_action VARCHAR,return_factor DOUBLE)')
        con.execute('CREATE TABLE shares(security_id BIGINT,session_date DATE,shrout DOUBLE)')
        for sid in (1,17):
            for index,date in enumerate(dates):
                con.execute('INSERT INTO shares VALUES(?,?,?)',[sid,date,None if index%13==0 else 1000])
                if index in (19,20):continue
                close=10+index*.01
                con.execute('INSERT INTO buckets VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                    [sid,date,index+1,close+1,close-1,close,0 if index==35 else 100,
                     0 if index==35 else close*100,.01,index==40,'repaired' if index==41 else None,
                     .5 if index==42 else 1.0])
        bucket=tmp_path/'buckets.parquet';shares=tmp_path/'shares.parquet'
        con.execute("COPY buckets TO '"+bucket.as_posix()+"' (FORMAT PARQUET)")
        con.execute("COPY shares TO '"+shares.as_posix()+"' (FORMAT PARQUET)")
        original=original_sql_oracle(bucket.as_posix(),shares.as_posix(),calendar,1)
        explicit=T.query("read_parquet('"+bucket.as_posix()+"')","read_parquet('"+shares.as_posix()+"')",calendar,1)
        assert con.execute('DESCRIBE ('+original+')').fetchall()==con.execute('DESCRIBE ('+explicit+')').fetchall()
        con.execute('CREATE TABLE old AS '+original);con.execute('CREATE TABLE new AS '+explicit)
        assert con.execute('SELECT count(*) FROM old').fetchone()[0]==180
        assert con.execute('SELECT count(*) FROM ((SELECT * FROM old EXCEPT ALL SELECT * FROM new) UNION ALL (SELECT * FROM new EXCEPT ALL SELECT * FROM old))').fetchone()[0]==0
        assert con.execute("SELECT has_bar,halt_proxy FROM new WHERE security_id=1 AND session_date=?",[dates[19]]).fetchone()==(False,True)
