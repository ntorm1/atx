"""R26 bounded fixed-stage original market replay and time-relative admission.

Native legacy products remain diagnostics. Qualified cells derive their complete
source clocks and economic basis separately; future classifications and anchors
never become historical evidence merely through a matching output hash.
"""
from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import math
import os
import re
import tempfile
from pathlib import Path

from . import common as C, prices as P, market_common as M, shares_daily as SH
from . import corporate_actions as CA, liquidity as L, borrow_proxy as B
from . import security_master as SM, identity_table as IT, panel as PN, identity_names as IN, identity_backfill as IB, identity_links as IL
from . import private_tier1_family_sources as A
from . import private_tier1_liquidity_replay as LR

VERSION='tier1-market-sources-v1'
NAMES=('prices','planned_calendar','shares','liquidity','borrow')
DEPENDENCIES={'prices':(), 'planned_calendar':(), 'shares':('prices',),
              'liquidity':('prices','shares'), 'borrow':('prices','shares','liquidity')}
NATIVE_ROLES=('security_master','identity')


def _native_security_master(con,*,vendor,si,raw_finra,directory_files,namespace):
    """Original full-schema security-master projection, never PIT qualification.

    The caller must authenticate every original vendor/SI/raw/directory source
    and the native SI replay. Snapshot classifications and inferred delisting
    dates remain native diagnostics; they never supply dated economic proof.
    """
    from ..research import spine
    from ..universe_us_listed import classify_security_type
    import pyarrow as pa
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):
        raise ValueError('fixed security-master namespace required')
    if len(directory_files)!=2 or tuple(Path(p).name for p in directory_files)!=('nasdaqlisted.txt','otherlisted.txt'):
        raise ValueError('both fixed original Nasdaq directory files required in native order')
    files=tuple(A.absolute(p) for p in directory_files)
    for path in files:
        if path.stat().st_size>4*1024*1024:raise ValueError('bounded native directory source exceeded')
    directory=spine.read_directory(files=files)
    raw=_table(con,namespace+'_raw_names',f'''SELECT symbolCode AS symbol,
        CAST(settlementDate AS DATE) AS settlement_date,any_value(issueName) AS issue_name,
        any_value(marketClassCode) AS market_class FROM {raw_finra}
        WHERE symbolCode IS NOT NULL AND settlementDate IS NOT NULL GROUP BY 1,2''')
    function_names=(namespace+'_classify_finra',namespace+'_share_class')
    registered={row[0] for row in con.execute('SELECT function_name FROM duckdb_functions()').fetchall()}
    if any(name in registered for name in function_names):raise ValueError('native classification namespace occupied')
    created=[]
    try:
        for name,function,args in ((function_names[0],SM.classify_finra,['VARCHAR','VARCHAR']),
                                  (function_names[1],SM.share_class_of,['VARCHAR'])):
            con.create_function(name,function,args,'VARCHAR',null_handling='special');created.append(name)
        names=_table(con,namespace+'_finra_names',f'''SELECT s.security_id,s.settlement_date,s.dissemination_date,
            CAST(s.dissemination_date AS TIMESTAMP)+INTERVAL 22 HOUR AS available_at,s.symbol,r.issue_name,
            coalesce(r.market_class,s.market_class) AS market_class,
            {SM.exchange_case('coalesce(r.market_class,s.market_class)')} AS exchange,
            {function_names[0]}(r.issue_name,s.symbol) AS finra_type,
            {function_names[1]}(r.issue_name) AS share_class FROM {si} s LEFT JOIN {raw} r
            USING(symbol,settlement_date) WHERE s.security_id IS NOT NULL ORDER BY s.security_id,s.settlement_date''')
    finally:
        for name in reversed(created):con.remove_function(name)
    first,last=con.execute(f'SELECT min(tradingDate),max(tradingDate) FROM {vendor}').fetchone()
    if first is None or last is None:raise ValueError('complete native vendor line population is empty')
    base=_table(con,namespace+'_line_base',f'''SELECT securityID AS security_id,
        min(tradingDate) AS first_session,max(tradingDate) AS last_session,
        arg_max(ticker_tk,tradingDate) AS last_ticker,count(*) AS vendor_rows
        FROM {vendor} WHERE securityID>0 GROUP BY 1''')
    view=namespace+'_directory_arrow';directory_table=namespace+'_directory_rows'
    if con.execute('SELECT count(*) FROM duckdb_tables() WHERE table_name IN (?,?)',[view,directory_table]).fetchone()[0] or con.execute(
            'SELECT count(*) FROM duckdb_views() WHERE view_name=?',[view]).fetchone()[0]:
        raise ValueError('native directory namespace occupied')
    def append_matches(rows,first_batch):
        snapshot=pa.table({'security_id':[r[0] for r in rows],'directory_name':[r[1] for r in rows],
            'directory_etf':[r[2] for r in rows],'directory_type':[r[3] for r in rows]})
        con.register(view,snapshot)
        try:
            if first_batch:_table(con,directory_table,f'SELECT * FROM {view}')
            else:con.execute(f'INSERT INTO {directory_table} SELECT * FROM {view}')
        finally:con.unregister(view)
    matched=[];have_directory=False
    marker=None
    while True:
        predicate='' if marker is None else ' AND security_id>?'
        parameters=[] if marker is None else [marker]
        batch=con.execute(f"SELECT security_id,last_ticker FROM {base} WHERE last_session>=DATE '{last}'-INTERVAL 14 DAY"
                          +predicate+' ORDER BY security_id LIMIT 64',parameters).fetchall()
        if not batch:break
        marker=batch[-1][0]
        for sid,ticker in batch:
            entry=next((directory[v] for v in spine.symbol_variants(ticker or '') if v in directory),None)
            if entry is not None:
                matched.append((sid,entry['name'],entry['etf']=='Y',classify_security_type(entry['name'],
                    etf=entry['etf']=='Y',test_issue=entry['test_issue']=='Y')))
                if len(matched)==64:
                    append_matches(matched,not have_directory);have_directory=True;matched=[]
    # Retain the original Arrow inference even for an empty snapshot match.
    if matched or not have_directory:append_matches(matched,not have_directory)
    lines=_table(con,namespace+'_lines',f'''SELECT l.security_id,l.first_session,l.last_session,
            l.first_session=DATE '{first}' AS left_censored,
            CASE WHEN l.last_session<DATE '{SM.DEAD_BEFORE}' THEN l.last_session END AS delisting_date,
            l.last_ticker,l.vendor_rows,d.directory_name,d.directory_etf,d.directory_type,
            l.security_id>={SM.INDEX_ID_MIN} AS is_index_line FROM {base} l LEFT JOIN {directory_table} d
            USING(security_id) ORDER BY 1''')
    return {'security_master/lines.parquet':lines,'security_master/finra_names.parquet':names}


def _native_identity(con,*,core,combined,name_days,si,raw_finra,calendar,members,namespace):
    """Full original identity_table projection on explicit admitted relations.

    This preserves original primary/name/backfill/ever-member diagnostics. Its
    results are never the causal raw strict/name census used by core shares or
    Task1. All seven original upstream roles need separate complete admission.
    """
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):
        raise ValueError('fixed native identity namespace required')
    days=namespace+'_days'
    _table(con,days,'''SELECT CAST(NULL AS DATE) AS session_date,CAST(NULL AS BIGINT) AS security_id,
        CAST(NULL AS BIGINT) AS cik,CAST(NULL AS VARCHAR) AS link_tier,CAST(NULL AS VARCHAR) AS basis,
        CAST(NULL AS TIMESTAMP) AS link_available_at,CAST(NULL AS DOUBLE) AS adv63,
        CAST(NULL AS VARCHAR) AS ticker WHERE false''')
    for year in range(2017,2027):
        con.execute(f'''INSERT INTO {days} SELECT c.session_date,c.security_id,l.cik,
            CASE WHEN l.tier IN ('backfill','name') THEN l.tier ELSE 'strict' END,l.basis,
            CASE WHEN l.tier='name' THEN coalesce(n.available_at,l.available_at) ELSE l.available_at END,
            c.adv63,c.ticker FROM(SELECT session_date,security_id,ticker,adv63,is_operating,is_index
            FROM {core} WHERE year(session_date)={year}) c
            JOIN(SELECT * FROM {combined} WHERE start<=DATE '{year}-12-31' AND end_incl>=DATE '{year}-01-01') l
            ON l.security_id=c.security_id AND c.session_date BETWEEN l.start AND l.end_incl
            LEFT JOIN(SELECT * FROM {name_days} WHERE year(session_date)={year}) n
            ON n.security_id=c.security_id AND n.session_date=c.session_date AND n.cik=l.cik''')
    fr=_table(con,namespace+'_fr',f'''SELECT DISTINCT s.security_id,s.dissemination_date,
        regexp_extract(r.issueName,'Class ([A-Z])( |$)',1) AS cls FROM {si} s
        JOIN(SELECT DISTINCT symbolCode,CAST(settlementDate AS DATE) AS settlement_date,issueName
        FROM {raw_finra} WHERE issueName IS NOT NULL) r
        ON r.symbolCode=s.symbol AND r.settlement_date=s.settlement_date WHERE s.security_id IS NOT NULL''')
    fr_cls=_table(con,namespace+'_fr_cls',f"SELECT security_id,dissemination_date,CASE WHEN cls='' THEN NULL ELSE cls END AS cls FROM {fr}")
    days2=_table(con,namespace+'_days2',f'''SELECT d.*,
        coalesce(CASE WHEN regexp_matches(d.ticker,'^[A-Z]+\\.[A-Z]$') THEN right(d.ticker,1) END,f.cls) AS share_class,
        row_number() OVER(PARTITION BY d.cik,d.session_date ORDER BY d.adv63 DESC NULLS LAST,d.security_id)=1 AS is_issuer_primary,
        count(*) OVER(PARTITION BY d.cik,d.session_date) AS issuer_lines FROM {days} d
        ASOF LEFT JOIN {fr_cls} f ON d.security_id=f.security_id AND d.session_date>f.dissemination_date''')
    islands=_table(con,namespace+'_islands',f'''WITH cal AS(SELECT session_date,
        row_number() OVER(ORDER BY session_date) AS k FROM {calendar}),x AS(
        SELECT d.*,c.k,c.k-row_number() OVER(PARTITION BY d.security_id,d.cik,d.link_tier,d.basis,
        d.is_issuer_primary,coalesce(d.share_class,'') ORDER BY c.k) AS grp FROM {days2} d JOIN cal c USING(session_date))
        SELECT security_id,cik,link_tier,basis,is_issuer_primary,share_class,min(session_date) AS valid_from,
        max(session_date) AS valid_to,count(*) AS sessions,min(link_available_at) AS link_available_at,
        max(issuer_lines) AS max_issuer_lines FROM x GROUP BY security_id,cik,link_tier,basis,is_issuer_primary,share_class,grp''')
    ever=_table(con,namespace+'_ever',f'SELECT DISTINCT security_id FROM {members} WHERE member')
    output=_table(con,namespace+'_link_table',f'''SELECT i.security_id,i.cik,i.valid_from,i.valid_to,i.sessions,
        i.link_tier,i.basis,i.is_issuer_primary,i.share_class,i.max_issuer_lines,i.link_available_at AS available_at,
        CASE WHEN i.link_tier='backfill' THEN CAST(i.valid_from AS TIMESTAMP)+INTERVAL 22 HOUR
        ELSE i.link_available_at END AS evidence_at,e.security_id IS NOT NULL AS ever_member
        FROM {islands} i LEFT JOIN {ever} e USING(security_id) ORDER BY i.security_id,i.valid_from''')
    return {'identity/link_table.parquet':output}


class _HtmlNode:
    """Small stdlib original HTML tree; no undeclared parser dependency."""
    def __init__(self,tag='',attrs=()):
        self.tag=tag;self.attrs=dict(attrs);self.children=[]
    def get(self,key,default=None):return self.attrs.get(key,default)
    def find_all(self,names,recursive=True):
        names={names} if isinstance(names,str) else set(names)
        result=[]
        for child in self.children:
            if isinstance(child,_HtmlNode):
                if child.tag in names:result.append(child)
                if recursive:result.extend(child.find_all(names))
        return result
    def get_text(self,separator='',strip=False):
        parts=[]
        for child in self.children:
            value=child.get_text(separator,strip) if isinstance(child,_HtmlNode) else child
            if strip:value=value.strip()
            if value:parts.append(value)
        return separator.join(parts)


def _html(body_path):
    from html.parser import HTMLParser
    class Tree(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.root=_HtmlNode();self.stack=[self.root]
        def handle_starttag(self,tag,attrs):
            node=_HtmlNode(tag,attrs);self.stack[-1].children.append(node)
            if tag not in {'area','base','br','col','embed','hr','img','input','link','meta','param','source','track','wbr'}:
                self.stack.append(node)
        def handle_startendtag(self,tag,attrs):
            self.stack[-1].children.append(_HtmlNode(tag,attrs))
        def handle_endtag(self,tag):
            for i in range(len(self.stack)-1,0,-1):
                if self.stack[i].tag==tag:
                    del self.stack[i:];return
        def handle_data(self,value):self.stack[-1].children.append(value)
    parser=Tree();parser.feed(A._blob(body_path).decode('utf-8',errors='strict'));parser.close()
    return parser.root


def calendar_query(vendor):
    return f"SELECT tradingDate AS session_date,count(*) AS vendor_rows,count(DISTINCT securityID) AS vendor_ids FROM {vendor} GROUP BY 1 HAVING count(*)>={C.MIN_ROWS_PER_SESSION} ORDER BY 1"


def project_query(vendor,project_start):
    BUCKETS=P.BUCKETS
    iv_cols=', '.join(f'atmCenI_{t}d AS iv_raw_{t}d' for t in P.IV_TENORS)
    return f"""
        SELECT tradingDate AS session_date, securityID AS security_id, ticker_tk AS ticker,
               CAST(open AS DOUBLE) AS open, CAST(high AS DOUBLE) AS high, CAST(low AS DOUBLE) AS low,
               CAST(close AS DOUBLE) AS close, CAST(closeUnadjPr AS DOUBLE) AS prev_raw_close_vendor,
               CAST(returnFactor AS DOUBLE) AS return_factor, CAST(totalReturn AS DOUBLE) AS total_return_vendor,
               CAST(cumulReturnFactor AS DOUBLE) AS cumul_factor, volume, shares AS shares_thousands,
               earnFlag AS earn_flag, GICS AS gics, {iv_cols},
               CAST(securityID % {BUCKETS} AS INTEGER) AS bucket
        FROM {vendor}
        WHERE tradingDate >= DATE '{project_start}' AND securityID IS NOT NULL AND tradingDate IS NOT NULL
    """


def steps_query(sources,calendar):
    return f"""
        WITH raw AS (
            SELECT * FROM {sources}
            WHERE session_date IN (SELECT session_date FROM {calendar})
              AND security_id > 0  -- securityID 0 is the vendor's catch-all; see repair_sid0
        ),
        keyed AS (
            SELECT *,
                   count(*) FILTER (WHERE NOT repaired) OVER (PARTITION BY security_id, session_date) AS key_orig,
                   count(*) FILTER (WHERE repaired) OVER (PARTITION BY security_id, session_date) AS key_rep
            FROM raw
        ),
        obs AS (
            SELECT * EXCLUDE (key_orig, key_rep) FROM keyed
            WHERE (NOT repaired AND key_orig = 1) OR (repaired AND key_orig = 0 AND key_rep = 1)
        ),
        lagged AS (
            SELECT *,
                   lag(close) OVER w AS prev_close, lag(session_date) OVER w AS prev_date
            FROM obs
            WHERE close > 0 AND isfinite(close)
            WINDOW w AS (PARTITION BY security_id ORDER BY session_date)
        )
        SELECT *,
               CASE WHEN return_factor > 0 AND isfinite(return_factor) THEN -ln(return_factor) END AS s,
               CASE WHEN prev_close > 0 THEN ln(close / prev_close) END AS r,
               date_diff('day', prev_date, session_date) AS gap_days
        FROM lagged
    """


def finalize_query(steps,mass,project_start):
    in_mass=('session_date IN ('+', '.join(f"DATE '{d}'" for d in mass)+')') if mass else 'FALSE'
    ln_split=math.log(P.SPLIT_RATIO)
    NOISE,MAX_GAP_DAYS,SHARES_ROW_CEILING=P.NOISE,P.MAX_GAP_DAYS,P.SHARES_ROW_CEILING
    iv_sel=', '.join(f'CASE WHEN iv_raw_{t}d >= {P.IV_DOMAIN[0]} AND iv_raw_{t}d <= {P.IV_DOMAIN[1]} THEN CAST(iv_raw_{t}d AS DOUBLE) END AS iv_atm_{t}d' for t in P.IV_TENORS)
    return f"""
        WITH st AS ({steps}),
        cls AS (
            SELECT *,
                CASE
                    WHEN NOT ({in_mass}) OR s IS NULL OR abs(s) <= {NOISE} OR prev_close IS NULL THEN 'none'
                    WHEN gap_days > {MAX_GAP_DAYS} THEN 'kept_gap'
                    WHEN (s < 0 AND r >= greatest(abs(s) / 2, {ln_split})) OR (s > 0 AND -r >= greatest(s / 2, {ln_split}))
                        THEN 'kept_split_follow'
                    WHEN s > 0 AND s < {ln_split} THEN 'kept_distribution'
                    ELSE 'repaired'
                END AS fb_action
            FROM st
        ),
        src AS (
            SELECT *,
                CASE WHEN prev_close IS NULL THEN NULL
                     WHEN fb_action = 'repaired' THEN 'repaired_raw'
                     WHEN prev_raw_close_vendor > 0 AND abs(prev_raw_close_vendor / prev_close - 1) <= 0.001
                          AND isfinite(total_return_vendor) AND total_return_vendor > -1 THEN 'vendor'
                     ELSE 'observed' END AS ret_source
            FROM cls
        ),
        rets AS (
            SELECT *,
                CASE WHEN ret_source = 'repaired_raw' THEN close / prev_close - 1
                     WHEN ret_source = 'vendor' THEN total_return_vendor
                     WHEN ret_source = 'observed' THEN
                          close / (prev_close * CASE WHEN return_factor > 0 AND isfinite(return_factor) THEN return_factor ELSE 1 END) - 1
                END AS ret_all
            FROM src
        ),
        chained AS (
            SELECT *,
                sum(CASE WHEN prev_close IS NULL THEN 0 ELSE ln(1 + ret_all) END)
                    OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS cum_log,
                last_value(close) OVER (PARTITION BY security_id ORDER BY session_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS anchor_close,
                sum(CASE WHEN prev_close IS NULL THEN 0 ELSE ln(1 + ret_all) END)
                    OVER (PARTITION BY security_id) AS total_log
            FROM rets
        )
        SELECT session_date, security_id, ticker, open, high, low, close,
               CASE WHEN prev_close IS NULL THEN NULL ELSE ret_all END AS ret,
               CASE WHEN prev_close IS NULL THEN NULL
                    ELSE (NOT isfinite(ln(1 + ret_all)) OR abs(ln(1 + ret_all)) > 1.5 OR abs(ln(1 + ret_all)) > abs(r) + 0.10)
               END AS ret_guarded,
               anchor_close * exp(cum_log - total_log) AS adj_close,
               prev_close AS prev_raw_close, gap_days,
               volume, close * volume AS dollar_volume,
               CASE WHEN shares_thousands > 0 AND shares_thousands <= {SHARES_ROW_CEILING}
                    THEN CAST(shares_thousands AS DOUBLE) * 1000.0 END AS shares_vendor,
               shares_thousands > {SHARES_ROW_CEILING} AS shares_above_ceiling,
               return_factor, cumul_factor, total_return_vendor, fb_action, ret_source, repaired AS sid0_repaired,
               earn_flag, NULLIF(gics, '') AS gics, {iv_sel}
        FROM chained
        WHERE session_date >= DATE '{project_start}'
    """


def shares_query(bucket,splits,links,sec,eqcnt,line_type,b,k=None):
    lo,hi=SH.DOMAIN
    condition=M.sub_filter(k)
    MATCH_BEFORE_DAYS,MATCH_AFTER_DAYS=SH.MATCH_BEFORE_DAYS,SH.MATCH_AFTER_DAYS
    VENDOR_LAG_DAYS,SEC_STALE_DAYS,CONFLICT_RATIO=SH.VENDOR_LAG_DAYS,SH.SEC_STALE_DAYS,SH.CONFLICT_RATIO
    return f"""
    WITH ld AS (
        SELECT security_id, session_date, prev_session, shares_vendor,
               CAST(prev_session AS TIMESTAMP) + {M.MARK} AS prev_mark
        FROM {bucket} WHERE {condition}
    ),
    spl AS (SELECT * FROM {splits} WHERE security_id % {M.BUCKETS} = {b}),
    cs AS (
        SELECT ld.*, coalesce(s.lsr, 0.0) AS lsr_d,
               sum(coalesce(s.lsr, 0.0)) OVER w AS csf,
               max(CASE WHEN s.lsr IS NOT NULL THEN ld.session_date END) OVER w AS last_split
        FROM ld LEFT JOIN spl s ON s.security_id = ld.security_id AND s.ex_date = ld.session_date
        WINDOW w AS (PARTITION BY ld.security_id ORDER BY ld.session_date ROWS UNBOUNDED PRECEDING)
    ),
    lagged AS (
        SELECT *,
               last_value(shares_vendor IGNORE NULLS) OVER wp AS pv,
               last_value(CASE WHEN shares_vendor IS NOT NULL THEN csf END IGNORE NULLS) OVER wp AS pcsf
        FROM cs
        WINDOW wp AS (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING)
    ),
    vch AS (
        SELECT security_id, session_date AS c, shares_vendor AS s, csf AS csf_c FROM lagged
        WHERE shares_vendor BETWEEN {lo} AND {hi}
          AND (pv IS NULL OR abs(shares_vendor / (pv * exp(csf - pcsf)) - 1) > 1e-6)
    ),
    vcik AS (
        SELECT v.*, r.cik FROM vch v
        LEFT JOIN (SELECT DISTINCT security_id, cik, valid_from, valid_to FROM {links}
                   WHERE security_id % {M.BUCKETS} = {b}) r
          ON r.security_id = v.security_id AND v.c BETWEEN r.valid_from AND r.valid_to
    ),
    vmatch AS (
        SELECT v.security_id, v.c, any_value(v.s) AS s, any_value(v.csf_c) AS csf_c, min(e.clock_utc) AS match_clock
        FROM vcik v LEFT JOIN {sec} e
          ON e.cik = v.cik AND e.filed >= v.c - {MATCH_BEFORE_DAYS} AND e.filed <= v.c + {MATCH_AFTER_DAYS}
         AND abs(e.shrs_q - v.s) < greatest(1000.5, 1e-4 * v.s)
        GROUP BY 1, 2
    ),
    vclock AS (
        SELECT *, match_clock IS NOT NULL AS matched,
               CASE WHEN match_clock IS NOT NULL THEN greatest(match_clock, CAST(c AS TIMESTAMP) + {M.MARK})
                    ELSE CAST(c AS TIMESTAMP) + INTERVAL {VENDOR_LAG_DAYS} DAY + {M.MARK} END AS clock_v
        FROM vmatch
    ),
    vbest AS (
        SELECT security_id, clock_v, best_s, best_c, best_csf, best_clock, best_matched FROM (
            SELECT security_id, clock_v, c,
                   arg_max(s, c) OVER w AS best_s, max(c) OVER w AS best_c, arg_max(csf_c, c) OVER w AS best_csf,
                   arg_max(clock_v, c) OVER w AS best_clock, arg_max(matched, c) OVER w AS best_matched
            FROM vclock
            WINDOW w AS (PARTITION BY security_id ORDER BY clock_v, c ROWS UNBOUNDED PRECEDING))
        QUALIFY row_number() OVER (PARTITION BY security_id, clock_v ORDER BY c DESC) = 1
    ),
    lk AS (
        SELECT cs.*, r.cik FROM cs
        LEFT JOIN (SELECT * FROM {links} WHERE security_id % {M.BUCKETS} = {b}) r
          ON r.security_id = cs.security_id AND cs.session_date BETWEEN r.valid_from AND r.valid_to
         AND r.known_at <= CAST(cs.session_date AS TIMESTAMP) + {M.MARK}
        QUALIFY row_number() OVER (PARTITION BY cs.security_id, cs.session_date ORDER BY r.known_at) = 1
    ),
    fs AS (
        SELECT lk.*, e.shrs_q, e.shrs_src, e.filed, e.clock_utc AS sec_clock
        FROM lk ASOF LEFT JOIN {sec} e ON lk.cik = e.cik AND lk.prev_mark > e.clock_utc
    ),
    fa AS (
        SELECT fs.*, c2.csf AS csf_filed
        FROM fs ASOF LEFT JOIN (SELECT security_id, session_date AS sd, csf FROM cs) c2
          ON fs.security_id = c2.security_id AND fs.filed >= c2.sd
    ),
    va AS (
        SELECT fa.*, v.best_s, v.best_c, v.best_csf, v.best_clock, v.best_matched
        FROM fa ASOF LEFT JOIN vbest v ON fa.security_id = v.security_id AND fa.prev_mark > v.clock_v
    ),
    cand AS (
        SELECT va.*, n.n AS issuer_equity_lines, lt.line_type,
               CASE WHEN shrs_q IS NOT NULL AND session_date - filed <= {SEC_STALE_DAYS}
                    THEN shrs_q * exp(csf - coalesce(csf_filed, 0.0)) END AS sec_v,
               best_s * exp(csf - best_csf) AS ven_v
        FROM va ASOF LEFT JOIN {eqcnt} n ON va.cik = n.cik AND va.session_date >= n.change_date
        LEFT JOIN {line_type} lt ON lt.security_id = va.security_id
    ),
    comp AS (
        SELECT *, coalesce(shrs_src = 'dei' AND issuer_equity_lines = 1 AND coalesce(line_type, '') <> 'ADR', false)
                  AS comparable
        FROM cand
    ),
    pick AS (
        SELECT *,
               comparable AND sec_v IS NOT NULL AND ven_v IS NOT NULL
                   AND abs(ln(sec_v / ven_v)) > ln({CONFLICT_RATIO}) AS conflict,
               comparable AND sec_v BETWEEN {lo} AND {hi}
                   AND (ven_v IS NULL OR (filed >= best_c AND abs(ln(sec_v / ven_v)) <= ln({CONFLICT_RATIO})))
                   AS use_sec
        FROM comp
    )
    SELECT security_id, session_date, cik,
           CASE WHEN use_sec THEN sec_v WHEN ven_v BETWEEN {lo} AND {hi} THEN ven_v END AS shrout,
           CASE WHEN use_sec THEN 'sec_' || shrs_src WHEN ven_v BETWEEN {lo} AND {hi}
                THEN CASE WHEN best_matched THEN 'vendor_sec_matched' ELSE 'vendor_lag' END END AS shrout_source,
           CASE WHEN use_sec THEN filed WHEN ven_v BETWEEN {lo} AND {hi} THEN best_c END AS shrout_asof,
           CASE WHEN use_sec THEN sec_clock WHEN ven_v BETWEEN {lo} AND {hi} THEN best_clock END AS obs_available_at,
           CASE WHEN use_sec THEN exp(csf - coalesce(csf_filed, 0.0))
                WHEN ven_v BETWEEN {lo} AND {hi} THEN exp(csf - best_csf) END AS split_adj,
           last_split,
           sec_v AS shares_sec, shrs_src AS shares_sec_source, filed AS shares_sec_asof,
           ven_v AS shares_vendor_pit, best_c AS shares_vendor_asof, best_matched AS shares_vendor_matched,
           CASE WHEN shares_vendor BETWEEN {lo} AND {hi} THEN shares_vendor END AS shares_vendor_current,
           issuer_equity_lines, comparable AS shares_sec_line_level, conflict AS shrout_conflict
    FROM pick
    WHERE session_date >= DATE '{C.WARMUP_START}'
    """


def shares_output_query(sql):
    return f"""SELECT * EXCLUDE(last_split,obs_available_at),obs_available_at,
        CASE WHEN last_split IS NOT NULL AND last_split>shrout_asof
             THEN greatest(obs_available_at,CAST(last_split AS TIMESTAMP)+{M.MARK})
             ELSE obs_available_at END AS available_at FROM ({sql})"""


def known_link_query(links):
    known="CASE WHEN link_tier='backfill' THEN evidence_at ELSE available_at END"
    first=f"CAST(({known})-INTERVAL 22 HOUR AS DATE)+CASE WHEN ({known})-INTERVAL 22 HOUR>CAST(CAST(({known})-INTERVAL 22 HOUR AS DATE) AS TIMESTAMP) THEN 1 ELSE 0 END"
    return f"SELECT security_id,cik,valid_from,valid_to,link_tier,{known} AS known_at,greatest(valid_from,{first}) AS eff_from FROM {links}"


def _sql(paths):
    if isinstance(paths,(str,Path)):
        paths=[paths]
    if not paths:
        raise ValueError('complete fixed market input required')
    return 'read_parquet(['+','.join("'"+str(Path(p).as_posix()).replace("'","''")+"'" for p in paths)+'],hive_partitioning=false)'


def _json(path):
    def pairs(values):
        result={}
        for key,value in values:
            if key in result:raise ValueError('duplicate original market metadata key')
            result[key]=value
        return result
    return json.loads(A._blob(path),object_pairs_hook=pairs)


def _clock(value):
    if not isinstance(value,str):raise ValueError('actual original offset clock required')
    parsed=dt.datetime.fromisoformat(value.replace('Z','+00:00'))
    if parsed.tzinfo is None:raise ValueError('market clock requires original explicit offset')
    return parsed.astimezone(dt.timezone.utc)


def _date(value):
    if not isinstance(value,str) or not re.fullmatch(r'\d{4}-\d{2}-\d{2}',value):
        raise ValueError('original market calendar date required')
    return dt.date.fromisoformat(value)


def _owner(value):
    if type(value) is not int or value<=0:
        raise ValueError('strict original positive security identifier required')
    return value


def _compare(con,sql,path):
    actual=f'SELECT * FROM {_sql(path)}'
    if con.execute('DESCRIBE '+sql).fetchall()!=con.execute('DESCRIBE '+actual).fetchall():
        raise ValueError('complete native market typed schema differs')
    if (con.execute(f'SELECT count(*) FROM (({sql}) EXCEPT ALL ({actual}))').fetchone()[0] or
        con.execute(f'SELECT count(*) FROM (({actual}) EXCEPT ALL ({sql}))').fetchone()[0]):
        raise ValueError('complete duplicate-preserving native market replay differs')


def _table(con,name,sql):
    if not re.fullmatch(r't17_market_[a-z0-9_]+',name):raise ValueError('fixed private market relation required')
    if con.execute('SELECT count(*) FROM duckdb_tables() WHERE table_name=?',[name]).fetchone()[0]:
        raise ValueError('retained market work relation requires controller reconciliation')
    con.execute(f'CREATE TEMP TABLE {name} AS {sql}')
    return name


def repair_query(project):
    """Original SID0 bracket populations and evidence, with no frozen scratch IO."""
    return f'''WITH nz AS(SELECT DISTINCT ticker,session_date,security_id FROM {project}
        WHERE security_id>0 AND ticker IS NOT NULL),
    z AS(SELECT * EXCLUDE(bucket) FROM(SELECT *,count(*) OVER(PARTITION BY session_date,ticker) AS n_same
        FROM {project} WHERE security_id=0 AND ticker IS NOT NULL) WHERE n_same=1),
    nz_dedup AS(SELECT ticker,session_date,min(security_id) AS sid,count(*) AS n FROM nz GROUP BY 1,2),
    zb AS(SELECT z.*,p.sid AS prev_sid,p.session_date AS prev_date,p.n AS prev_n FROM z
        ASOF LEFT JOIN nz_dedup p ON z.ticker=p.ticker AND z.session_date>p.session_date),
    zbb AS(SELECT zb.*,q.sid AS next_sid,q.session_date AS next_date,q.n AS next_n FROM zb
        ASOF LEFT JOIN(SELECT ticker,-epoch(session_date) AS neg_t,session_date,sid,n FROM nz_dedup) q
        ON zb.ticker=q.ticker AND -epoch(zb.session_date)>q.neg_t)
    SELECT *,prev_sid IS NOT NULL AND prev_sid=next_sid AND prev_n=1 AND next_n=1
        AND date_diff('day',prev_date,session_date)<={P.REPAIR_MAX_BRACKET_DAYS}
        AND date_diff('day',session_date,next_date)<={P.REPAIR_MAX_BRACKET_DAYS} AS mapped FROM zbb'''


def replay_prices(con,vendor,project_start,write_from,namespace='t17_market'):
    project_start,write_from=_date(project_start),_date(write_from)
    if write_from<project_start:raise ValueError('original native price writing interval differs')
    if not re.fullmatch(r't17_market(?:_[a-z0-9_]+)?',namespace):raise ValueError('fixed private market replay namespace required')
    calendar=_table(con,namespace+'_calendar',calendar_query(vendor))
    project=_table(con,namespace+'_project',project_query(vendor,project_start))
    bracket=_table(con,namespace+'_brackets',repair_query(project))
    original=f'SELECT * EXCLUDE(bucket),false AS repaired FROM {project}'
    # z already excludes bucket; the explicit schema below avoids schema guessing.
    columns=[r[0] for r in con.execute(f'DESCRIBE {project}').fetchall() if r[0] not in ('security_id','bucket')]
    repaired=f"SELECT prev_sid AS security_id,{','.join(columns)},true AS repaired FROM {bracket} WHERE mapped"
    sources=f'({original} UNION ALL BY NAME {repaired})'
    steps=_table(con,namespace+'_steps',steps_query(sources,calendar))
    mass=[r[0] for r in con.execute(f'''SELECT session_date FROM {steps} GROUP BY 1
        HAVING count(*) FILTER(WHERE abs(s)>{P.CELL_STEP} AND abs(r+s)>abs(r)+{P.CELL_EXCESS})>={P.MASS_MIN_CELLS}
        ORDER BY 1''').fetchall()]
    final=_table(con,namespace+'_prices',finalize_query(f'SELECT * FROM {steps}',mass,project_start))
    return {'calendar':calendar,'project':project,'brackets':bracket,'prices':final,'mass_sessions':list(map(str,mass)),
            'write_from':write_from,'source_rows':con.execute(f'SELECT count(*) FROM {vendor}').fetchone()[0]}


def replay_legacy_prices(con,native_root,vendor_path,project_start,write_from,namespace='t17_market'):
    """Authenticate the actual legacy manifest and every annual typed output.

    This is a native replay boundary, not economic/source qualification. The
    legacy envelope deliberately has no fabricated status or schema fields.
    Original capture/economic/full corpus admission is an additional stage duty.
    """
    root=A.absolute(native_root);vendor=A.absolute(vendor_path)
    if any(p.is_file() and p.name.endswith('.partial') for p in (root/'prices').rglob('*')) or (root/'calendar.parquet.partial').exists():
        raise ValueError('retained interrupted legacy price publication requires controller reconciliation')
    manifest_path=root/'prices'/'manifest.json';manifest=_json(manifest_path)
    if manifest.get('stage')!='prices' or manifest.get('rule')!=P.RULE:
        raise ValueError('actual legacy native price contract differs')
    source=manifest.get('source')
    if source!=C.file_identity(vendor):raise ValueError('actual legacy native vendor identity differs')
    years=manifest.get('years')
    if not isinstance(years,dict) or not years or any(
        not re.fullmatch(r'\d{4}',y) or type(n) is not int or n<0 for y,n in years.items()):
        raise ValueError('complete actual legacy annual price population required')
    expected={f'prices/year={y}/prices.parquet' for y in years}
    actual={p.relative_to(root).as_posix() for p in (root/'prices').rglob('*.parquet')}
    if actual!=expected:raise ValueError('actual legacy price publication leaves incomplete or extra')
    replay=replay_prices(con,_sql(vendor),project_start,write_from,namespace)
    _compare(con,f"SELECT * FROM {replay['calendar']}",root/'calendar.parquet')
    lo,hi=con.execute(f"SELECT min(session_date),max(session_date) FROM {replay['prices']}").fetchone()
    if lo is None or set(years)!={str(y) for y in range(max(replay['write_from'].year,lo.year),hi.year+1)}:
        raise ValueError('complete native annual axis does not replay original source')
    products={'calendar.parquet':root/'calendar.parquet'}
    for year,count in sorted(years.items()):
        key=f'prices/year={year}/prices.parquet';path=root/key
        start=max(dt.date(int(year),1,1),replay['write_from'])
        sql=f"SELECT * FROM {replay['prices']} WHERE session_date>=DATE '{start}' AND session_date<=DATE '{year}-12-31'"
        _compare(con,sql,path)
        if con.execute(f'SELECT count(*) FROM ({sql})').fetchone()[0]!=count:
            raise ValueError('actual native annual manifest row denominator differs')
        products[key]=path
    if manifest.get('last_session')!=hi.isoformat():raise ValueError('actual native endpoint does not replay source')
    pins={str(manifest_path):A.pin(manifest_path),str(vendor):A.pin(vendor)}
    pins.update({str(p):A.pin(p) for p in products.values()})
    return replay,products,pins


def _copy_immutable(source,dest):
    """Copy exact authenticated native bytes without rewriting their schema."""
    source=A.absolute(source);before=A.pin(source)
    dest=Path(dest);dest.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as incoming,dest.open('xb') as outgoing:
        while block:=incoming.read(1024*1024):outgoing.write(block)
        outgoing.flush();os.fsync(outgoing.fileno())
    if A.pin(source)!=before or A.pin(dest)!=before:
        raise ValueError('native role payload changed during immutable copy')
    return before


def _native_role(target,role,products,dependency_pins,replay_validation):
    """Publish a genuine replayed role payload; never alias a legacy envelope.

    Only the fixed complete stage orchestration may call this after replay and
    source admission. It neither creates cap_basis_audit nor declares economic,
    issuer-completeness, membership or historical clock qualification.
    """
    if role not in ('prices','prices_history','market_shares',*NATIVE_ROLES):
        raise ValueError('fixed original native market role required')
    if not products or not replay_validation:
        raise ValueError('complete original typed replay and source dependencies required')
    _check(dependency_pins)
    directory=Path(target)/'native'/role
    if directory.exists():raise ValueError('retained native role publication requires controller reconciliation')
    directory.mkdir(parents=True)
    files={}
    for key,path in sorted(products.items()):
        relative=Path(key)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('fixed native payload relative path required')
        files[relative.as_posix()]=_copy_immutable(path,directory/relative)['sha256']
    manifest={'status':'complete','stage':role,'schema':'atx.tier1-market-native-role/v1',
              'files':files,'source_dependency_pins':dependency_pins,
              'native_replay_validation':replay_validation,'code_pins':code_pins(),
              'economic_qualification':'separate original qualified-cell evidence; no cap audit supplied'}
    manifest_path=directory/'manifest.json'
    with manifest_path.open('x',encoding='utf-8') as stream:
        json.dump(manifest,stream,sort_keys=True,separators=(',',':'),default=str,allow_nan=False)
        stream.flush();os.fsync(stream.fileno())
    _check(dependency_pins)
    return {'manifest':str(manifest_path),'sha256':A.pin(manifest_path)['sha256'],
            'files':list(files),'product_pins':{k:A.pin(directory/k) for k in files}}


def replay_share_preparation(con,links,names,events,namespace='t17_market'):
    if not re.fullmatch(r't17_market(?:_[a-z0-9_]+)?',namespace):raise ValueError('fixed share preparation namespace required')
    line_type=_table(con,namespace+'_line_type',f'SELECT security_id,mode(finra_type) AS line_type FROM {names} GROUP BY 1')
    known=_table(con,namespace+'_links',known_link_query(links))
    bad=','.join("'"+kind+"'" for kind in SH.NON_EQUITY_TYPES)
    runs=_table(con,namespace+'_runs',f'''SELECT r.* FROM {known} r LEFT JOIN {line_type} t USING(security_id)
        WHERE coalesce(t.line_type NOT IN({bad}),true) AND r.eff_from<=r.valid_to''')
    eqcnt=_table(con,namespace+'_eqcnt',f'''SELECT cik,d AS change_date,
        CAST(sum(delta) OVER(PARTITION BY cik ORDER BY d ROWS UNBOUNDED PRECEDING) AS INTEGER) AS n
        FROM(SELECT cik,d,sum(delta) AS delta FROM(SELECT cik,eff_from AS d,1 AS delta FROM {runs}
        UNION ALL SELECT cik,valid_to+1 AS d,-1 AS delta FROM {runs}) GROUP BY 1,2)''')
    sec=_table(con,namespace+'_sec',f'''SELECT cik,clock_utc,filed,shrs_q,shrs_src FROM {events}
        WHERE shrs_q>0 AND clock_utc IS NOT NULL AND cik IS NOT NULL''')
    return known,line_type,eqcnt,sec


def _asof_share_inputs(con,*,links,names,events,cutoff,namespace):
    """Private causal fitting cohort; inputs must come from original admission.

    Unlike the diagnostic preparation, this filters *all* fitting inputs before
    selection. The fixed stage must supply complete authenticated source-derived
    relations and prove their census coverage independently. This phase alone
    neither proves absence nor qualifies its output.
    """
    cutoff=_clock(cutoff)
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):
        raise ValueError('fixed private market relation namespace required')
    stamp="TIMESTAMPTZ '"+cutoff.isoformat()+"'"
    # No future full-history modal type, backfill or future SEC matching document
    # is allowed to influence these fitting populations, even indirectly.
    available_names=_table(con,namespace+'_names',f'''SELECT * FROM {names}
        WHERE source_available_at IS NOT NULL AND source_available_at<={stamp}
          AND observation_date<=CAST({stamp} AS DATE)''')
    line_type=_table(con,namespace+'_line_type',f'''WITH counts AS (
        SELECT security_id,finra_type,count(*) AS n FROM {available_names}
        WHERE finra_type IS NOT NULL GROUP BY 1,2),
        best AS (SELECT *,max(n) OVER(PARTITION BY security_id) AS largest FROM counts)
        SELECT security_id,CASE WHEN count(*)=1 THEN min(finra_type) END AS line_type,
               count(*) AS modal_ties FROM best WHERE n=largest GROUP BY 1''')
    admitted_links=_table(con,namespace+'_link_facts',f'''SELECT * EXCLUDE(available_at),
        greatest(available_at,source_available_at) AS available_at FROM {links}
        WHERE link_tier IN ('strict','name') AND available_at IS NOT NULL
          AND available_at<={stamp} AND source_available_at IS NOT NULL
          AND source_available_at<={stamp}''')
    known=_table(con,namespace+'_links',known_link_query(admitted_links))
    bad=','.join("'"+kind+"'" for kind in SH.NON_EQUITY_TYPES)
    runs=_table(con,namespace+'_runs',f'''SELECT r.* FROM {known} r
        JOIN {line_type} t USING(security_id)
        WHERE t.modal_ties=1 AND t.line_type NOT IN({bad}) AND r.eff_from<=r.valid_to''')
    # Retain unresolved lines for the full census check. Their exclusion from
    # fitting is not proof that an issuer has only the remaining equity line.
    unresolved=_table(con,namespace+'_unresolved',f'''SELECT r.security_id,r.cik,
        r.valid_from,r.valid_to,CASE WHEN t.security_id IS NULL
        THEN 'missing_dated_classification' ELSE 'ambiguous_dated_classification' END AS reason
        FROM {known} r LEFT JOIN {line_type} t USING(security_id)
        WHERE t.security_id IS NULL OR t.modal_ties<>1''')
    eqcnt=_table(con,namespace+'_eqcnt',f'''SELECT cik,d AS change_date,
        CAST(sum(delta) OVER(PARTITION BY cik ORDER BY d ROWS UNBOUNDED PRECEDING) AS INTEGER) AS n
        FROM(SELECT cik,d,sum(delta) AS delta FROM(
        SELECT cik,eff_from AS d,1 AS delta FROM {runs}
        UNION ALL SELECT cik,valid_to+1 AS d,-1 AS delta FROM {runs}) GROUP BY 1,2)''')
    sec=_table(con,namespace+'_sec',f'''SELECT cik,
        greatest(clock_utc,source_available_at) AS clock_utc,filed,shrs_q,shrs_src FROM {events}
        WHERE shrs_q>0 AND cik IS NOT NULL AND clock_utc IS NOT NULL AND clock_utc<={stamp}
          AND source_available_at IS NOT NULL AND source_available_at<={stamp}''')
    return {'links':known,'line_type':line_type,'eqcnt':eqcnt,'sec':sec,
            'unresolved_lines':unresolved,'cutoff':cutoff}


def original_observation(receipt_path,body_path,source_url):
    """Conservative original capture floor; body dates never replace observation."""
    record=_json(receipt_path)
    body=A.pin(body_path)
    if record.get('status')=='FOUR_ORIGINAL_SOURCE_CAPTURES_NOT_ADMISSION':
        matches=[r for r in record['records'] if r.get('url')==source_url and
                 str(A.absolute(r.get('path','')))==str(A.absolute(body_path))]
        if len(matches)!=1:raise ValueError('complete distributed original capture inventory differs')
        record=matches[0]
        if (record.get('capture_complete') is not True or record.get('successful_response') is not True or
            type(record.get('status_code')) is not int or record['status_code']!=200 or
            record.get('sha256')!=body['sha256'] or record.get('bytes')!=body['bytes']):
            raise ValueError('distributed original capture result/body differs')
        return _clock(record['finished_at_utc'])
    if record.get('status')=='FIVE_NYSE_PUBLISHER_CAPTURES_NOT_SOURCE_ADMISSION':
        matches=[r for r in record['records'] if r.get('url')==source_url and
                 str(A.absolute(r['path']))==str(A.absolute(body_path))]
        if len(matches)!=1:raise ValueError('complete original NYSE capture receipt identity differs')
        record=matches[0]
        if (record.get('completed') is not True or type(record.get('status_code')) is not int or
            record['status_code']!=200 or record.get('sha256')!=body['sha256'] or record.get('bytes')!=body['bytes']):
            raise ValueError('original NYSE capture result/body differs')
        return _clock(record['finished_at_utc'])
    if (record.get('url')!=source_url or not source_url.startswith('https://') or
        type(record.get('http_status')) is not int or record['http_status']!=200 or
        record.get('sha256_of_raw_bytes')!=body['sha256'] or record.get('bytes')!=body['bytes']):
        raise ValueError('original market capture receipt/body binding differs')
    return _clock(record['fetched_at'])


def _literal_document(body_path,receipt_path,source_url,expression):
    observed=original_observation(receipt_path,body_path,source_url)
    text=A._blob(body_path).decode('utf-8',errors='strict')
    # Parse the full original supported statement. Never select an affirmative
    # quote out of contradictory or unparsed document context.
    text=re.sub(r'\s+',' ',text).strip()
    matched=re.fullmatch(expression,text)
    if not matched:raise ValueError('unsupported complete original market documentary context')
    return matched.groupdict(),observed


def original_identity(body,receipt,url):
    fields,clock=_literal_document(body,receipt,url,
        r'ORATS securityID (?P<sid>[1-9][0-9]*) represents ticker (?P<ticker>[A-Za-z0-9./_-]+) in the (?P<namespace>equity|index) namespace\. '
        r'Issuer CIK (?P<cik>[0-9]{1,10})\. Effective from (?P<start>\d{4}-\d{2}-\d{2}) through (?P<end>\d{4}-\d{2}-\d{2})\.')
    start,end=_date(fields['start']),_date(fields['end'])
    if end<start:raise ValueError('original identity interval reversed')
    return {'security_id':int(fields['sid']),'ticker':fields['ticker'],'cik':int(fields['cik']),
            'namespace':fields['namespace'],'is_index':fields['namespace']=='index','valid_from':start,'valid_to':end,
            'available_at':clock,'source_sha256':A.pin(body)['sha256'],'source_document':str(body)}


def original_issuer_lines(body,receipt,url):
    fields,clock=_literal_document(body,receipt,url,
        r'Issuer CIK (?P<cik>[0-9]{1,10}) has exactly these listed equity lines: (?P<lines>[1-9][0-9]*(?:,[1-9][0-9]*)*)\. '
        r'No other listed equity class is outstanding\. Effective from (?P<start>\d{4}-\d{2}-\d{2}) through (?P<end>\d{4}-\d{2}-\d{2})\.')
    ids=list(map(int,fields['lines'].split(',')))
    start,end=_date(fields['start']),_date(fields['end'])
    if len(ids)!=len(set(ids)) or end<start:raise ValueError('original issuer line census duplicated or reversed')
    for sid in ids:
        yield {'cik':int(fields['cik']),'security_id':sid,'issuer_equity_lines':len(ids),'valid_from':start,'valid_to':end,
               'available_at':clock,'source_sha256':A.pin(body)['sha256'],'source_document':str(body)}


def original_share_basis(body,receipt,url):
    fields,clock=_literal_document(body,receipt,url,
        r'ORATS securityID (?P<sid>[1-9][0-9]*) ticker (?P<ticker>[A-Za-z0-9./_-]+) represents (?P<class>common stock|ADR)\. '
        r'Shares outstanding are denominated in (?P<unit>common shares|depositary receipts) for this line\. '
        r'Effective from (?P<start>\d{4}-\d{2}-\d{2}) through (?P<end>\d{4}-\d{2}-\d{2})\.')
    start,end=_date(fields['start']),_date(fields['end'])
    if end<start or (fields['class']=='ADR')!=(fields['unit']=='depositary receipts'):
        raise ValueError('original share class/unit/date proof differs')
    return {'security_id':int(fields['sid']),'ticker':fields['ticker'],'line_type':fields['class'],
            'unit':fields['unit'],'valid_from':start,'valid_to':end,'available_at':clock,
            'source_sha256':A.pin(body)['sha256'],'source_document':str(body)}


def original_economic(body,receipt,url,pin):
    """Unchanged native economic parser plus actual captured-body floor.

    The independently reviewed whole source contract must authenticate publisher
    authority and inventory. A matching hash or this parser alone grants none.
    """
    from . import mna_price_native_v2 as MP,terminal_mna_v2 as T
    if not isinstance(pin,MP.EconomicPin):raise ValueError('typed independent native economic pin required')
    observed=original_observation(receipt,body,url)
    if T.utc(pin.fetched_at)!=observed or T.utc(pin.available_at)!=observed:
        raise ValueError('native economic clock does not replay original capture receipt')
    return MP.read_economic(A._blob(body),pin)


def original_sec_ixbrl_primitives(body_path,cik,*,original_bytes=None):
    """Stream actual SEC inline facts/contexts/units; return no admission flags.

    Accepted accession/owner/body authentication belongs to the fixed full stage
    reader. Every fact and conflicting class/context remains visible. Neither a
    cover-page listing nor one share fact proves issuer-wide class completeness.
    """
    import codecs
    from decimal import Decimal,InvalidOperation,localcontext
    from html.parser import HTMLParser
    cik=_owner(cik);path=A.absolute(body_path);storage_identity=A.pin(path)
    if original_bytes is not None and not isinstance(original_bytes,bytes):
        raise ValueError('immutable original decoded SEC bytes required')
    identity=storage_identity if original_bytes is None else {
        'bytes':len(original_bytes),'sha256':hashlib.sha256(original_bytes).hexdigest()}
    if identity['bytes']>40*1024*1024:raise ValueError('original SEC body exceeds unchanged native40MiB ceiling')
    if original_bytes is not None:
        # The native ledger reader authenticates its terminal and raw hash. Also
        # bind decoded bytes to this exact retained storage object here: an
        # unrelated pinned file plus caller-supplied text is not an original.
        import gzip
        decoded_hash=hashlib.sha256();decoded_size=0
        with (gzip.open(path,'rb') if path.suffix=='.gz' else path.open('rb')) as stream:
            while block:=stream.read(65536):
                decoded_size+=len(block)
                if decoded_size>40*1024*1024:raise ValueError('original decoded SEC body exceeds native ceiling')
                decoded_hash.update(block)
        if {'bytes':decoded_size,'sha256':decoded_hash.hexdigest()}!=identity:
            raise ValueError('original decoded SEC bytes do not match retained storage')
    wanted={'EntityCommonStockSharesOutstanding','TradingSymbol','Security12bTitle','EntityRegistrantName','EntityCentralIndexKey'}
    class Facts(HTMLParser):
        def __init__(self,*,capture_facts=True,needed_contexts=(),needed_units=()):
            super().__init__(convert_charrefs=True)
            self.namespaces={};self.contexts={};self.units={};self.facts=[]
            self.context=None;self.unit=None;self.capture=None;self.exclude_depth=0
            self.capture_facts=capture_facts
            self.needed_contexts=set(needed_contexts);self.needed_units=set(needed_units)
            self.context_depth=0;self.unit_depth=0
        def handle_starttag(self,tag,attrs):
            a=dict(attrs);local=tag.rsplit(':',1)[-1]
            if len(a)!=len(attrs):raise ValueError('duplicate original inline primitive attribute')
            for name,value in attrs:
                if name.startswith('xmlns:'):
                    prefix=name.split(':',1)[1]
                    if prefix in self.namespaces and self.namespaces[prefix]!=value:
                        raise ValueError('conflicting original inline namespace binding')
                    self.namespaces[prefix]=value
                elif name=='xmlns':
                    if '' in self.namespaces and self.namespaces['']!=value:
                        raise ValueError('conflicting original default namespace binding')
                    self.namespaces['']=value
            prefix=tag.split(':',1)[0] if ':' in tag else ''
            namespace=self.namespaces.get(prefix,'')
            instance='http://www.xbrl.org/2003/instance'
            if local in ('context','unit','divide','identifier','instant','measure') and namespace!=instance:
                raise ValueError('original SEC instance primitive namespace unsupported')
            if local in ('explicitmember','typedmember') and namespace!='http://xbrl.org/2006/xbrldi':
                raise ValueError('original SEC dimension primitive namespace unsupported')
            if local in ('nonfraction','nonnumeric','exclude') and namespace not in (
                    'http://www.xbrl.org/2013/inlineXBRL','http://www.xbrl.org/2008/inlineXBRL'):
                raise ValueError('original SEC inline primitive namespace unsupported')
            if local=='exclude' and self.capture is not None:
                self.exclude_depth+=1
            if local=='context':
                self.context_depth+=1
                if self.context_depth!=1 or not a.get('id'):raise ValueError('malformed original SEC context')
                if a['id'] in self.needed_contexts:
                    self.context={'id':a['id'],'identifiers':[],'instants':[],'dimensions':[]}
            elif local=='unit':
                self.unit_depth+=1
                if self.unit_depth!=1 or not a.get('id'):raise ValueError('malformed original SEC unit')
                if a['id'] in self.needed_units:
                    self.unit={'id':a['id'],'measures':[],'divided':False}
            elif local=='divide' and self.unit is not None:self.unit['divided']=True
            if ((self.context is not None and local in ('identifier','instant','explicitmember','typedmember')) or
                (self.unit is not None and local=='measure') or
                (self.capture_facts and local in ('nonfraction','nonnumeric') and a.get('name','').rsplit(':',1)[-1] in wanted)):
                if self.capture is not None:raise ValueError('nested original inline fact/value unsupported')
                self.capture={'tag':tag,'local':local,'attrs':a,'text':[]}
        def handle_data(self,value):
            if self.capture is not None and not self.exclude_depth:
                self.capture['text'].append(value)
                if sum(map(len,self.capture['text']))>65536:raise ValueError('original inline primitive exceeds bounded value size')
        def handle_endtag(self,tag):
            local=tag.rsplit(':',1)[-1]
            if local=='exclude' and self.exclude_depth:
                self.exclude_depth-=1
                return
            if self.capture is not None and self.capture['tag']==tag:
                value=self.capture;self.capture=None
                text=re.sub(r'\s+',' ',''.join(value['text'])).strip()
                if local=='identifier':self.context['identifiers'].append((value['attrs'].get('scheme'),text))
                elif local=='instant':self.context['instants'].append(text)
                elif local in ('explicitmember','typedmember'):
                    self.context['dimensions'].append((value['attrs'].get('dimension'),text,local))
                elif local=='measure':self.unit['measures'].append(text)
                else:
                    self.facts.append({'attributes':value['attrs'],'raw_value':text,'tag':tag})
                    if len(self.facts)>4096:raise ValueError('original DEI primitive count exceeds bounded per-document reader')
            if local=='context':self.context_depth-=1
            elif local=='unit':self.unit_depth-=1
            if self.context_depth<0 or self.unit_depth<0:raise ValueError('malformed original SEC context/unit closing tag')
            if local=='context' and self.context is not None:
                key=self.context['id']
                if key in self.contexts:raise ValueError('duplicate original SEC context identifier')
                self.contexts[key]=self.context;self.context=None
            elif local=='unit' and self.unit is not None:
                key=self.unit['id']
                if key in self.units:raise ValueError('duplicate original SEC unit identifier')
                self.units[key]=self.unit;self.unit=None
    def parse(encoding,errors,**selection):
        parser=Facts(**selection);decoder=codecs.getincrementaldecoder(encoding)(errors=errors)
        import io
        with (path.open('rb') if original_bytes is None else io.BytesIO(original_bytes)) as stream:
            while block:=stream.read(65536):parser.feed(decoder.decode(block))
        parser.feed(decoder.decode(b'',final=True));parser.close()
        if (parser.capture is not None or parser.context is not None or parser.unit is not None or
                parser.exclude_depth or parser.context_depth or parser.unit_depth):
            raise ValueError('incomplete original SEC inline primitive')
        return parser
    try:parsed=parse('utf-8','strict');encoding='utf-8'
    except UnicodeDecodeError:parsed=parse('cp1252','replace');encoding='cp1252'
    # An original body may have hundreds of thousands of financial contexts.
    # Replay it again and retain only contexts/units referenced by every target
    # DEI fact; never select a caller subset or keep the unrelated full corpus.
    referenced=parse(encoding,'strict' if encoding=='utf-8' else 'replace',capture_facts=False,
        needed_contexts={f['attributes'].get('contextref') for f in parsed.facts},
        needed_units={f['attributes'].get('unitref') for f in parsed.facts})
    if referenced.namespaces!=parsed.namespaces:raise ValueError('original SEC namespaces changed between passes')
    parsed.contexts=referenced.contexts;parsed.units=referenced.units
    output=[]
    for index,fact in enumerate(parsed.facts):
        a=fact['attributes'];name=a.get('name','');pieces=name.split(':',1)
        if len(pieces)!=2 or pieces[1] not in wanted:continue
        namespace=parsed.namespaces.get(pieces[0].casefold(),'')
        if not re.fullmatch(r'https?://xbrl\.sec\.gov/dei/\d{4}(?:-\d{2}-\d{2})?',namespace):
            raise ValueError('original DEI primitive namespace unsupported')
        context=parsed.contexts.get(a.get('contextref'));reason=[];asof=None;amount=None
        if a.get('continuedat'):reason.append('original_inline_continuation_unproved')
        if a.get('xsi:nil') in ('true','1'):reason.append('original_inline_nil')
        if context is None:reason.append('missing_original_context')
        else:
            identifiers=context['identifiers']
            if (len(identifiers)!=1 or identifiers[0][0]!='http://www.sec.gov/CIK' or
                not re.fullmatch(r'[0-9]{1,10}',identifiers[0][1]) or int(identifiers[0][1])!=cik):
                reason.append('original_context_issuer_mismatch')
            if len(context['instants'])==1:
                try:asof=_date(context['instants'][0])
                except ValueError:reason.append('unsupported_original_instant')
        if pieces[1]=='EntityCommonStockSharesOutstanding':
            unit=parsed.units.get(a.get('unitref'))
            measures=[] if unit is None else unit['measures']
            valid_measure=False
            if len(measures)==1 and ':' in measures[0]:
                prefix,measure=measures[0].split(':',1)
                valid_measure=(measure=='shares' and parsed.namespaces.get(prefix.casefold())=='http://www.xbrl.org/2003/instance')
            if unit is None or unit['divided'] or not valid_measure:reason.append('original_share_unit_unproved')
            if asof is None:reason.append('original_share_instant_unproved')
            try:
                scale=a.get('scale','0')
                if not re.fullmatch(r'-?[0-9]{1,2}',scale) or not -12<=int(scale)<=12:
                    raise ValueError('unsupported original inline scale')
                format_=a.get('format','')
                if format_:
                    prefix,separator,transform=format_.partition(':')
                    uri=parsed.namespaces.get(prefix.casefold(),'')
                    if not separator or transform not in ('num-dot-decimal','numcommadot') or not re.fullmatch(
                        r'https?://www\.xbrl\.org/inlineXBRL/transformation/\d{4}-\d{2}-\d{2}',uri):
                        raise ValueError('unsupported original inline transformation namespace')
                if a.get('sign'):
                    raise ValueError('unsupported original inline numeric transformation')
                text=fact['raw_value']
                if not re.fullmatch(r'(?:[0-9]+|[0-9]{1,3}(?:,[0-9]{3})+)(?:\.[0-9]+)?',text):
                    raise ValueError('unsupported original share numeric lexical value')
                lexical=text.replace(',','')
                with localcontext() as arithmetic:
                    arithmetic.prec=len(lexical)+abs(int(scale))+2
                    amount=Decimal(lexical)*(Decimal(10)**int(scale))
                if not amount.is_finite() or amount<=0 or amount!=amount.to_integral_value():
                    raise ValueError('invalid original outstanding share count')
            except (ValueError,InvalidOperation):reason.append('original_share_quantity_unproved');amount=None
        output.append({'cik':cik,'fact_index':index,'name':name,'context_id':a.get('contextref'),
            'unit_id':a.get('unitref'),'raw_value':fact['raw_value'],'scale':a.get('scale','0'),
            'asof_date':asof,'shares':amount,'dimensions':context['dimensions'] if context else [],
            'primitive_reason':';'.join(reason) or 'parsed_original_dei_primitive',
            'source_sha256':identity['sha256'],'source_document':str(path),'source_encoding':encoding,
            'source_storage_identity':storage_identity})
    if A.pin(path)!=storage_identity:raise ValueError('original SEC body changed during primitive parsing')
    return output


def price_cell_qualification(row,*,vendor_clock,fitting_clocks,economic,identity,bracket_clock=None):
    """Derive a native return cell's complete information clock and exclusions.

    Fitting clocks cover the full session/mass cohort and every source document
    used by the authenticated replay, not only this line's two observations.
    Inventory authentication belongs to the fixed stage reader; no caller flags
    or native diagnostic non-nullness enter this arithmetic.
    """
    clocks=[];reasons=[]
    mark=dt.datetime.combine(row['session_date'],dt.time(22),tzinfo=dt.timezone.utc)
    if vendor_clock is None:reasons.append('missing_original_vendor_observation_clock')
    else:clocks.append(_clock(vendor_clock) if isinstance(vendor_clock,str) else vendor_clock)
    if not fitting_clocks or any(v is None for v in fitting_clocks):
        reasons.append('missing_complete_fitting_corpus_clocks')
    else:clocks.extend(_clock(v) if isinstance(v,str) else v for v in fitting_clocks)
    if identity is None:reasons.append('missing_original_dated_identity')
    elif (identity['security_id']!=row['security_id'] or identity['ticker']!=row['ticker'] or
          not identity['valid_from']<=row['session_date']<=identity['valid_to']):
        reasons.append('original_identity_date_or_security_mismatch')
    else:clocks.append(identity['available_at'])
    if economic is None:reasons.append('missing_original_price_economics')
    elif economic.ticker!=row['ticker'] or not economic.valid_from<=row['session_date']<=economic.valid_to:
        reasons.append('original_economic_date_or_ticker_mismatch')
    else:clocks.append(economic.available_at)
    if row.get('mapped_sid0'):
        if bracket_clock is None:reasons.append('sid0_mapping_has_no_dated_original_evidence')
        else:clocks.append(bracket_clock)
    if any(v.tzinfo is None for v in clocks):raise ValueError('original qualification clock requires explicit offset')
    clocks=[v.astimezone(dt.timezone.utc) for v in clocks]
    available=max(clocks+[mark]) if clocks else None
    if available is not None and available>mark:reasons.append('complete_original_inputs_not_known_at_session_mark')
    ret=row.get('ret')
    if ret is None or not math.isfinite(ret) or ret<=-1:reasons.append('native_return_missing_or_invalid')
    # Absolute backward-anchored levels are never qualified by this ratio route.
    return {'return_certified':not reasons,'qualified':not reasons,
            'source_available_at':available,'available_at':available,
            'native_source_clock':max(clocks) if clocks else None,
            'absolute_adj_close_qualified':False,'reason':';'.join(reasons) or 'complete_original_asof_return_ratio',
            'fitting_source_clocks':clocks}


def vendor_scope_code_pins():
    """Exact point/source code inventory for the separately reviewed native caller."""
    from . import mna_price_native_v2 as MP,terminal_mna_v2 as T
    import inspect,types
    modules=(C,A,MP,T,__import__(__name__,fromlist=['']))
    def code(value):
        if isinstance(value,types.CodeType):
            return {k:code(getattr(value,k)) for k in ('co_code','co_consts','co_names','co_varnames','co_freevars',
                'co_cellvars','co_argcount','co_posonlyargcount','co_kwonlyargcount','co_nlocals','co_stacksize','co_flags','co_exceptiontable')}
        if isinstance(value,bytes):return ['bytes',value.hex()]
        if isinstance(value,tuple):return ['tuple',[code(v) for v in value]]
        return [type(value).__name__,repr(value)]
    loaded={m.__name__+'.'+k:code(v.__code__) for m in modules for k,v in vars(m).items()
            if inspect.isfunction(v) and v.__module__==m.__name__}
    loaded.update({m.__name__+'.'+k+'.'+method:code(value.__code__)
        for m in modules for k,kind in vars(m).items() if inspect.isclass(kind) and kind.__module__==m.__name__
        for method,value in vars(kind).items() if inspect.isfunction(value)})
    import duckdb,pyarrow,_duckdb,sys
    binary={str(p.resolve()):A.pin(p) for package in (Path(duckdb.__file__).parent,Path(pyarrow.__file__).parent)
            for p in package.iterdir() if p.suffix.lower() in ('.pyd','.dll')}
    return {str(Path(m.__file__).resolve()):A.pin(m.__file__) for m in (*modules,_duckdb)}|binary|{
        'runtime:market-vendor-point-code':{'bytes':None,'sha256':A.digest(loaded)},
        'runtime:market-vendor-point-packages':{'bytes':None,'sha256':A.digest({
            'python':sys.version,'duckdb':duckdb.__version__,'pyarrow':pyarrow.__version__})}}


def _vendor_check(expected):
    current=vendor_scope_code_pins()
    for path,pin in expected.items():
        actual=current.get(path) if path.startswith('runtime:') else A.pin(path)
        if actual!=pin:raise ValueError('independently pinned original vendor/dependency/code changed: '+path)


def code_pins():
    """Full fixed market phase code, constants, runtime and binary inventory."""
    import inspect,marshal
    from ..research import spine
    from .. import universe_us_listed
    from .. import identity_links as r4_links,identity_reconstruction as r4_reconstruction
    from .. import market_owner_bridge,historical_identity,historical_identity_sources
    from .. import _fundamental_clock,_submissions_archive,calendar,ticker_history,market_daily,warehouse
    modules=(C,P,M,SH,CA,L,B,LR,SM,IT,PN,IN,IB,IL,spine,universe_us_listed,r4_links,r4_reconstruction,market_owner_bridge,
             historical_identity,historical_identity_sources,_fundamental_clock,_submissions_archive,
             calendar,ticker_history,market_daily,warehouse)
    result=vendor_scope_code_pins()
    loaded={}
    constants={}
    for module in modules:
        result[str(Path(module.__file__).resolve())]=A.pin(module.__file__)
        for name,value in vars(module).items():
            if inspect.isfunction(value) and value.__module__==module.__name__:
                loaded[module.__name__+'.'+name]=hashlib.sha256(marshal.dumps(value.__code__)).hexdigest()
            elif inspect.isclass(value) and value.__module__==module.__name__:
                for method,function in vars(value).items():
                    if inspect.isfunction(function):loaded[module.__name__+'.'+name+'.'+method]=hashlib.sha256(marshal.dumps(function.__code__)).hexdigest()
            elif name.isupper() and isinstance(value,(str,int,float,bool,tuple,list,dict,frozenset,dt.date,dt.timedelta,Path,re.Pattern)):
                constants[module.__name__+'.'+name]=sorted(value,key=repr) if isinstance(value,frozenset) else value
    result['runtime:market-r4-reconstruction-params']={'bytes':None,'sha256':r4_reconstruction.ReconstructionParams().digest()}
    result['runtime:market-fixed-native-phases']={'bytes':None,'sha256':A.digest(loaded)}
    result['runtime:market-fixed-native-constants']={'bytes':None,'sha256':A.digest(constants)}
    return result


def _check(pins):
    current=code_pins()
    if any(pins.get(key)!=value for key,value in current.items()):
        raise ValueError('complete independently pinned market native code/runtime required')
    for path,expected in pins.items():
        actual=current.get(path) if path.startswith('runtime:') else A.pin(path)
        if actual!=expected:raise ValueError('complete original market dependency changed: '+path)


class _VendorPointReader:
    def __init__(self,con,vendors,relations):
        self.con,self.vendors,self.relations=con,vendors,relations
        self.active=True

    def point(self,vendor_path,vendor_pin,economic_bytes,economic_pin,
              provenance='independently-reviewed-golden-window'):
        from . import mna_price_native_v2 as MP,terminal_mna_v2 as T
        from decimal import Decimal
        if not self.active:raise ValueError('original vendor point outside authenticated consumption scope')
        path=str(A.absolute(vendor_path))
        if path not in self.vendors:raise ValueError('original vendor outside independently admitted full inventory')
        entry=self.vendors[path]
        if (not isinstance(vendor_pin,MP.VendorPricePin) or not isinstance(economic_pin,MP.EconomicPin) or
            not provenance or vendor_pin.sha256!=entry['identity']['sha256'] or
            vendor_pin.expected_schema!=entry['expected_schema']):
            raise ValueError('complete independently pinned native vendor point differs')
        proof=MP.read_economic(economic_bytes,economic_pin)
        if (vendor_pin.economic_source_id!=proof.source_id or vendor_pin.ticker!=proof.ticker or
            not proof.valid_from<=vendor_pin.session_date<=proof.valid_to):
            raise ValueError('dated native security/ticker/economic binding mismatch')
        relation,dates_supported=self.relations[path]
        if not dates_supported:
            raise ValueError('native vendor security/date/ticker price missing or mismatched')
        # Project the complete session, then apply the unchanged native Python
        # equality/set semantics. SQL casts/grouping would change string SID,
        # NULL and NaN ambiguity semantics; a premature LIMIT could hide matches.
        cursor=self.con.execute(f'''SELECT securityID,tradingDate,ticker_tk,close
            FROM {relation} WHERE tradingDate=?''',[vendor_pin.session_date])
        rows=[];ticker_security_ids=set()
        while True:
            batch=cursor.fetchmany(32768)
            if not batch:break
            for sid,day,ticker,raw_close in batch:
                if day==vendor_pin.session_date and ticker==vendor_pin.ticker:
                    ticker_security_ids.add(sid)
                    if len(ticker_security_ids)>1:
                        raise ValueError('ambiguous native vendor ticker/date security identity')
                if sid==vendor_pin.security_id and day==vendor_pin.session_date:
                    rows.append((raw_close,ticker))
                    if len(rows)>1:raise ValueError('ambiguous native vendor SID/date price')
        if len(rows)!=1 or rows[0][1]!=vendor_pin.ticker:
            raise ValueError('native vendor security/date/ticker price missing or mismatched')
        close=Decimal(str(rows[0][0]))
        if not close.is_finite() or close<=0:raise ValueError('native raw traded close invalid')
        floor=dt.datetime.combine(vendor_pin.session_date,dt.time(22),tzinfo=T.UTC)
        evidence=T.Evidence(vendor_pin.source_id,vendor_pin.sha256,
            max(T.utc(vendor_pin.available_at),T.utc(proof.available_at),floor),provenance)
        return T.Price(vendor_pin.security_id,vendor_pin.session_date,close,proof.currency,proof.share_class,
                       proof.unit,proof.basis,evidence)


@contextlib.contextmanager
def vendor_price_scope(source_root,vendor_pins,dependency_pins,con):
    """Transaction-only native point projection of independently admitted originals.

    The caller's fixed NativeWindows source-contract reader is the original
    inventory admission authority. This utility neither certifies arbitrary
    tables nor replaces its independently reviewed economic/source contract.
    Every original and complete expected dependency is authenticated before and
    after the scope, including errors; no relation survives scope termination.
    """
    import copy
    import pyarrow as pa
    import pyarrow.parquet as pq
    root=A.absolute(source_root)
    A._guard(con,root)
    vendors=copy.deepcopy(vendor_pins);expected=copy.deepcopy(dependency_pins)
    required=vendor_scope_code_pins()
    if not vendors or any(expected.get(key)!=pin for key,pin in required.items()):
        raise ValueError('independently pinned complete original vendor point code required')
    for path,entry in vendors.items():
        if (str(A.absolute(path))!=path or set(entry)!= {'identity','expected_schema'} or
            expected.get(path)!=entry['identity']):
            raise ValueError('fixed independently admitted original vendor inventory differs')
    _vendor_check(expected)
    relations={};reader=None
    try:
        for i,(path,entry) in enumerate(sorted(vendors.items())):
            schema=pq.read_schema(path)
            if str(schema)!=entry['expected_schema'] or any(k not in schema.names for k in
                    ('securityID','tradingDate','ticker_tk','close')):
                raise ValueError('complete original vendor schema mismatch')
            name=f't17_market_vendor_point_{i}'
            relation=_table(con,name,f'SELECT securityID,tradingDate,ticker_tk,close FROM {_sql(path)}')
            relations[path]=(relation,pa.types.is_date(schema.field('tradingDate').type))
        reader=_VendorPointReader(con,vendors,relations)
        yield reader
    finally:
        if reader is not None:reader.active=False
        try:
            _vendor_check(expected)
        finally:
            for relation,_ in relations.values():con.execute(f'DROP TABLE {relation}')


def publication_clock(body_path,receipt_path,source_url,archive=None):
    """Exact original UTC/archival body proof; date labels are diagnostic only."""
    from email.utils import parsedate_to_datetime
    receipt=_json(receipt_path)
    observed_url=receipt.get('url',source_url)
    if observed_url!=source_url and (archive is None or not re.fullmatch(
        r'https://web\.archive\.org/web/\d{14}id_/'+re.escape(source_url),observed_url)):
        raise ValueError('original publisher/archive capture URL differs')
    original_observation(receipt_path,body_path,observed_url)
    soup=_html(body_path)
    values=[tag.get('content') for tag in soup.find_all('meta')
            if tag.get('property') in ('article:published_time','og:article:published_time')]
    values.extend(tag.get('datetime') for tag in soup.find_all('time') if tag.get('datetime'))
    if values:
        parsed=[_clock(v) for v in values]
        if len(set(parsed))!=1:raise ValueError('ambiguous original publisher publication clock')
        # A publication label alone cannot bind an edited current body to the
        # claimed old vintage. A complete original capture is still required.
        if archive is None:return None,'missing_original_body_vintage_proof'
    if archive is None:return None,'date_only_or_missing_original_publication_clock'
    if set(archive)!= {'body','receipt'}:raise ValueError('complete original archive capture inventory required')
    captured=A.absolute(archive['body']);record=_json(archive['receipt'])
    url=record.get('url','')
    match=re.fullmatch(r'https://web\.archive\.org/web/(\d{14})id_/'+re.escape(source_url),url)
    if not match:raise ValueError('original archived publisher URL/identity differs')
    original_observation(archive['receipt'],captured,url)
    header=record.get('headers',{}).get('Memento-Datetime')
    if not header:raise ValueError('actual authoritative archive observation timestamp missing')
    clock=parsedate_to_datetime(header)
    if clock.tzinfo is None:raise ValueError('original archive observation requires explicit UTC')
    clock=clock.astimezone(dt.timezone.utc)
    stamp=dt.datetime.strptime(match[1],'%Y%m%d%H%M%S').replace(tzinfo=dt.timezone.utc)
    if clock!=stamp or A._blob(captured)!=A._blob(body_path):
        raise ValueError('original archived timestamp/body bytes conflict')
    if values and parsed[0]>clock:raise ValueError('archive capture precedes claimed original publication')
    return clock,'original_matching_body_archival_observation_floor'


def original_nyse_calendar(body_path):
    """Parse the actual ICE HTML title/holiday table, including blank spacers."""
    soup=_html(body_path)
    titles={t.get('content','') for t in soup.find_all('meta') if t.get('property')=='og:title'}
    if len(titles)!=1:raise ValueError('unique original publisher title required')
    title=next(iter(titles))
    match=re.fullmatch(r'NYSE Group Announces (\d{4}), (\d{4}) and (\d{4}) Holiday and Early Closings Calendar',title)
    if not match:raise ValueError('original NYSE cash-equity calendar covered-year title unsupported')
    years=list(map(int,match.groups()))
    if years!=list(range(years[0],years[0]+3)):
        raise ValueError('original covered-year authority is not contiguous')
    candidates=[]
    for table in soup.find_all('table'):
        rows=[[re.sub(r'\s+',' ',cell.get_text(' ',strip=True)).strip()
               for cell in row.find_all(['td','th'],recursive=False)] for row in table.find_all('tr')]
        headers=[i for i,row in enumerate(rows) if 'HOLIDAY' in row and all(str(y) in row for y in years)]
        if headers:candidates.append((rows,headers))
    if len(candidates)!=1 or len(candidates[0][1])!=1:
        raise ValueError('complete unique original holiday table required')
    rows,headers=candidates[0];header=rows[headers[0]]
    positions=[header.index('HOLIDAY')]+[header.index(str(y)) for y in years]
    baseline={"New Year's Day",'Martin Luther King, Jr. Day',"Washington's Birthday",'Good Friday',
              'Memorial Day','Independence Day','Labor Day','Thanksgiving Day','Christmas Day'}
    holidays={y:{} for y in years};seen=set()
    for row in rows[headers[0]+1:]:
        if not any(row):continue
        if len(row)!=len(header):raise ValueError('malformed original holiday row/spacer axis')
        fields=[row[i] for i in positions]
        label=fields[0].replace('\u2019',"'")
        if label not in baseline|{'Juneteenth National Independence Day'} or label in seen:
            raise ValueError('unknown or duplicate original holiday category')
        seen.add(label)
        for year,text in zip(years,fields[1:],strict=True):
            text=re.sub(r'\*+$','',text).strip()
            if text in ('--','\u2014','\u2013','-'):continue
            match=re.fullmatch(r'(Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday), ([A-Za-z]+) (\d{1,2})(?:\([^)]*\)| \([^)]*\))?(?:\*+)?',text)
            if not match:raise ValueError('unsupported complete original holiday date/footnote context')
            date=dt.datetime.strptime(f'{match[2]} {match[3]} {year}','%B %d %Y').date()
            if date.strftime('%A')!=match[1] or date in holidays[year]:
                raise ValueError('original holiday date/weekday or duplicate conflict')
            holidays[year][date]=label
    if not baseline<=seen:raise ValueError('incomplete original annual holiday category population')
    return {'title':title,'years':years,'holidays':holidays,'source_sha256':A.pin(body_path)['sha256']}


def original_nyse_closure(body_path):
    soup=_html(body_path);matches=[]
    for p in soup.find_all('p'):
        text=re.sub(r'\s+',' ',p.get_text(' ',strip=True))
        for match in re.finditer(r'will be closed on (Monday|Tuesday|Wednesday|Thursday|Friday), ([A-Za-z]+) (\d{1,2}), (\d{4})',text):
            if not re.search(r'New York Stock Exchange|NYSE',text):continue
            day=dt.datetime.strptime(f'{match[2]} {match[3]} {match[4]}','%B %d %Y').date()
            if day.strftime('%A')!=match[1]:raise ValueError('original extraordinary closure weekday mismatch')
            matches.append((day,hashlib.sha256(text.encode()).hexdigest()))
    if len(matches)!=1:raise ValueError('complete unique original NYSE closure declaration required')
    return {'closure_date':matches[0][0],'paragraph_sha256':matches[0][1],
            'source_sha256':A.pin(body_path)['sha256']}


def original_distributed_nyse_clock(body_path,receipt_path,source_url):
    """Parse this Nasdaq distributed copy's clock, never the ICE clock.

    The explicit publisher label has minute precision. The returned interval
    retains that precision; conservative availability is its upper boundary.
    Complete source/vintage/rights and schedule admission remain separate.
    """
    from html.parser import HTMLParser
    from zoneinfo import ZoneInfo
    class PublishedPage(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.paragraphs=[];self.paragraph=None;self.links=[]
        def handle_starttag(self,tag,attrs):
            attributes=dict(attrs)
            if tag=='p':
                if self.paragraph is not None:raise ValueError('nested original publisher paragraph')
                self.paragraph=[]
            elif tag=='a' and attributes.get('href'):
                self.links.append(attributes['href'])
        def handle_data(self,value):
            if self.paragraph is not None:self.paragraph.append(value)
        def handle_endtag(self,tag):
            if tag=='p' and self.paragraph is not None:
                self.paragraphs.append(re.sub(r'\s+',' ',''.join(self.paragraph)).strip())
                self.paragraph=None
    if not re.fullmatch(r'https://www\.nasdaq\.com/press-release/nyse-group-announces-'
                        r'\d{4}-\d{4}-and-\d{4}-holiday-and-early-closings-calendar-\d{4}-\d{2}',source_url):
        raise ValueError('unsupported original distributed publisher URL')
    observed=original_observation(receipt_path,body_path,source_url)
    page=PublishedPage();page.feed(A._blob(body_path).decode('utf-8',errors='strict'));page.close()
    if page.paragraph is not None:raise ValueError('incomplete original publisher paragraph')
    paragraphs=page.paragraphs
    labels=[paragraphs[i+1] for i,p in enumerate(paragraphs[:-1]) if p=='Published']
    if len(labels)!=1:raise ValueError('unique original distributed Published label required')
    match=re.fullmatch(r'([A-Z][a-z]{2} \d{1,2}, \d{4} \d{1,2}:\d{2}[ap]m) (EST|EDT)',labels[0])
    if not match:raise ValueError('explicit original publisher clock/timezone required')
    naive=dt.datetime.strptime(match[1],'%b %d, %Y %I:%M%p')
    local=naive.replace(tzinfo=ZoneInfo('America/New_York'))
    lower=local.astimezone(dt.timezone.utc)
    if local.tzname()!=match[2] or lower.astimezone(local.tzinfo).replace(tzinfo=None)!=naive:
        raise ValueError('original publisher timezone/date conflict')
    upper=lower+dt.timedelta(minutes=1)
    if observed<lower:raise ValueError('original capture predates publisher label')
    links=set()
    for href in page.links:
        if re.fullmatch(r'https?://(?:www\.)?businesswire\.com/news/home/[0-9]+/en/?',href):
            links.add(href.replace('http:','https:').replace('https://businesswire.','https://www.businesswire.').rstrip('/'))
    if len(links)!=1:raise ValueError('unique original distributed source-version attribution required')
    early=[p for p in paragraphs if 'market will close early at' in p]
    if not early:raise ValueError('complete original early-close paragraphs missing')
    return {'publisher_url':source_url,'published_label':labels[0],
            'publication_lower_inclusive':lower,'publication_upper_exclusive':upper,
            'publication_precision':'minute','conservative_available_at':upper,
            'capture_available_at':observed,'source_version_url':next(iter(links)),
            'early_close_paragraphs':early,'source_sha256':A.pin(body_path)['sha256'],
            'ice_publication_clock':None,'source_qualified':False}


def panel_core_query(prices,calendar,b):
    """Exact native vendor-derived core; snapshot share fields are diagnostics."""
    if type(b) is not int or not 0<=b<P.BUCKETS:raise ValueError('native core bucket outside fixed axis')
    return f"""
    WITH cal AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM {calendar}),
    p AS (
        SELECT p.*, cal.sidx,
               CASE WHEN fb_action = 'repaired' OR NOT (return_factor > 0 AND isfinite(return_factor)) THEN 0.0
                    ELSE ln(return_factor) END AS lrf
        FROM {prices} p
        JOIN cal USING (session_date)
        WHERE p.security_id % {P.BUCKETS} = {b}
    ),
    w AS (
        SELECT *,
            sum(lrf) OVER (PARTITION BY security_id ORDER BY session_date ROWS UNBOUNDED PRECEDING) AS clf,
            avg(dollar_volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {PN.LOOKBACK} PRECEDING AND 1 PRECEDING) AS adv63,
            count(dollar_volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {PN.LOOKBACK} PRECEDING AND 1 PRECEDING) AS adv63_n,
            lag(sidx, {PN.LOOKBACK}) OVER (PARTITION BY security_id ORDER BY session_date) AS sidx_lag63,
            lag(close) OVER (PARTITION BY security_id ORDER BY session_date) AS raw_close_prev,
            lag(sidx) OVER (PARTITION BY security_id ORDER BY session_date) AS sidx_prev,
            bool_or(shares_above_ceiling) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS UNBOUNDED PRECEDING) AS shares_withheld,
            max(CASE WHEN earn_flag = '0' THEN session_date END) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS UNBOUNDED PRECEDING) AS last_earn_date
        FROM p
    ),
    sh_obs AS (
        SELECT security_id, session_date AS obs_date, shares_vendor AS obs_shares, clf AS obs_clf
        FROM w WHERE shares_vendor IS NOT NULL
    ),
    sh AS (
        SELECT w.security_id, w.session_date, o.obs_date, o.obs_shares, o.obs_clf
        FROM w ASOF LEFT JOIN sh_obs o
          ON w.security_id = o.security_id AND (w.session_date - INTERVAL {PN.SHARES_LAG_DAYS} DAY) >= o.obs_date
    ),
    j AS (
        SELECT w.*, sh.obs_date AS shares_obs_date,
            CASE WHEN sh.obs_date IS NULL OR sh.obs_date < w.session_date - INTERVAL {PN.SHARES_LAG_DAYS + PN.SHARES_MAX_AGE_DAYS} DAY
                      OR w.shares_withheld THEN NULL
                 ELSE sh.obs_shares * exp(-(w.clf - sh.obs_clf)) END AS shares_out_a8
        FROM w JOIN sh USING (security_id, session_date)
    ),
    t AS (
        SELECT *,
            -- trailing median daily volume in the session's share basis: volume / exp(clf) is basis-free
            median(volume * exp(-clf)) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {PN.TURNOVER_WINDOW - 1} PRECEDING AND CURRENT ROW) * exp(clf) AS med_vol21,
            count(volume) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN {PN.TURNOVER_WINDOW - 1} PRECEDING AND CURRENT ROW) AS med_vol21_n
        FROM j
    )
    SELECT session_date, security_id, sidx, ticker, open, high, low, close AS raw_close, adj_close AS close,
           ret, ret_guarded, volume, dollar_volume, raw_close_prev, (sidx_prev = sidx - 1) AS present_prev,
           CASE WHEN adv63_n = {PN.LOOKBACK} AND sidx_lag63 = sidx - {PN.LOOKBACK} AND sidx_prev = sidx - 1
                     AND raw_close_prev > {PN.PRICE_MIN} AND adv63 > {PN.ADV_MIN} THEN adv63 END AS adv63_eligible,
           adv63,
           CASE WHEN shares_out_a8 BETWEEN {PN.SHARES_DOMAIN[0]} AND {PN.SHARES_DOMAIN[1]}
                     AND NOT (med_vol21_n >= {PN.TURNOVER_MIN_OBS} AND med_vol21 > {PN.TURNOVER_MAX} * shares_out_a8)
                THEN shares_out_a8 END AS shares_out_pre_si,
           shares_obs_date, shares_withheld,
           CASE earn_flag WHEN '0' THEN 1.0 WHEN '1' THEN 1.0 WHEN 'N' THEN 0.0 WHEN '-1' THEN 0.0 END AS earn_recent,
           earn_flag, gics,
           coalesce(session_date - last_earn_date <= {PN.OPERATING_DAYS}, false) AS is_operating,
           security_id >= {PN.INDEX_ID_MIN} AS is_index,
           iv_atm_5d, iv_atm_10d, iv_atm_21d, iv_atm_42d, iv_atm_63d, iv_atm_126d, iv_atm_252d
    FROM t
    """


def panel_membership_query(core,year):
    """Exact native full-population membership, independent of issuer links."""
    if type(year) is not int or not 1900<=year<=2200:raise ValueError('native membership year outside axis')
    lo=f"DATE '{year}-01-01'";hi=f"DATE '{year}-12-31'"
    return f"""
    WITH c AS (
        SELECT session_date, security_id, sidx, adv63_eligible, ret, ret_guarded, present_prev
        FROM {core}
        WHERE session_date BETWEEN {lo} - INTERVAL 10 DAY AND {hi}
    ),
    r AS (
        SELECT *, CASE WHEN adv63_eligible IS NOT NULL THEN
                 row_number() OVER (PARTITION BY session_date ORDER BY adv63_eligible DESC NULLS LAST, security_id) END AS adv_rank
        FROM c
    ),
    m AS (
        SELECT *, coalesce(adv_rank <= {PN.TOP_N}, false) AS member FROM r
    ),
    mm AS (
        SELECT m.*, lag(member) OVER (PARTITION BY security_id ORDER BY sidx) AS member_prev_row,
               lag(sidx) OVER (PARTITION BY security_id ORDER BY sidx) AS sidx_prev_row
        FROM m
    ),
    mk AS (
        SELECT session_date,
               avg(ret) FILTER (WHERE member_prev_row AND sidx_prev_row = sidx - 1 AND NOT ret_guarded AND ret IS NOT NULL) AS mkt_ret,
               count(*) FILTER (WHERE member_prev_row AND sidx_prev_row = sidx - 1 AND NOT ret_guarded AND ret IS NOT NULL) AS mkt_n,
               count(*) FILTER (WHERE member) AS members
        FROM mm GROUP BY 1
    )
    SELECT mm.session_date, mm.security_id, mm.member, mm.adv_rank, mk.mkt_ret, mk.mkt_n
    FROM mm JOIN mk USING (session_date)
    WHERE mm.session_date BETWEEN {lo} AND {hi}
    """


def _native_name_days(con,*,raw_finra,si,submissions,prices,calendar,excluded_days,namespace):
    """Original name-phase replay; modeled clocks and precedence stay diagnostic.

    All seven relations must originate in complete native source replay. In
    particular excluded_days is the complete original strict/backfill axis;
    this function does not infer its absence or qualify historical name clocks.
    """
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed native names namespace required')
    funcs=(namespace+'_stem_finra',namespace+'_stem_sec')
    occupied={r[0] for r in con.execute('SELECT function_name FROM duckdb_functions()').fetchall()}
    if any(f in occupied for f in funcs):raise ValueError('native name namespace occupied')
    created=[]
    try:
        for name,func in zip(funcs,(IN.stem_finra,IN.stem_sec)):
            con.create_function(name,func,['VARCHAR'],'VARCHAR');created.append(name)
        fr=_table(con,namespace+'_fr',f"SELECT DISTINCT symbolCode AS symbol,CAST(settlementDate AS DATE) AS settlement_date,issueName AS issue_name FROM {raw_finra} WHERE symbolCode IS NOT NULL AND issueName IS NOT NULL AND settlementDate IS NOT NULL")
        rows=_table(con,namespace+'_rows',f"SELECT s.security_id,s.settlement_date,s.dissemination_date,fr.issue_name,{funcs[0]}(fr.issue_name) AS fstem,length(fr.issue_name)>=30 AS truncated FROM {si} s JOIN {fr} fr USING(symbol,settlement_date) WHERE s.security_id IS NOT NULL")
        forms=','.join("'"+f+"'" for f in IN.PERIODIC)
        names=_table(con,namespace+'_secnames',f"SELECT DISTINCT CAST(cik AS BIGINT) AS cik,{funcs[1]}(name) AS sstem,filed FROM {submissions} WHERE form IN ({forms})")
        stems=_table(con,namespace+'_fst',f"SELECT DISTINCT fstem,truncated FROM {rows} WHERE length(fstem)>=4")
        pairs=_table(con,namespace+'_pairs',f"""SELECT DISTINCT f.fstem,f.truncated,n.sstem FROM {stems} f JOIN(SELECT DISTINCT sstem FROM {names}) n ON n.sstem=f.fstem
            UNION SELECT DISTINCT f.fstem,f.truncated,n.sstem FROM(SELECT * FROM {stems} WHERE truncated AND length(fstem)>=12) f
            JOIN(SELECT DISTINCT sstem,substr(sstem,1,12) AS k FROM {names} WHERE length(sstem)>=12) n
            ON n.k=substr(f.fstem,1,12) AND starts_with(n.sstem,f.fstem)""")
        matches=_table(con,namespace+'_matches',f"""SELECT r.security_id,r.settlement_date,r.dissemination_date,r.issue_name,r.fstem,n.cik
            FROM {rows} r JOIN {pairs} p ON p.fstem=r.fstem AND p.truncated=r.truncated JOIN {names} n ON n.sstem=p.sstem
            WHERE n.filed<=r.dissemination_date AND n.filed>r.dissemination_date-INTERVAL 450 DAY""")
        unique=_table(con,namespace+'_uniq',f"""SELECT security_id,settlement_date,dissemination_date,any_value(cik) AS cik,any_value(issue_name) AS issue_name
            FROM(SELECT DISTINCT security_id,settlement_date,dissemination_date,cik,issue_name FROM {matches})
            GROUP BY 1,2,3 HAVING count(DISTINCT cik)=1""")
        days=_table(con,namespace+'_days',f"""WITH s AS(SELECT u.*,lead(dissemination_date) OVER(PARTITION BY security_id ORDER BY settlement_date) AS next_dissem FROM {unique} u)
            SELECT s.security_id,c.session_date,s.cik,CAST(s.dissemination_date AS TIMESTAMP)+INTERVAL 22 HOUR AS available_at,s.issue_name
            FROM s JOIN {calendar} c ON c.session_date>s.dissemination_date AND c.session_date<=s.dissemination_date+INTERVAL 45 DAY
            AND(s.next_dissem IS NULL OR c.session_date<=s.next_dissem)
            SEMI JOIN(SELECT security_id,session_date FROM {prices}) px ON px.security_id=s.security_id AND px.session_date=c.session_date""")
        con.execute(f'DELETE FROM {days} WHERE (security_id,session_date) IN (SELECT security_id,session_date FROM {excluded_days})')
        return {'identity/links_name_days.parquet':days}
    finally:
        for name in reversed(created):con.remove_function(name)


def _native_backfill_days(con,*,export,strict,prices,calendar,submissions,companyfacts,namespace):
    """Exact snapshot/backfill diagnostic replay, never historical identity proof."""
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed backfill namespace required')
    forms=','.join("'"+f+"'" for f in IB.PERIODIC)
    cand=_table(con,namespace+'_cand',f"""
        SELECT perm_security_id AS security_id, CAST(cik AS BIGINT) AS cik, link_primary AS snap_primary,
               available_at
        FROM {export}
        WHERE link_basis = 'current_ticker_verified' AND link_primary IN ('P', 'J') AND valid_until IS NULL
    """)
    con.execute(f"DELETE FROM {cand} WHERE security_id IN (SELECT security_id FROM {cand} GROUP BY 1 HAVING count(DISTINCT cik)>1)")
    first_filing=_table(con,namespace+'_first_filing',f"""
        SELECT cik, min(filed) AS first_filed FROM (
            SELECT CAST(cik AS BIGINT) AS cik, filed FROM {submissions} WHERE form IN ({forms})
            UNION ALL
            SELECT CAST(cik AS BIGINT) AS cik, filed_date AS filed FROM {companyfacts}
            WHERE form IN ({forms}) AND taxonomy = 'us-gaap'
        ) GROUP BY 1
    """)
    pres=_table(con,namespace+'_pres',f"""
        WITH c AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM {calendar}),
        p AS (
            SELECT p.security_id, p.session_date, c.sidx
            FROM {prices} p JOIN c USING (session_date)
            WHERE p.security_id IN (SELECT security_id FROM {namespace}_cand)
        ),
        g AS (
            SELECT *, CASE WHEN sidx - lag(sidx) OVER (PARTITION BY security_id ORDER BY sidx) > {IB.RUN_GAP_SESSIONS}
                           THEN 1 ELSE 0 END AS brk
            FROM p
        ),
        r AS (SELECT *, sum(brk) OVER (PARTITION BY security_id ORDER BY sidx ROWS UNBOUNDED PRECEDING) AS run_id FROM g)
        SELECT r.* FROM r
        JOIN (SELECT security_id, max(run_id) AS last_run, max(session_date) AS last_day FROM r GROUP BY 1) l
          ON r.security_id = l.security_id AND r.run_id = l.last_run
        WHERE l.last_day >= DATE '{IB.ALIVE_FROM}'
    """)
    other_cik_end=_table(con,namespace+'_other_cik_end',f"""
        SELECT s.security_id, max(s.end_incl) AS other_end
        FROM {strict} s JOIN {namespace}_cand c ON s.security_id = c.security_id AND s.cik <> c.cik
        GROUP BY 1
    """)
    bf_days=_table(con,namespace+'_bf_days',f"""
        SELECT p.security_id, p.session_date, p.sidx, c.cik, c.snap_primary, c.available_at
        FROM {namespace}_pres p
        JOIN {namespace}_cand c USING (security_id)
        JOIN {namespace}_first_filing f ON f.cik = c.cik AND f.first_filed <= p.session_date
        LEFT JOIN {namespace}_other_cik_end o ON o.security_id = p.security_id
        WHERE (o.other_end IS NULL OR p.session_date > o.other_end)
    """)
    con.execute(f"""
        DELETE FROM {namespace}_bf_days WHERE (security_id, session_date) IN (
            SELECT b.security_id, b.session_date FROM {namespace}_bf_days b JOIN {strict} s
              ON s.security_id = b.security_id AND b.session_date BETWEEN s.start AND s.end_incl)
    """)
    return {'backfill_days':bf_days}


def _native_combined(con,*,strict,backfill_days,name_days,calendar,namespace):
    """Complete native combined precedence and compression, diagnostic only.

    All name days are required, including those suppressed by original strict
    and backfill precedence. A later primary selection is not dated evidence.
    """
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed combined namespace required')
    sd=_table(con,namespace+'_strict_days',f"""WITH c AS(SELECT session_date,row_number() OVER(ORDER BY session_date) AS sidx FROM {calendar})
        SELECT s.security_id,c.session_date,c.sidx,s.cik,s."primary" AS strict_primary,s.tier,s.basis,s.available_at
        FROM {strict} s JOIN c ON c.session_date BETWEEN s.start AND s.end_incl WHERE c.session_date>=DATE '{IB.COMBINED_FROM}'""")
    days=_table(con,namespace+'_combined_days',f"""WITH u AS(
        SELECT security_id,session_date,sidx,cik,strict_primary,NULL AS snap_primary,tier,basis,available_at FROM {sd}
        UNION ALL SELECT security_id,session_date,sidx,cik,NULL,snap_primary,'backfill','snapshot_run_backfill',available_at FROM {backfill_days}
        UNION ALL SELECT n.security_id,n.session_date,c.sidx,n.cik,NULL,NULL,'name','finra_name_match',n.available_at
        FROM {name_days} n JOIN(SELECT session_date,row_number() OVER(ORDER BY session_date) AS sidx FROM {calendar}) c USING(session_date)
        ANTI JOIN {sd} s ON s.security_id=n.security_id AND s.session_date=n.session_date
        ANTI JOIN {backfill_days} b ON b.security_id=n.security_id AND b.session_date=n.session_date),
        pk AS(SELECT cik,session_date,coalesce(min(security_id) FILTER(WHERE strict_primary='P'),
            min(security_id) FILTER(WHERE tier='backfill' AND snap_primary='P'),min(security_id)) AS p_sid FROM u GROUP BY 1,2)
        SELECT u.security_id,u.session_date,u.sidx,u.cik,u.tier,u.basis,u.available_at,
            CASE WHEN u.security_id=pk.p_sid THEN 'P' ELSE 'J' END AS "primary" FROM u JOIN pk USING(cik,session_date)""")
    products={}
    for key,where,suffix in (('identity/links_backfill.parquet',"tier='backfill'",'backfill'),('identity/links_combined.parquet','TRUE','combined')):
        products[key]=_table(con,namespace+'_'+suffix,f"""WITH d AS(SELECT * FROM {days} WHERE {where}),
            k AS(SELECT *,sidx-row_number() OVER(PARTITION BY security_id,cik,"primary",tier,basis ORDER BY sidx) AS grp FROM d)
            SELECT security_id,cik,min(session_date) AS start,max(session_date) AS end_incl,"primary",tier,basis,max(available_at) AS available_at
            FROM k GROUP BY security_id,cik,"primary",tier,basis,grp ORDER BY security_id,start""")
    products['_strict_backfill_days']=_table(con,namespace+'_exclusions',f'SELECT security_id,session_date FROM {sd} UNION SELECT security_id,session_date FROM {backfill_days}')
    return products


class _MarketAdmission(dict):
    """Private immutable replay identity; a caller dictionary is never admitted."""
    def __init__(self,value):
        import copy
        super().__init__(copy.deepcopy(value))
        self._sealed=A.digest(value)
        self._active=False
        self._complete=False
        self._interrupted=False


def _consumption_check(admission,con):
    if not isinstance(admission,_MarketAdmission) or A.digest(dict(admission))!=admission._sealed:
        raise ValueError('market consumption requires unchanged native public replay admission')
    A._guard(con,A.absolute(admission['root']))
    _check(admission['dependency_pins'])
    for key,path in admission['products'].items():
        if admission['dependency_pins'].get(str(path))!=A.pin(path):
            raise ValueError('fixed immutable market product changed: '+key)


@contextlib.contextmanager
def consuming(admission,con):
    """One full immutable dependency check before/finally a bounded transaction.

    Iterator close or an exception always rechecks integrity, but cannot issue
    successful complete-consumption evidence. No scope or relation is reusable
    across transactions without repeating the full physical admission checks.
    """
    _consumption_check(admission,con)
    if admission._active:raise ValueError('market admission already in consumption scope')
    admission._active=True;admission._complete=False;admission._interrupted=False
    success=False
    try:
        yield admission['products']
        success=True
    finally:
        admission._active=False
        _consumption_check(admission,con)
        admission._complete=success and not admission._interrupted


def iter_rows(admission,product,con,batch_size=32):
    """Bounded typed rows; nested reads share only the active checked scope."""
    import pyarrow.parquet as pq
    if type(batch_size) is not int or not 1<=batch_size<=64 or product not in admission.get('products',{}):
        raise ValueError('fixed market product and native bounded row batch required')
    if not isinstance(admission,_MarketAdmission):raise ValueError('native market admission required')
    def rows():
        complete=False
        try:
            for batch in pq.ParquetFile(admission['products'][product]).iter_batches(batch_size=batch_size):
                yield from batch.to_pylist()
            complete=True
        finally:
            if not complete:admission._interrupted=True
    if admission._active:
        if A.digest(dict(admission))!=admission._sealed:raise ValueError('active market binding mutated')
        yield from rows()
    else:
        with consuming(admission,con):yield from rows()


def native_role(name,admission,con):
    """Resolve a complete immutable native role only in its checked transaction.

    This resolver cannot manufacture an admission from a manifest or legacy
    cache. Native identity clocks/tiers remain diagnostic unless separately
    qualified by original as-of evidence; this exposes original schema only.
    """
    if name not in NATIVE_ROLES or not isinstance(admission,_MarketAdmission) or not admission._active:
        raise ValueError('fixed native identity role requires active complete market admission')
    if A.digest(dict(admission))!=admission._sealed:raise ValueError('native role admission mutated')
    binding=admission.get('native_bindings',{}).get(name)
    keys={'security_master':('security_master/lines.parquet','security_master/finra_names.parquet'),
          'identity':('identity/link_table.parquet',)}[name]
    if not binding or any(key not in admission['products'] for key in keys):
        raise ValueError('complete native role and every original upstream dependency missing')
    manifest_path=A.absolute(binding['manifest'])
    if admission['dependency_pins'].get(str(manifest_path),{}).get('sha256')!=binding['sha256']:
        raise ValueError('native role manifest not bound to independently replayed admission')
    manifest=_json(manifest_path)
    if manifest.get('status')!='complete' or manifest.get('stage')!=name:
        raise ValueError('immutable native role contract differs')
    return {'products':{key:admission['products'][key] for key in keys},
            'relations':{key:_sql(admission['products'][key]) for key in keys},
            'schemas':{key:admission['schemas'][key] for key in keys},
            'dependency_pins':dict(admission['dependency_pins']),
            'native_binding':dict(binding),'source_quality_certified':False}


def _write_native_relations(con,relations,target):
    """Bounded immutable phase leaves preserving every native column and row.

    Input relations are fixed internally produced native phases, never public
    caller SQL. Each leaf is replayed with duplicate-preserving typed equality
    before it may enter a native role. No source qualification follows here.
    """
    root=Path(target)
    if root.exists():raise ValueError('retained native phase directory requires reconciliation')
    root.mkdir(parents=True)
    products={};validation={}
    try:
        for key,relation in sorted(relations.items()):
            relative=Path(key)
            if relative.is_absolute() or '..' in relative.parts or relative.suffix!='.parquet':
                raise ValueError('fixed native Parquet leaf required')
            path=root/relative;path.parent.mkdir(parents=True,exist_ok=True)
            C.copy_to_parquet(con,f'SELECT * FROM {relation}',path)
            _compare(con,f'SELECT * FROM {relation}',path)
            import pyarrow.parquet as pq
            schema=pq.read_schema(path)
            validation[key]={'rows':con.execute(f'SELECT count(*) FROM {relation}').fetchone()[0],
                'schema':str(schema),'identity':A.pin(path)}
            products[key]=path
        return products,validation
    except BaseException:
        # Partial immutable evidence is deliberately retained for controller
        # reconciliation, including failed typed replay; no activation occurs.
        raise


def _native_shares(con,*,prices,calendar,splits,links,names,events,namespace):
    """Full original shares phase, including every native bucket and exclusion.

    This diagnostic fit intentionally retains the native full-history type
    mode. Qualified shares must be fitted anew from complete as-of inputs;
    filtering this result afterward cannot establish historical availability.
    """
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed native shares namespace required')
    known,line_type,eqcnt,sec=replay_share_preparation(con,links,names,events,namespace+'_prepare')
    queries=[]
    for b in range(M.BUCKETS):
        bucket=_table(con,namespace+f'_prices_{b}',LR.price_query(prices,calendar,b))
        queries.append(shares_output_query(shares_query(bucket,splits,known,sec,eqcnt,line_type,b)))
    return _table(con,namespace+'_shares',' UNION ALL '.join('('+q+')' for q in queries))


def _native_liquidity(con,*,prices,calendar,shares,namespace):
    """Original complete liquidity phase; full source windows still need proof."""
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed native liquidity namespace required')
    cal=_table(con,namespace+'_calendar',LR.calendar_query(calendar))
    queries=[]
    for b in range(M.BUCKETS):
        bucket=_table(con,namespace+f'_prices_{b}',LR.price_query(prices,calendar,b))
        queries.append(LR.query(bucket,shares,cal,b))
    return _table(con,namespace+'_liquidity',' UNION ALL '.join('('+q+')' for q in queries))


def _full_form_share_primitives(con,*,filings,documents,target):
    """Bounded full-form/body projection retaining every missing candidate.

    Internal only: filings must come from complete native extraction plus the
    original Accepted scope; documents must be the authenticated fixed native
    event ledger scope. Neither a periodic-only census nor annual FT receipts
    can substitute. Returned records are primitives, not class-completeness or
    admitted share values. No modeled filing timestamp is promoted.
    """
    if not re.fullmatch(r't17_market_[a-z0-9_]+',filings):raise ValueError('fixed internal full-form relation required')
    columns={r[0] for r in con.execute('DESCRIBE '+filings).fetchall()}
    required={'cik','accession','form','primary_document','original_accepted_at','original_accepted_clock_supported'}
    if not required<=columns:raise ValueError('full original form and independently Accepted population required')
    if getattr(documents,'con',None) is not con:raise ValueError('same authenticated full-form/body transaction required')
    dest=Path(target)
    if dest.exists():raise ValueError('retained primitive publication requires reconciliation')
    seen=0;counts={'filings':0,'missing_document':0,'missing_body':0,'unresolved_Accepted':0,'parsed_facts':0}
    last=None
    with dest.open('x',encoding='utf-8') as out:
        while True:
            where='' if last is None else 'WHERE (cik,accession)>(?,?)'
            batch=con.execute(f'SELECT cik,accession,form,primary_document,original_accepted_at,original_accepted_clock_supported FROM {filings} {where} ORDER BY cik,accession LIMIT 64',[] if last is None else list(last)).fetchall()
            if not batch:break
            for cik,accession,form,primary,accepted,supported in batch:
                if type(cik) is not int or cik<=0 or not re.fullmatch(r'\d{10}-\d{2}-\d{6}',accession):
                    raise ValueError('native full-form filing owner/accession malformed')
                key=(cik,accession)
                if last is not None and key<=last:raise ValueError('full-form filing key duplicate or out of order')
                last=key;seen+=1;counts['filings']+=1
                record={'cik':cik,'accession':accession,'form':form,'primary_document':primary,
                    'original_accepted_at':None if accepted is None else accepted.isoformat(),
                    'original_accepted_clock_supported':bool(supported),'facts':[],
                    'source_quality_certified':False}
                if not supported:counts['unresolved_Accepted']+=1
                if not primary:
                    record['reason']='native_primary_document_missing';counts['missing_document']+=1
                else:
                    if '/' in primary or '\\' in primary or primary in ('.','..'):
                        raise ValueError('native primary document path malformed')
                    url=f'https://www.sec.gov/Archives/edgar/data/{cik}/{accession.replace("-","")}/{primary}'
                    record['url']=url
                    stored=con.execute('SELECT sha256,body_path,body_present FROM t17_event_body_registry WHERE url=?',[url]).fetchone()
                    entry=None if stored is None else dict(zip(('sha256','body_path','body_present'),stored))
                    blob=None if entry is None or not entry['body_present'] else documents.read(url,entry['sha256'])
                    if blob is None:
                        record['reason']='fixed_original_event_body_unreadable_or_missing';counts['missing_body']+=1
                    else:
                        facts=original_sec_ixbrl_primitives(entry['body_path'],cik,original_bytes=blob)
                        record['facts']=facts;record['reason']='parsed_original_primitives_not_share_admission'
                        counts['parsed_facts']+=len(facts)
                out.write(json.dumps(record,sort_keys=True,default=str,allow_nan=False)+'\n')
        out.flush();os.fsync(out.fileno())
    if seen!=con.execute('SELECT count(*) FROM '+filings).fetchone()[0]:raise ValueError('full original filing denominator changed')
    return {'path':dest,'identity':A.pin(dest),'counts':counts,'source_quality_certified':False}


def _native_core_members(con,*,prices,calendar,namespace):
    """Vendor-only upstream core/member replay with the full fixed native axis."""
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed native panel namespace required')
    core=_table(con,namespace+'_core',' UNION ALL '.join('('+panel_core_query(prices,calendar,b)+')' for b in range(P.BUCKETS)))
    # Original producer's output-year population, never inferred from an issuer
    # join, membership subset, or an arbitrary caller-selected year list.
    queries=[panel_membership_query(core,y) for y in range(2018,2027)]
    members=_table(con,namespace+'_members',' UNION ALL '.join('('+q+')' for q in queries))
    return {'core':core,'members':members}


def _native_strict(con,*,export,seal,namespace):
    """Disk-backed exact native r4-links interval algorithm; not PIT authority.

    Original modeled available_at and later-evidence defects remain diagnostic.
    Complete source documentary replay is required before any qualified dated
    strict/name census can use these rows, including absence/ambiguity queries.
    """
    if type(seal) is not dt.date or not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):
        raise ValueError('fixed original strict seal and namespace required')
    cutoff=f"TIMESTAMP '{seal.isoformat()} 00:00:00'";last=f"DATE '{(seal-dt.timedelta(days=1)).isoformat()}'"
    first="CAST(available_at-INTERVAL 22 HOUR AS DATE)+CASE WHEN available_at-INTERVAL 22 HOUR=CAST(CAST(available_at-INTERVAL 22 HOUR AS DATE) AS TIMESTAMP) THEN 0 ELSE 1 END"
    until="CAST(valid_until-INTERVAL 22 HOUR AS DATE)+CASE WHEN valid_until-INTERVAL 22 HOUR=CAST(CAST(valid_until-INTERVAL 22 HOUR AS DATE) AS TIMESTAMP) THEN 0 ELSE 1 END-1"
    versions=_table(con,namespace+'_versions',f"""SELECT CAST(perm_security_id AS BIGINT) AS sid,CAST(cik AS BIGINT) AS cik,
        row_number() OVER() AS source_ordinal,greatest(link_start,{first}) AS start,least(coalesce(link_end,{last}),{last},coalesce({until},{last})) AS end_incl,
        link_basis AS basis,link_primary AS "primary",tier,available_at FROM {export}
        WHERE available_at IS NOT NULL AND available_at<{cutoff}""")
    visible=_table(con,namespace+'_visible',f'SELECT * FROM {versions} WHERE start<=end_incl')
    linebounds=_table(con,namespace+'_linebounds',f"""WITH b AS(SELECT sid,start AS day FROM {visible} UNION SELECT sid,end_incl+1 AS day FROM {visible})
        SELECT sid,day AS start,lead(day) OVER(PARTITION BY sid ORDER BY day)-1 AS end_incl FROM b""")
    active=_table(con,namespace+'_active',f"""SELECT b.sid,b.start,b.end_incl,v.cik,v.basis,v."primary",v.tier,v.available_at,v.source_ordinal
        FROM {linebounds} b JOIN {visible} v ON v.sid=b.sid AND v.start<=b.start AND v.end_incl>=b.end_incl""")
    allowed_tiers=','.join("'"+x+"'" for x in sorted(IL.KEEP_TIERS))
    allowed_bases=','.join("'"+x+"'" for x in sorted(IL.KEEP_BASES))
    dropped_bases=','.join("'"+x+"'" for x in sorted(IL.DROP_BASES))
    unambiguous=_table(con,namespace+'_unambiguous',f'SELECT sid,start,end_incl FROM {active} GROUP BY 1,2,3 HAVING count(DISTINCT cik)=1')
    kept=_table(con,namespace+'_kept',f"""SELECT a.* FROM {active} a JOIN {unambiguous} u USING(sid,start,end_incl)
        WHERE (tier IN({allowed_tiers}) OR basis IN({allowed_bases})) AND "primary" IN('P','J')
        AND (basis IS NULL OR basis NOT IN({dropped_bases}))""")
    lines=_table(con,namespace+'_line_segments',f"""WITH ranked AS(SELECT *,max(available_at) OVER(PARTITION BY sid,start,end_incl) AS known,
        row_number() OVER(PARTITION BY sid,start,end_incl ORDER BY available_at DESC,coalesce(tier='high',false) DESC,basis DESC NULLS LAST,source_ordinal) AS rn FROM {kept})
        SELECT sid,cik,start,end_incl,tier,basis,known AS available_at FROM ranked WHERE rn=1""")
    companybounds=_table(con,namespace+'_companybounds',f"""WITH b AS(SELECT cik,start AS day FROM {lines} UNION SELECT cik,end_incl+1 AS day FROM {lines})
        SELECT cik,day AS start,lead(day) OVER(PARTITION BY cik ORDER BY day)-1 AS end_incl FROM b""")
    pieces=_table(con,namespace+'_pieces',f"""WITH a AS(SELECT l.sid,b.cik,b.start,b.end_incl,l.tier,l.basis,l.available_at
        FROM {companybounds} b JOIN {lines} l ON l.cik=b.cik AND l.start<=b.start AND l.end_incl>=b.end_incl)
        SELECT *,CASE WHEN sid=min(sid) OVER(PARTITION BY cik,start,end_incl) THEN 'P' ELSE 'J' END AS "primary" FROM a""")
    result=_table(con,namespace+'_strict_links',f"""WITH prior AS(SELECT *,lag(end_incl) OVER w AS previous_end,
        lag(cik) OVER w AS previous_cik,lag("primary") OVER w AS previous_primary,lag(tier) OVER w AS previous_tier,
        lag(basis) OVER w AS previous_basis,lag(available_at) OVER w AS previous_clock FROM {pieces} WINDOW w AS(PARTITION BY sid ORDER BY start)),
        flags AS(SELECT *,CASE WHEN previous_end=start-1 AND previous_cik=cik AND previous_primary="primary"
            AND previous_tier IS NOT DISTINCT FROM tier AND previous_basis IS NOT DISTINCT FROM basis
            AND previous_clock IS NOT DISTINCT FROM available_at THEN 0 ELSE 1 END AS brk FROM prior),
        islands AS(SELECT *,sum(brk) OVER(PARTITION BY sid ORDER BY start ROWS UNBOUNDED PRECEDING) AS island FROM flags)
        SELECT sid AS security_id,cik,min(start) AS start,max(end_incl) AS end_incl,"primary",tier,basis,available_at
        FROM islands GROUP BY sid,cik,"primary",tier,basis,available_at,island ORDER BY security_id,start""")
    stats={'versions':con.execute('SELECT count(*) FROM '+export).fetchone()[0],
        'visible_versions':con.execute('SELECT count(*) FROM '+visible).fetchone()[0],
        'ambiguous_line_days':con.execute(f'SELECT coalesce(sum(end_incl-start+1),0) FROM(SELECT sid,start,end_incl FROM {active} GROUP BY 1,2,3 HAVING count(DISTINCT cik)>1)').fetchone()[0],
        'multi_version_line_days':con.execute(f'SELECT coalesce(sum(end_incl-start+1),0) FROM(SELECT sid,start,end_incl FROM {kept} GROUP BY 1,2,3 HAVING count(*)>1)').fetchone()[0]}
    return {'identity/links.parquet':result,'stats':stats}


def _original_r4_catalog(build_path,receipt_path,ops_manifest,review_path,expected_pins):
    """Derive original r4 source closure from retained native records themselves.

    This is authentication for a subsequent complete producer replay, never
    admission or repair of later-evidence/primary/name precedence. Caller seals
    cannot choose a smaller source/document population than the original build.
    """
    paths=tuple(A.absolute(p) for p in (build_path,receipt_path,ops_manifest,review_path))
    for path in paths:
        if expected_pins.get(str(path))!=A.pin(path):raise ValueError('original r4 control record missing or changed')
    build=_json(paths[0])['pin'];receipt=_json(paths[1])['inputs'];ops=_json(paths[2]);review=_json(paths[3])
    pins={str(path):expected_pins[str(path)] for path in paths}
    fixed=('ticker_history','companyfacts','submissions','company_tickers')
    if any(name not in build['inputs'] or name not in receipt for name in fixed):raise ValueError('complete original r4 native source roles missing')
    for name in fixed:
        source=build['inputs'][name];other=receipt[name]
        for key in ('path','sha256','received_at','receipt_basis'):
            if source[key]!=other[key]:raise ValueError('original r4 source/receipt contradiction')
        path=A.absolute(source['path']);identity=expected_pins.get(str(path))
        if not identity or identity['sha256']!=source['sha256']:raise ValueError('original r4 source not independently pinned')
        pins[str(path)]=identity
    if expected_pins[str(paths[2])]['sha256']!=build['ops_manifest_sha256']:raise ValueError('original r4 OPS source differs')
    for item in ops['exports']:
        if item.get('file') is None:continue
        path=A.absolute(paths[2].parent/item['file'])
        if not path.is_relative_to(paths[2].parent):raise ValueError('original OPS export escapes source root')
        identity={'bytes':item['bytes'],'sha256':item['sha256']}
        if expected_pins.get(str(path))!=identity:raise ValueError('complete original r4 OPS population missing')
        pins[str(path)]=identity
    original_review=build['document_review']
    if (str(paths[3])!=str(A.absolute(original_review['path'])) or
        pins[str(paths[3])]['sha256']!=original_review['sha256']):raise ValueError('original r4 reviewed document inventory differs')
    docs={}
    for item in review['assertions']:
        path=str(A.absolute(item['document_path']));sha=item['document_sha256']
        if path in docs and docs[path]!=sha:raise ValueError('original r4 conflicting documentary evidence')
        docs[path]=sha
    expected_docs={str(A.absolute(path)):sha for path,sha in original_review['documents'].items()}
    if not docs or docs!=expected_docs:raise ValueError('complete original r4 document population differs')
    for path,sha in docs.items():
        if expected_pins.get(path,{}).get('sha256')!=sha:raise ValueError('original r4 documentary body absent from independent pins')
        pins[path]=expected_pins[path]
    source=build['inputs']['submissions'];archive=A.absolute(source['path'])
    index=archive.with_name(archive.name+'.directory-v1-'+source['sha256']+'.sqlite3')
    for path in (index,index.with_suffix('.sqlite3.sha256')):
        if str(path) not in expected_pins:raise ValueError('original r4 native archive directory/cache receipt absent')
        pins[str(path)]=expected_pins[str(path)]
    for path,identity in pins.items():
        if A.pin(path)!=identity:raise ValueError('original r4 complete source bytes changed: '+path)
    return {'build':build,'inputs':build['inputs'],'document_review':review,'ops':ops,
            'source_dependency_pins':pins,'documents':docs,'source_quality_certified':False,
            'reason':'complete_original_authentication_requires_native_producer_and_separate_PIT_replay'}


def _original_r4_phase_catalog(rehearsal_root,expected_pins):
    """Authenticate every original phase and export before native replay.

    Original scope_complete:false and remaining_acceptance are preserved, never
    rewritten into a qualified final envelope. Complete original producer replay
    and genuine as-of reconstruction are additional required operations.
    """
    from .. import identity_links as native
    root=A.absolute(rehearsal_root)
    build_path=root/'build.json';manifest_path=root/'manifest.json'
    required={};receipts={};products={}
    def read(path):
        path=A.absolute(path)
        if not path.is_relative_to(root):raise ValueError('original r4 phase escapes fixed source root')
        identity=expected_pins.get(str(path))
        if not identity or A.pin(path)!=identity:raise ValueError('original r4 phase/control source missing or changed')
        required[str(path)]=identity
        return _json(path)
    build=read(build_path);manifest=read(manifest_path)
    if manifest.get('rehearsal') is not True or manifest.get('schema_version')!=native.SCHEMA_VERSION:
        raise ValueError('actual original rehearsal envelope differs')
    fixed_code=('identity_links.py','identity_reconstruction.py','market_owner_bridge.py',
                'calendar.py','_submissions_archive.py','universe_us_listed.py',
                'historical_identity.py','historical_identity_sources.py','_fundamental_clock.py',
                'ticker_history.py','market_daily.py','warehouse.py')
    if set(build['pin']['code_files'])!=set(fixed_code):
        raise ValueError('complete original twelve-module producer snapshot required')
    for name in fixed_code:
        path=A.absolute(root/'code_snapshot'/name)
        identity=expected_pins.get(str(path))
        if not identity or identity['sha256']!=build['pin']['code_files'][name] or A.pin(path)!=identity:
            raise ValueError('original producer snapshot bytes missing or changed: '+name)
        current=Path(native.__file__).parent/name
        def lf(body):return body.replace(b'\r\n',b'\n').replace(b'\r',b'\n')
        if max(path.stat().st_size,current.stat().st_size)>4*1024*1024:
            raise ValueError('original producer source exceeds bounded code comparison')
        if lf(path.read_bytes())!=lf(current.read_bytes()):
            raise ValueError('loaded original producer source differs beyond line endings: '+name)
        required[str(path)]=identity
    for name in native.REHEARSAL_PHASES:
        path=root/'phases'/(name+'.json');receipt=read(path)
        if receipt['build_sha256']!=required[str(build_path)]['sha256'] or manifest['phases'][name]!=required[str(path)]['sha256']:
            raise ValueError('original r4 full phase ancestry differs')
        for item in receipt['files']:
            leaf=A.absolute(root/item['file'])
            if not leaf.is_relative_to(root):raise ValueError('original r4 payload escapes fixed source root')
            identity={'bytes':item['bytes'],'sha256':item['sha256']}
            if expected_pins.get(str(leaf))!=identity or A.pin(leaf)!=identity:
                raise ValueError('complete original r4 phase payload differs')
            required[str(leaf)]=identity
            products.setdefault((name,item['table']),[]).append(leaf)
        receipts[name]=receipt
    if manifest['files']!=receipts['export']['files']:raise ValueError('original r4 final export catalog differs')
    for path,identity in required.items():
        if A.pin(path)!=identity:raise ValueError('original r4 phase changed during full catalog authentication')
    return {'build':build,'manifest':manifest,'phases':receipts,'products':products,
            'dependency_pins':required,'source_quality_certified':False,
            'native_source_scope_complete':manifest.get('scope_complete'),
            'remaining_acceptance':manifest.get('remaining_acceptance')}


def _native_r4_link_versions(con,*,candidates,unresolved,created_at,namespace):
    """Reuse original r4 per-company version kernel with bounded source slices.

    This preserves original economics/primary decisions, including their known
    PIT limitations. It is one phase of complete producer replay; authenticated
    candidate/evidence bytes alone cannot stand in for preceding source phases.
    """
    from .. import identity_links as native
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace) or type(created_at) is not dt.datetime:
        raise ValueError('fixed original r4 version namespace and build clock required')
    output={name:namespace+'_'+name for name in ('company_permanent_ids','security_company_links')}
    occupied={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if any(name in occupied for name in output.values()):raise ValueError('native r4 version namespace occupied')
    for name,table in output.items():con.execute('CREATE TABLE '+table+'('+native.TABLE_COLUMNS[name]+')')
    census=_table(con,namespace+'_cik_census',f'SELECT cik FROM {candidates} UNION SELECT cik FROM {unresolved}')
    last=None;count=0;maximum=0
    while True:
        condition='' if last is None else 'WHERE cik>?'
        ciks=con.execute(f'SELECT cik FROM {census} {condition} ORDER BY cik LIMIT 64',[] if last is None else [last]).fetchall()
        if not ciks:break
        for (cik,) in ciks:
            cursor=con.execute(f'SELECT * FROM {candidates} WHERE cik=?',[cik])
            columns=[field[0] for field in cursor.description]
            rows=[]
            while batch:=cursor.fetchmany(64):
                rows.extend(dict(zip(columns,row)) for row in batch)
                if len(rows)>256 or sum(len(json.dumps(row,default=str)) for row in rows)>4*1024*1024:
                    raise ValueError('complete native r4 company slice exceeds bounded replay; no truncation allowed')
            versions=native._company_versions(rows,created_at)
            native._insert_rows(con,output['security_company_links'],versions)
            native._insert_rows(con,output['company_permanent_ids'],[(int(cik),cik,'SEC_CIK_permanent_v1',created_at)])
            maximum=max(maximum,len(versions));count+=1;last=cik
    if count!=con.execute('SELECT count(*) FROM '+census).fetchone()[0]:raise ValueError('full original r4 company census changed')
    return {'products':output,'companies':count,'max_company_versions':maximum,'source_quality_certified':False}


@contextlib.contextmanager
def _native_r4_vendor_scope(con,vendor_path,dependency_pins):
    """Original preparation vendor phase on the explicit bounded connection.

    No rehearsal entrypoint, resource override, global root or reusable scratch
    database is invoked. Original64 vendor-id slices and all native price/share
    populations are preserved. The surrounding source stage must subsequently
    replay facts/reconstruction/evidence and every rehearsal phase.
    """
    from .. import identity_reconstruction as native
    import copy
    path=A.absolute(vendor_path);pins=copy.deepcopy(dependency_pins)
    A._guard(con,path.parent)
    if str(path) not in pins:raise ValueError('complete original r4 vendor source not bound')
    _check(pins)
    owned=('ri_vendor_lines','ri_vendor_symbols','ri_share_runs','ri_share_obs','ri_operating_lines')
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if any(name in existing for name in owned):raise ValueError('original r4 preparation namespace occupied')
    try:
        native.stage_vendor_ticker_history(con,path,chunks=64)
        con.execute('CREATE TABLE ri_operating_lines AS SELECT DISTINCT securityID::BIGINT AS vendor_id FROM read_parquet(?) WHERE securityID>0 AND earnFlag=\'0\'',[str(path)])
        yield {'vendor_lines':'ri_vendor_lines','vendor_symbols':'ri_vendor_symbols','share_runs':'ri_share_runs','operating_lines':'ri_operating_lines'}
    finally:
        try:
            _check(pins);A._guard(con,path.parent)
            if dependency_pins!=pins:raise ValueError('original r4 vendor binding mutated during replay')
        finally:
            for table in reversed(owned):con.execute('DROP TABLE IF EXISTS '+table)


def _native_r4_share_facts(con,companyfacts_path):
    """Original streaming fact preparation, with explicit resource refusals.

    Internal complete source stage only. Oversized originals cause refusal,
    never omission or a smaller qualifying universe. Native values/concepts,
    duplicate members, missing fields and row interpretation stay unchanged.
    """
    from .. import identity_reconstruction as native
    import zipfile
    path=A.absolute(companyfacts_path)
    A._guard(con,path.parent)
    if 'ri_share_facts' in {r[0] for r in con.execute('SHOW TABLES').fetchall()}:
        raise ValueError('original r4 fact namespace occupied')
    # Native JSON decoding materializes one complete member. Authenticate the
    # complete native eligible directory and refuse unsupported working sets
    # before calling that unchanged decoder; do not yield a truncated body.
    count=0;maximum=0
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if not(info.filename.startswith('CIK') and info.filename.endswith('.json')):continue
            try:native.normalize_cik(info.filename[3:-5])
            except ValueError:continue
            if info.file_size>40*1024*1024:raise ValueError('complete original companyfacts member exceeds bounded native JSON replay')
            count+=1;maximum=max(maximum,info.file_size)
    rows=native.stage_share_facts(con,native.iter_companyfacts_share_facts(path),batch=10_000)
    return {'relation':'ri_share_facts','rows':rows,'native_eligible_members':count,
            'maximum_original_member_bytes':maximum,'source_quality_certified':False}


def _r4_vendor_lines(con,after=None,batch_size=64):
    """Original VendorLine projection with source-order keys and bounded batch."""
    from .. import identity_reconstruction as native
    if type(batch_size) is not int or not 1<=batch_size<=64:raise ValueError('fixed bounded original line batch required')
    where='' if after is None else 'WHERE vendor_id>?'
    values=con.execute(f'SELECT * FROM ri_vendor_lines {where} ORDER BY vendor_id LIMIT {batch_size}',[] if after is None else [after]).fetchall()
    result=[]
    for vid,first,last,count,symbol,today in values:
        cursor=con.execute('SELECT symbol,first_date,last_date FROM ri_vendor_symbols WHERE vendor_id=? ORDER BY first_date,symbol',[vid])
        symbols=[]
        while batch:=cursor.fetchmany(64):
            symbols.extend((str(name),lo,hi) for name,lo,hi in batch)
            if len(symbols)>4096:raise ValueError('complete original line symbol history exceeds bounded source replay')
        result.append(native.VendorLine(vendor_id=int(vid),price_security_id=f'TBLTICKERHISTORY-{int(vid)}',
            first_trade_date=first,last_trade_date=last,bar_rows=int(count),last_symbol=symbol,symbols=tuple(symbols),current_cik=None))
    return result


def _r4_lifecycle_filings(archive,ciks):
    """Original lifecycle loop over a single already authenticated archive scope.

    Avoids reopening and whole-ZIP hashing for each reconstruction batch. All
    native member/filter/clock semantics remain diagnostic, and original main
    reader bounds are unchanged. Full all-form source closure is a separate
    prerequisite; missing native eligible files cannot confer qualification.
    """
    from .. import identity_reconstruction as native
    forms=set(native.LIFECYCLE_FORMS);since=dt.date(2009,1,1);out=[]
    for cik in sorted(set(ciks)):
        name=f'CIK{cik}.json'
        if name not in archive:continue
        data=json.loads(archive.read(name))
        def blocks():
            yield (data.get('filings') or {}).get('recent') or {}
            for extra in (data.get('filings') or {}).get('files') or ():
                if (extra.get('filingTo') or '9999')>=since.isoformat() and extra.get('name') in archive:
                    yield json.loads(archive.read(extra['name']))
        for block in blocks():
            entries=block.get('form') or [];dates=block.get('filingDate') or []
            accessions=block.get('accessionNumber') or [];stamps=block.get('acceptanceDateTime') or []
            for i,form in enumerate(entries):
                if form not in forms or i>=len(dates):continue
                filed=native._date(dates[i])
                if filed is None or filed<since:continue
                out.append(native.IssuerFiling(cik,form,filed,accessions[i] if i<len(accessions) else None,
                    native._acceptance_utc(stamps[i]) if i<len(stamps) else None))
                if len(out)>65536:raise ValueError('complete original lifecycle batch exceeds bounded source replay')
    return out


def _native_r4_reconstruction(con,*,inputs,archive,target,revision_key):
    """Original per-line reconstruction, bounded without changing global fit axes.

    The global horizon/price start derive from every original vendor line before
    batching. Native lifecycle/model clocks and all evidence remain diagnostic;
    complete original source admission and causal reruns are separate duties.
    """
    from .. import identity_reconstruction as native
    import pyarrow as pa,pyarrow.parquet as pq
    root=Path(target)
    if root.exists():raise ValueError('retained original reconstruction needs controller reconciliation')
    root.mkdir(parents=True)
    params=native.ReconstructionParams()
    horizon,price_start=con.execute('SELECT max(last_trade_date),min(first_trade_date) FROM ri_vendor_lines').fetchone()
    if horizon is None:raise ValueError('complete original vendor line population empty')
    tickers=native.read_sec_ticker_snapshot(inputs['company_tickers']['path'])
    ticker_clock=dt.datetime.fromisoformat(inputs['company_tickers']['received_at'])
    facts_clock=dt.datetime.fromisoformat(inputs['companyfacts']['received_at'])
    artifacts={name+'_sha256':item['sha256'] for name,item in inputs.items()}
    files={};rows=0;line_count=0;last=None;ordinal=0
    while lines:=_r4_vendor_lines(con,last,64):
        matches=native.match_share_counts(con,params,vendor_ids=[line.vendor_id for line in lines])
        ciks={str(row[1]) for row in matches}
        filings=_r4_lifecycle_filings(archive,ciks)
        result=native.reconstruct_issuer_links(con,lines,filings=filings,tickers=tickers,
            ticker_observed_at=ticker_clock,params=params,horizon=horizon,price_start=price_start,matches=matches)
        evidence=result.evidence_rows(observed_at=facts_clock,source_loaded_at=facts_clock,
            run_id='identity-rehearsal-preparation',artifact_sha256=inputs['companyfacts']['sha256'],
            artifacts=artifacts,revision_key=revision_key)
        key=f'part-{ordinal:04d}.parquet';path=root/key
        pq.write_table(pa.Table.from_pylist(evidence,schema=native._evidence_schema()),path,compression='zstd')
        files[key]={'identity':A.pin(path),'rows':len(evidence)}
        rows+=len(evidence);line_count+=len(lines);last=lines[-1].vendor_id;ordinal+=1
    if line_count!=con.execute('SELECT count(*) FROM ri_vendor_lines').fetchone()[0]:raise ValueError('complete native reconstruction line denominator changed')
    return {'products':{key:root/key for key in files},'files':files,'evidence_rows':rows,
            'vendor_lines':line_count,'global_horizon':horizon,'global_price_start':price_start,
            'source_quality_certified':False}


def _native_r4_names(con,*,vendor_symbols,candidates,archive,observed_at,namespace):
    """Original complete names phase with separately preserved snapshot clocks."""
    from .. import identity_links as native
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace) or type(observed_at) is not dt.datetime:
        raise ValueError('fixed original names namespace and receipt clock required')
    table=namespace+'_names'
    if table in {r[0] for r in con.execute('SHOW TABLES').fetchall()}:raise ValueError('native r4 names namespace occupied')
    con.execute('CREATE TABLE '+table+'('+native.TABLE_COLUMNS['security_names_history']+')')
    con.execute(f"INSERT INTO {table} SELECT vendor_id,symbol,NULL,NULL,first_date,last_date,'vendor_ticker_interval_reconstructed',CAST(first_date AS TIMESTAMP)+INTERVAL 22 HOUR FROM {vendor_symbols}")
    last=None;count=0
    while True:
        predicate='' if last is None else 'WHERE cik>?'
        batch=con.execute(f'SELECT DISTINCT cik FROM {candidates} {predicate} ORDER BY cik LIMIT 64',[] if last is None else [last]).fetchall()
        if not batch:break
        for (cik,) in batch:
            last=cik;count+=1;member=f'CIK{cik}.json'
            if member not in archive:continue
            payload=json.loads(archive.read(member))
            spans={}
            cursor=con.execute(f'SELECT DISTINCT perm_security_id,link_start,link_end FROM {candidates} WHERE cik=?',[cik])
            n=0
            while records:=cursor.fetchmany(64):
                for vid,lo,hi in records:spans.setdefault(vid,set()).add((lo,hi));n+=1
                if n>4096:raise ValueError('complete native name company slice exceeds bounded replay')
            names=set()
            for vid,intervals in spans.items():
                if payload.get('name') and any(lo<=observed_at.date() and(hi is None or hi>=observed_at.date()) for lo,hi in intervals):
                    names.add((vid,None,str(payload['name']),None,observed_at.date(),None,'sec_current_name_snapshot',observed_at))
                for former in payload.get('formerNames') or ():
                    if not former.get('name') or not former.get('from') or not former.get('to'):continue
                    first=dt.date.fromisoformat(former['from'][:10]);end=dt.date.fromisoformat(former['to'][:10])
                    for lo,hi in intervals:
                        low,high=max(first,lo),min(end,hi or dt.date.max)
                        if low<=high:
                            names.add((vid,None,str(former['name']),None,low,high,'sec_former_name_reconstructed',observed_at))
                            if len(names)>65536:raise ValueError('complete original name expansion exceeds bounded replay')
                if len(names)>65536:raise ValueError('complete original name expansion exceeds bounded replay')
            native._insert_rows(con,table,sorted(names,key=lambda row:tuple(str(value) for value in row)))
    if count!=con.execute(f'SELECT count(DISTINCT cik) FROM {candidates}').fetchone()[0]:raise ValueError('complete original name issuer population changed')
    distinct=_table(con,namespace+'_names_distinct','SELECT DISTINCT * FROM '+table)
    return {'security_names_history':distinct,'source_quality_certified':False}


def _native_r4_symbols(con,*,vendor_path,created):
    """Replay the unchanged original symbols phase on the authenticated vendor.

    These islands use the original modeled session axis and remain diagnostic.
    The permanent-ID relation is kept separately from the candidate namespace;
    it must be joined back only in the original final-export phase.
    """
    from .. import identity_links as native,identity_reconstruction as ir
    owned={'identity_sessions','identity_vendor_symbols','identity_symbol_days',
           't17_market_r4_symbol_permanent_ids'}
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if existing&owned:raise ValueError('original symbols namespace occupied')
    vendor,rejected=native._dated_vendor_symbols(con,A.absolute(vendor_path),ir.read_vendor_lines(con),chunks=64)
    target='t17_market_r4_symbol_permanent_ids'
    con.execute('CREATE TABLE '+target+'('+native.TABLE_COLUMNS['security_permanent_ids']+')')
    for begin in range(0,len(vendor),1000):
        native._insert_rows(con,target,[(line.vendor_id,line.price_security_id,
            line.first_trade_date,line.last_trade_date,created) for line in vendor[begin:begin+1000]])
    return {'identity_vendor_symbols':'identity_vendor_symbols','security_permanent_ids':target,
            'vendor_lines':len(vendor),'ambiguous_or_missing_symbol_days':rejected,
            'source_quality_certified':False}


@contextlib.contextmanager
def _native_r4_ops_scope(con,*,ops_manifest,ops,dependency_pins):
    """Install exactly the five original OPS exports without global overrides.

    The original bridge discovers persistent tables. Full catalog authentication
    remains a prerequisite; this scope neither accepts a caller relation nor
    certifies the snapshot's classifications as historical facts.
    """
    names=('securities','exchange_listings','sec_company_tickers',
           'nasdaq_symbol_directory','security_identifier_history')
    root=A.absolute(ops_manifest).parent
    selected={}
    for entry in ops['exports']:
        if entry.get('file') and entry['table'] in names:
            if entry['table'] in selected:raise ValueError('duplicate original OPS role')
            path=A.absolute(root/entry['file'])
            if not path.is_relative_to(root):raise ValueError('original OPS export escapes root')
            identity={'bytes':entry['bytes'],'sha256':entry['sha256']}
            if dependency_pins.get(str(path))!=identity:raise ValueError('original OPS independent source pin differs')
            selected[entry['table']]=(path,identity)
    if set(selected)!=set(names):raise ValueError('complete five-role original OPS population required')
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if existing&set(names):raise ValueError('original OPS namespace occupied')
    created=[]
    try:
        for name in names:
            path,identity=selected[name]
            if A.pin(path)!=identity:raise ValueError('original OPS bytes changed')
            con.execute('CREATE TABLE '+name+' AS SELECT * FROM read_parquet(?)',[str(path)])
            created.append(name)
        yield {name:name for name in names}
    finally:
        try:
            for path,identity in selected.values():
                if A.pin(path)!=identity:raise ValueError('original OPS bytes changed during consumption')
        finally:
            for name in reversed(created):con.execute('DROP TABLE '+name)


def _native_r4_export_relations(con,*,symbols,links,names,candidates,uncertainty,namespace):
    """Original complete final table population, including both uncertainty axes.

    Duplicate native keys refuse publication. This phase does not turn the
    rehearsal's diagnostic coverage or modeled clocks into source admission.
    """
    from .. import identity_links as native
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):
        raise ValueError('fixed original export namespace required')
    sources={'security_permanent_ids':symbols['security_permanent_ids'],
             'company_permanent_ids':links['products']['company_permanent_ids'],
             'security_company_links':links['products']['security_company_links'],
             'security_names_history':names['security_names_history']}
    outputs={}
    for name in native.TABLE_COLUMNS:
        if name in sources:query='SELECT * FROM '+sources[name]
        elif name in ('identity_link_evidence','identity_unresolved_memberships'):
            query='SELECT * FROM '+candidates['products'][name]+' UNION ALL SELECT * FROM '+uncertainty['products'][name]
        else:raise ValueError('unimplemented original final role: '+name)
        table=_table(con,namespace+'_'+name,query)
        keys=','.join(native.TABLE_KEYS[name])
        if con.execute('SELECT 1 FROM '+table+' GROUP BY '+keys+' HAVING count(*)>1 LIMIT 1').fetchone():
            raise ValueError('duplicate original final native keys: '+name)
        outputs[name]=table
    return {'relations':outputs,'source_quality_certified':False,
            'requires_original_public_view_audit_and_complete_typed_replay':True}


@contextlib.contextmanager
def _native_r4_final_scope(con,export,original_catalog):
    """Reuse the original public-view and audit kernels over final phase outputs.

    Phase-local fixed candidate tables must have been retained in separate
    outputs and removed before entry. Existing native tables/views are refused;
    no protected relation is replaced. The native audit does not establish
    closed-world membership completeness or correct known PIT defects.
    """
    from .. import identity_links as native
    relations=export['relations']
    if set(relations)!=set(native.TABLE_COLUMNS):raise ValueError('complete original final table roles required')
    owned=tuple(native.TABLE_COLUMNS)+('v_security_master_public',)
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if existing&set(owned):raise ValueError('original final audit namespace occupied')
    installed=[]
    try:
        for name in native.TABLE_COLUMNS:
            con.execute('CREATE TEMP VIEW '+name+' AS SELECT * FROM '+relations[name])
            installed.append(name)
        native.create_security_master_public_view(con)
        installed.append('v_security_master_public')
        audit=native.audit_identity(con)
        expected=original_catalog['phases']['export']['details']['audit']
        if audit!=expected:raise ValueError('complete original native final audit differs')
        yield {'relations':relations,'public_view':'v_security_master_public',
               'native_audit':audit,'source_quality_certified':False}
    finally:
        for name in reversed(installed):con.execute('DROP VIEW '+name)


def _native_r4_compare_phase(con,*,phase,relations,original_catalog):
    """Compare every retained original phase table, with full typed multiplicity.

    Full source-derived catalogs determine the table/file population. Hashes
    authenticate originals; neither matching hashes nor this diagnostic replay
    establish historical economic or PIT qualification.
    """
    from .. import identity_links as native
    if phase not in native.REHEARSAL_PHASES:raise ValueError('fixed original phase required')
    originals={name:paths for (owner,name),paths in original_catalog['products'].items() if owner==phase}
    if not originals or set(relations)!=set(originals):raise ValueError('complete original phase table population differs')
    rows={}
    for name in sorted(originals):
        paths=originals[name]
        for path in paths:
            if A.pin(path)!=original_catalog['dependency_pins'][str(path)]:
                raise ValueError('original phase payload changed before typed replay')
        source=_sql(paths)
        replay=relations[name]
        left=con.execute('DESCRIBE SELECT * FROM '+source).fetchall()
        right=con.execute('DESCRIBE SELECT * FROM '+replay).fetchall()
        if [(r[0],r[1]) for r in left]!=[(r[0],r[1]) for r in right]:
            raise ValueError('original phase typed schema differs: '+name)
        for first,second in ((source,replay),(replay,source)):
            if con.execute('SELECT 1 FROM ((SELECT * FROM '+first+') EXCEPT ALL (SELECT * FROM '+second+')) LIMIT 1').fetchone():
                raise ValueError('original full phase typed values/multiplicity differ: '+name)
        rows[name]=con.execute('SELECT count(*) FROM '+source).fetchone()[0]
    for paths in originals.values():
        for path in paths:
            if A.pin(path)!=original_catalog['dependency_pins'][str(path)]:
                raise ValueError('original phase payload changed during typed replay')
    return {'phase':phase,'rows':rows,'complete_native_typed_replay':True,
            'source_quality_certified':False}


def _native_r4_candidates(con,*,inputs,ops,evidence_files,document_review,created):
    """Explicit-connection original candidate phase; every clock stays diagnostic.

    Fixed source orchestration must install full original vendor/symbol/OPS and
    reconstruction relations after original-source replay. No caller Parquet or
    review assertion is itself source/PIT qualification. Native per-point
    evidence and entire unresolved population remain unchanged.
    """
    from types import SimpleNamespace
    from collections import defaultdict
    from .. import identity_links as native,identity_reconstruction as ir,market_owner_bridge as bridge
    snapshot_clock=dt.datetime(2026,9,20,22)
    details={};evidence_rows=[];unresolved_rows=[];candidate_rows=[]
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    owned=set(native.TABLE_COLUMNS)|{'identity_candidates','identity_document_assertions'}
    if existing&owned:raise ValueError('original candidate namespace occupied')
    # The unchanged bridge fits the whole native line/symbol population in
    # Python. Refuse unsupported complete working sets before materialization;
    # never select a smaller qualifying cohort to make the fit succeed.
    if con.execute('SELECT count(*) FROM ri_vendor_lines').fetchone()[0]>65536:
        raise ValueError('complete original candidate line working set exceeds bound')
    if con.execute('SELECT count(*) FROM identity_vendor_symbols').fetchone()[0]>262144:
        raise ValueError('complete original candidate symbol working set exceeds bound')
    vendor = native._load_dated_vendor(con)
    line_by_id = {line.price_security_id: line for line in vendor}
    def bounded_evidence():
        count=0;size=0
        for row in native._evidence_rows(con,evidence_files):
            count+=1;size+=len(json.dumps(row,default=str).encode('utf-8'))
            if count>262144 or size>32*1024*1024:
                raise ValueError('complete original reconstruction evidence working set exceeds bound')
            yield row
    reconstructed = bridge.reconstructed_link_evidence(bounded_evidence())
    lines = [bridge.PriceLine(line.price_security_id, line.last_symbol, line.first_trade_date,
                              line.last_trade_date, line.bar_rows, vendor_id=line.vendor_id) for line in vendor]
    symbols = [bridge.LineSymbol(line.price_security_id, symbol, first, last)
               for line in vendor for symbol, first, last in line.symbols]
    ticker_observed = dt.datetime.fromisoformat(inputs["company_tickers"]["received_at"])
    ticker_source = Path(inputs["company_tickers"]["path"])
    if native.file_sha256(ticker_source) != inputs["company_tickers"]["sha256"]:
        raise ValueError("retained company ticker source changed")
    tickers = [(cik, ticker, ticker_observed) for cik, ticker in
               ir.read_sec_ticker_snapshot(ticker_source)]
    directory = bridge._read_directory(SimpleNamespace(con=con))
    if not directory.types:
        raise ValueError("nonempty pinned directory was not read by bridge adapter")
    current_classes = bridge._class_index(bridge.classify_sec_tickers(tickers, directory.types))
    directory_clock = con.execute("SELECT max(source_loaded_at) FROM nasdaq_symbol_directory").fetchone()[0] or created
    retrospective, _, _, bridge_stats = bridge.classify_reconstructed_with_history(
        lines, tickers, {}, directory.types, symbols, directory.adr_ratios, reconstructed=reconstructed)
    symbol_holders = defaultdict(list)
    for line in vendor:
        for symbol, first, last in line.symbols:
            if key := bridge.normalize_symbol(symbol):
                symbol_holders[key].append((line.vendor_id, first, last))
    operating = {int(row[0]) for row in con.execute("SELECT vendor_id FROM ri_operating_lines").fetchall()}
    retrospective_linked = {line_by_id[row.price_security_id].vendor_id for row in retrospective if row.linked}
    details = {"operating_proxy_lines": len(operating),
               "retrospective_operating_linked": len(operating & retrospective_linked), "bridge_stats": bridge_stats}
    native.create_identity_tables(con)
    con.execute("CREATE TABLE identity_candidates (" + native.CANDIDATE_DDL + ")")
    del reconstructed, lines, symbols
    # Every evidence event names only the inputs needed at that clock. The
    # parent RI row can include later evidence; it is retained as provenance,
    # never silently treated as all known at the earliest tier.
    for ordinal, row in enumerate(native._evidence_rows(con, evidence_files)):
        if ordinal % 100 == 0:
            native._insert_rows(con, "identity_link_evidence", evidence_rows)
            evidence_rows.clear()
            native._insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
            unresolved_rows.clear()
            native._insert_candidates(con, candidate_rows)
            candidate_rows.clear()
        payload = json.loads(row["value_json"])
        vendor_id = int(row["native_key"])
        line = line_by_id.get(f"TBLTICKERHISTORY-{vendor_id}")
        if line is None:
            raise ValueError(f"evidence names unknown vendor {vendor_id}")
        if row["evidence_status"] != "reconstructed":
            # Rejected RI rows carry no historical knowledge clock. Keep
            # uncertainty at observation only; never fabricate an early one.
            at = native._clock(row["observed_at"])
            if at <= snapshot_clock:
                event_id = native._event_id(row["evidence_id"], "unresolved", at)
                first = row["valid_from"] or line.first_trade_date
                last = row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else line.last_trade_date
                evidence_rows.append((event_id, vendor_id, row["cik"], inputs["companyfacts"]["path"],
                    inputs["companyfacts"]["sha256"], json.dumps(payload, sort_keys=True), first, last, at, at,
                    "unresolved_company_candidate", None, "observed_reconstruction_without_historical_clock"))
                unresolved_rows.append((event_id, vendor_id, int(row["cik"]), row["cik"], first, last,
                                        at, None, json.dumps([event_id]), payload["rejection_reason"]))
            continue
        history = [(tier, native._clock(dt.datetime.fromisoformat(stamp))) for tier, stamp in payload["tier_history"]]
        clocks = {at for _, at in history}
        clocks.update(dt.datetime.combine(first, dt.time(22)) for _, first, _ in line.symbols)
        previous = None
        for at in sorted(clocks):
            if at > snapshot_clock:
                continue
            known_tiers = [(tier, since) for tier, since in history if since <= at]
            if not known_tiers:
                continue
            tier = max(known_tiers, key=lambda item: item[1])[0]
            proof = native._class_proof_at(line, payload, at, symbol_holders)
            state = (tier, proof["class_status"], proof["security_class"])
            if state == previous:
                continue
            previous = state
            event_id = native._event_id(row["evidence_id"], tier, at)
            assertion = {"parent_evidence_id": row["evidence_id"], "tier": tier,
                         **proof, "is_common": proof["class_status"] == "common",
                         "method": prepared["pin"]["reconstruction_method"]}
            evidence_rows.append((event_id, vendor_id, row["cik"], inputs["companyfacts"]["path"],
                inputs["companyfacts"]["sha256"], json.dumps(assertion, sort_keys=True, default=str), row["valid_from"],
                row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else None,
                at, row["observed_at"], "reconstructed_tier_and_class", None, "modeled_reconstruction"))
            candidate_rows.append({"assertion_id": row["evidence_id"], "perm_security_id": vendor_id,
                "cik": row["cik"], "link_start": row["valid_from"],
                "link_end": row["valid_to"] - dt.timedelta(days=1) if row["valid_to"] else None,
                "link_basis": "reconstructed_" + tier, "tier": tier, "available_at": at,
                "evidence_ids": json.dumps([event_id]), "is_common": proof["class_status"] == "common",
                "class_status": proof["class_status"]})
    snapshot_events = {}
    for row in retrospective:
        if not row.linked or row.evidence_id or not row.cik:
            continue
        line = line_by_id[row.price_security_id]
        # A current snapshot cannot identify a long-ended holder of a reused ticker.
        if (snapshot - line.last_trade_date).days > bridge.STALE_LINK_DAYS:
            continue
        at = row.evidence_observed_at or ticker_observed
        if at > snapshot_clock:
            continue
        event_id = native._event_id("current_sec_ticker", line.vendor_id, row.cik, at)
        item = current_classes.get((row.cik, bridge.normalize_symbol(row.share_class_symbol)))
        status = native._snapshot_class_status(item)
        signature = (row.share_class_symbol, status, item.security_class if item else None,
                     item.basis if item else None)
        if event_id in snapshot_events:
            if snapshot_events[event_id] != signature:
                raise ValueError(f"conflicting current snapshot assertion {event_id}")
            continue  # One observed event, despite historical bridge segmentation.
        snapshot_events[event_id] = signature
        evidence_rows.append((event_id, line.vendor_id, row.cik, inputs["company_tickers"]["path"],
            inputs["company_tickers"]["sha256"], json.dumps({"ticker": row.share_class_symbol,
            "scope": "snapshot_observation_onward", "common_class": False}), at.date(), None, at,
            ticker_observed, "current_ticker_snapshot", None, "observed_snapshot"))
        candidate_rows.append({"assertion_id": event_id, "perm_security_id": line.vendor_id, "cik": row.cik,
            "link_start": at.date(), "link_end": None, "link_basis": "current_ticker_verified", "tier": None,
            "available_at": at, "evidence_ids": json.dumps([event_id]), "is_common": False,
            "class_status": "unknown"})
        if status != "unknown" and directory_clock <= snapshot_clock:
            common_at = max(at, directory_clock)
            common_id = native._event_id(event_id, "directory_class", status, common_at)
            directory_entry = next(item for item in ops["exports"] if item.get("table") == "nasdaq_symbol_directory" and item.get("file"))
            evidence_rows.append((common_id, line.vendor_id, row.cik, str(ops_manifest.parent / directory_entry["file"]),
                directory_entry["sha256"], json.dumps({"security_class": item.security_class,
                    "class_basis": item.basis, "class_status": status}),
                at.date(), None, common_at, directory_clock, "current_classification", None, "observed_snapshot"))
            promoted = {**candidate_rows[-1], "available_at": common_at,
                        "evidence_ids": json.dumps([event_id, common_id]), "is_common": status == "common", "class_status": status}
            if common_at == at:
                candidate_rows[-1] = promoted
            else:
                candidate_rows.append(promoted)
    # Current SEC tickers are possible company classes even if the price
    # universe contains no usable line for them. Count them from observation,
    # never before, so a missing sibling cannot manufacture complete ME.
    for cik, ticker, at in tickers:
        if at > snapshot_clock:
            continue
        key = bridge.normalize_symbol(ticker)
        represented = any((snapshot - last).days <= bridge.STALE_LINK_DAYS
                          for _, _, last in symbol_holders.get(key, ()))
        if represented:
            continue
        event_id = native._event_id("unrepresented_current_ticker", cik, ticker, at)
        evidence_rows.append((event_id, None, cik, inputs["company_tickers"]["path"],
            inputs["company_tickers"]["sha256"], json.dumps({"ticker": ticker, "class_status": "unknown"}),
            at.date(), None, at, at, "unrepresented_current_ticker", None, "observed_snapshot"))
        unresolved_rows.append((event_id, None, int(cik), cik, at.date(), None, at, None,
                                json.dumps([event_id]), "current_ticker_without_price_line"))
    native._insert_rows(con, "identity_link_evidence", evidence_rows)
    native._insert_rows(con, "identity_unresolved_memberships", unresolved_rows)
    native._insert_candidates(con, candidate_rows)
    evidence_rows.clear()
    unresolved_rows.clear()
    candidate_rows.clear()
    del retrospective, current_classes, snapshot_events
    con.execute("CREATE TABLE identity_document_assertions(document_id VARCHAR,perm_security_id BIGINT,available_at TIMESTAMP,payload VARCHAR)")
    details["document_reviews"] = details["document_supported_events"] = 0
    if document_review is not None:
        document_candidates = {}
        for evidence, candidate, document in native._reviewed_document_events(document_review, vendor, snapshot_clock):
            details["document_reviews"] += 1
            native._insert_rows(con, "identity_link_evidence", [evidence])
            if candidate:
                key = candidate["assertion_id"], candidate["available_at"]
                document_candidates[key] = (native._merge_candidate_evidence(document_candidates[key], candidate)
                    if key in document_candidates else candidate)
                native._insert_rows(con, "identity_document_assertions", [(document["document_id"], document["perm_security_id"],
                    document["available_at"], json.dumps(document, sort_keys=True, default=str))])
                details["document_supported_events"] += 1
        native._insert_candidates(con, list(document_candidates.values()))
        details["document_candidate_events"] = len(document_candidates)
    return {'products':{name:name for name in ('identity_candidates','identity_link_evidence','identity_unresolved_memberships','identity_document_assertions')},'details':details,'source_quality_certified':False}


def _native_r4_uncertainty(con,*,candidates,assertions,inputs,namespace):
    """Original unresolved denominator replay with bounded full line slices."""
    from collections import defaultdict
    from .. import identity_links as native,identity_reconstruction as ir
    if not re.fullmatch(r't17_market_[a-z0-9_]+',namespace):raise ValueError('fixed native uncertainty namespace required')
    outputs={name:namespace+'_'+name for name in ('identity_link_evidence','identity_unresolved_memberships')}
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if existing&set(outputs.values()):raise ValueError('native uncertainty namespace occupied')
    for name,table in outputs.items():con.execute('CREATE TABLE '+table+'('+native.TABLE_COLUMNS[name]+')')
    params=ir.ReconstructionParams();snapshot_clock=dt.datetime(2026,9,20,22);last=None;count=0
    while lines:=_r4_vendor_lines(con,last,64):
        ids=[line.vendor_id for line in lines];line_by_id={line.vendor_id:line for line in lines}
        first={(vid,cik):at for vid,cik,at in con.execute(f"SELECT perm_security_id,cik,min(available_at) FROM {candidates} WHERE link_basis<>'reconstructed_document_symbol_continuity' AND perm_security_id IN(SELECT unnest(?)) GROUP BY 1,2",[ids]).fetchall()}
        documents=defaultdict(list)
        cursor=con.execute(f'SELECT payload FROM {assertions} WHERE perm_security_id IN(SELECT unnest(?))',[ids]);n=0
        while batch:=cursor.fetchmany(64):
            for (payload,) in batch:
                document=json.loads(payload)
                for key in ('link_start','link_end'):document[key]=dt.date.fromisoformat(document[key])
                for key in ('available_at','observed_at'):document[key]=dt.datetime.fromisoformat(document[key])
                documents[document['perm_security_id']].append(document);n+=1
                if n>4096:raise ValueError('complete native uncertainty documentary slice exceeds bounded replay')
        matches=ir.match_share_counts(con,params,vendor_ids=ids)
        share_items,_=ir._share_items(matches)
        for (vid,cik),items in share_items.items():
            item=min(items,key=lambda value:(value.known_at,str(value.detail)))
            at,until=item.known_at,first.get((vid,cik))
            if at>snapshot_clock or(until is not None and at>=until):continue
            line=line_by_id[vid];event=native._event_id('possible_common_member',vid,cik,at)
            evidence=[(event,vid,cik,inputs['companyfacts']['path'],inputs['companyfacts']['sha256'],
                json.dumps({'kind':item.kind,'known_at':at,**item.detail,'interpretation':'possible common member only; identity not yet supported'},sort_keys=True,default=str),
                line.first_trade_date,line.last_trade_date,at,dt.datetime.fromisoformat(inputs['companyfacts']['received_at']),
                'unresolved_share_candidate',None,'modeled_filed_plus_46h_and_vendor_close')]
            unresolved,resolutions=native._resolve_raw_membership((event,vid,int(cik),cik,line.first_trade_date,line.last_trade_date,
                at,until,json.dumps([event]),'share_candidate_before_supported_identity'),documents.get(vid,[]))
            native._insert_rows(con,outputs['identity_link_evidence'],evidence+resolutions)
            native._insert_rows(con,outputs['identity_unresolved_memberships'],unresolved)
        count+=len(lines);last=lines[-1].vendor_id
    if count!=con.execute('SELECT count(*) FROM ri_vendor_lines').fetchone()[0]:raise ValueError('full native uncertainty line census changed')
    return {'products':outputs,'vendor_lines':count,'source_quality_certified':False}
