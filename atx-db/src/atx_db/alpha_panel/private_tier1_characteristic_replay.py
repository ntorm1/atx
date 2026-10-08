"""Bounded connection-taking native daily-characteristic phase orchestration.

Input paths come from the fixed independently admitted original-source contract.
No legacy publisher, global replacement, source discovery or destructive cleanup.
All native members/buckets/months and registry expressions are preserved.
"""
import datetime as dt
from pathlib import Path
import pyarrow.parquet as pq
from . import characteristics as N, char_registry as R, common as C
from . import private_tier1_admission as A


def ident(value):return '"'+str(value).replace('"','""')+'"'


def input_query(panel,borrow,calendar,repairs=None,market=None,columns=N.PANEL_COLUMNS):
    if repairs is None:return N.input_sql(panel,borrow,columns=columns,market_rel=market)
    # Native input_sql's repair branch names a global calendar path. This exact
    # projection binds the explicitly admitted calendar without altering globals.
    repaired='q.repair_date=p.session_date';unmasked=set(N.KEYS)|set(N.FLAGS)
    selected=columns;columns=[]
    for column in selected:
        if column=='mkt_ret' and market:
            value=f'CASE WHEN {repaired} THEN NULL ELSE mk.mkt_ret END'
        elif column in unmasked:value='p.'+ident(column)
        elif column=='ret_guarded':value=f'CASE WHEN {repaired} THEN TRUE ELSE p.'+ident(column)+' END'
        else:value=f'CASE WHEN {repaired} THEN NULL ELSE p.'+ident(column)+' END'
        columns.append(value+' AS '+ident(column))
    for source,(column,kind) in N.BP_COLUMNS.items():
        value=f'CASE WHEN {repaired} THEN NULL ELSE b.{source} END' if borrow else f'CAST(NULL AS {kind})'
        columns.append(value+' AS '+ident(column))
    columns.extend([f'coalesce({repaired},false) AS source_repaired_now',
        f'coalesce(c.sidx-q.repair_sidx<={N.REPAIR_LOOKBACK},false) AS source_repaired_window'])
    join=f'LEFT JOIN {borrow} b USING(session_date,security_id)' if borrow else ''
    join+=f' JOIN (SELECT session_date,row_number() OVER(ORDER BY session_date) sidx FROM {A.sql([calendar])}) c USING(session_date) ASOF LEFT JOIN {repairs} q ON p.security_id=q.security_id AND p.session_date>=q.repair_date'
    if market:join+=' LEFT JOIN '+market+' mk USING(session_date)'
    return 'SELECT '+','.join(columns)+' FROM '+panel+' p '+join


def clean_market_query(panel,repairs,calendar):
    """Original prior-member recipe with the admitted calendar bound explicitly.

    The native helper invokes its global-calendar repair projection internally;
    this sequence supplies that projection without changing the native helper.
    """
    inp=input_query(panel,None,calendar,repairs,columns=(*N.KEYS,'member','ret','ret_guarded'))
    return f'''WITH cal AS (SELECT session_date,row_number() OVER(ORDER BY session_date) AS sidx
        FROM {A.sql([calendar])}),
        p AS (SELECT i.session_date,i.security_id,i.member,i.ret,i.ret_guarded,i.source_repaired_window,c.sidx
            FROM ({inp}) i JOIN cal c USING(session_date)),
        prior AS (SELECT *,lag(member) OVER w AS previous_member,lag(sidx) OVER w AS previous_sidx,
            lag(source_repaired_window) OVER w AS previous_repaired_window
            FROM p WINDOW w AS(PARTITION BY security_id ORDER BY sidx)),
        eligible AS (SELECT *,coalesce(previous_member AND previous_sidx=sidx-1,false) AS prior_member_present,
            coalesce(source_repaired_window OR previous_repaired_window,false) AS uncertain_identity FROM prior)
        SELECT session_date,avg(ret) FILTER(WHERE prior_member_present AND NOT uncertain_identity
            AND NOT coalesce(ret_guarded,false) AND isfinite(ret) AND ret>-1) AS mkt_ret,
            count(*) FILTER(WHERE prior_member_present AND NOT uncertain_identity
            AND NOT coalesce(ret_guarded,false) AND isfinite(ret) AND ret>-1) AS mkt_n,
            count(*) FILTER(WHERE prior_member_present AND uncertain_identity) AS excluded_identity_n
        FROM eligible GROUP BY session_date'''


def replay(con,products,target,years):
    """Replay complete native cells; caller owns full source/code admission."""
    target=Path(target);target.mkdir(parents=True,exist_ok=False)
    features=tuple(R.REGISTRY)
    if N.lint_build(features):raise ValueError('original native characteristic PIT lint failed')
    panel=A.sql(products['panel']);borrow=A.sql(products['borrow']) if products.get('borrow') else None
    calendar=products['calendar'];repair=A.sql(products['repairs']) if products.get('repairs') else None
    market=None
    if repair:
        con.execute('CREATE OR REPLACE TABLE t17_char_market AS '+clean_market_query(panel,repair,calendar))
        market='t17_char_market'
    con.execute('CREATE OR REPLACE TABLE t17_char_input AS '+input_query(panel,borrow,calendar,repair,market))
    history=None
    if products.get('history'):
        history=N.guarded_history_sql(con,A.sql(products['history']))
        con.execute('CREATE OR REPLACE TABLE t17_char_history AS SELECT * FROM ('+history+f") WHERE session_date<DATE '{N.PANEL_START}'")
        history='t17_char_history'
    daily=f'(SELECT session_date,security_id,ret,ret_guarded FROM t17_char_input WHERE security_id={N.MKT_ID}'
    if history:daily+=f' UNION ALL SELECT session_date,security_id,ret,ret_guarded FROM {history} WHERE security_id={N.MKT_ID}'
    daily+=')'
    con.execute('CREATE OR REPLACE TABLE t17_char_mkt_month AS '+N.mkt_month_sql(daily,str(calendar)))
    lines=[]
    for bucket in range(N.NB):
        inp=f'(SELECT * FROM t17_char_input WHERE security_id%{N.NB}={bucket})'
        hist=f'(SELECT * FROM {history} WHERE security_id%{N.NB}={bucket})' if history else None
        query,columns=N.line_query(con,inp,hist,'t17_char_mkt_month',str(calendar),features,tag='t17_char')
        path=target/'lines'/f'bucket={bucket}.parquet'
        C.copy_to_parquet(con,'SELECT '+','.join(ident(column) for column in columns)+' FROM ('+query+')'+
            f" WHERE session_date BETWEEN DATE '{min(years)}-01-01' AND DATE '{max(years)}-12-31'",path)
        lines.append(path)
    result={}
    for year in years:
        path=target/f'year={year}'/'characteristics.parquet';path.parent.mkdir(parents=True)
        writer=None
        try:
            for month in range(1,13):
                lo=dt.date(year,month,1);hi=dt.date(year+(month==12),month%12+1,1)-dt.timedelta(days=1)
                part=target/'months'/f'{year}-{month:02}.parquet'
                C.copy_to_parquet(con,N.year_sql(A.sql(lines),lo,hi,features)+' ORDER BY session_date,security_id',part)
                parquet=pq.ParquetFile(part)
                if writer is None:writer=pq.ParquetWriter(path,parquet.schema_arrow,compression='zstd')
                for batch in parquet.iter_batches(batch_size=64):writer.write_batch(batch)
        finally:
            if writer is not None:writer.close()
        result[path.relative_to(target).as_posix()]=A.pin(path)
    return result
