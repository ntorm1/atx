"""Native Task6 orchestration; data execution always requires external memory guard."""
from __future__ import annotations
import datetime as dt
import json
import math
from pathlib import Path
import pyarrow as pa

from . import common as C
from . import factors as F
from . import _factor_sources as S
from ._factor_io import Generation, publisher_lock

FACTOR_SCHEMA = pa.schema([('freq',pa.string()),('session_date',pa.date32()),
    ('available_at',pa.timestamp('us',tz='UTC')),('n_universe',pa.int64()),
    ('n_qualified',pa.int64()),('missing_reason',pa.string()),
    *[(k,pa.float64()) for k in F.FACTORS]])

EMPTY = {
 'cap_quality': 'security_id BIGINT,session_date DATE,available_at TIMESTAMP,source_available_at TIMESTAMP,certified BOOLEAN,membership_certified BOOLEAN,reason VARCHAR,audit_status VARCHAR,evidence_sha256 VARCHAR,shares_source_sha256 VARCHAR,membership_source_sha256 VARCHAR',
 'eg_training': 'cik BIGINT,fit_month DATE,fit_cutoff TIMESTAMPTZ,outcome_period_end DATE,outcome_available_at TIMESTAMPTZ,predictor_available_at TIMESTAMPTZ,dia DOUBLE,log_q DOUBLE,cop DOUBLE,droe DOUBLE,me DOUBLE',
}


def code_identity():
    return {k:v['sha256_lf'] for k,v in C.code_identity('factors','risk_inputs','_factor_io','_factor_sources','_factor_native').items() if k!='git_head'}


def rows(con, sql):
    cur = con.execute(sql)
    names = [x[0] for x in cur.description]
    # Bounded fetch; callers limit cross-sections to max 20k, never whole history.
    while batch := cur.fetchmany(1024):
        for row in batch:
            yield dict(zip(names,row))


def setup(con, spec: dict) -> dict:
    if spec.get('config_sha256')!=F.config_hash():
        raise ValueError('native source adapter does not bind frozen configuration')
    F.require_release(spec['release'])
    identities = {}
    required = {'prices','universe','funda','fundq','ratios_state','calendar','cap_quality',
                'liquidity','borrow','industry','rf','eg_training'}
    if set(spec['sources']) != required:
        raise ValueError('explicit complete native source map required')
    final = {'funda':'compustat_std_v11','fundq':'compustat_std_v11',
             'ratios_state':'ratios_v11','universe':'universe'}
    cap_audit=None
    for name,item in spec['sources'].items():
        if item is None:
            if name not in EMPTY:
                raise ValueError('mandatory native source unavailable: '+name)
            fields = [x.strip().split(' ',1) for x in EMPTY[name].split(',')]
            sql = ','.join(f'CAST(NULL AS {kind}) AS {field}' for field,kind in fields)
            con.execute(f'CREATE TEMP VIEW t6_{name} AS SELECT {sql} WHERE false')
            identities[name] = 'explicitly_unavailable'
            continue
        if name=='prices' and 'parts' in item:
            parts=item['parts']
            if {p['stage'] for p in parts}!={'prices_history','prices'}:
                raise ValueError('explicit prices_history/prices parts required')
            paths=[]
            hashes={}
            for part in parts:
                manifest=Path(part['manifest'])
                m=F.bind_snapshot(manifest,part['sha256'],part['stage'])
                if part['stage']=='prices_history' and part['sha256']!=spec['release']['prices_history']['sha256']:
                    raise ValueError('historical prices not bound to final release')
                if not part['files'] or any(p not in m['files'] for p in part['files']):
                    raise ValueError('price outputs not in source manifest')
                paths.extend(manifest.parent/p for p in part['files'])
                hashes[part['stage']]=part['sha256']
            con.execute(f'CREATE TEMP VIEW t6_prices AS SELECT * FROM {S.parquet_relation(paths)}')
            identities[name]=hashes
            continue
        stage = item['stage']
        if name in final and stage != final[name]:
            raise ValueError('old producer source forbidden: '+name)
        if name in final and item['sha256'] != spec['release'][stage]['sha256']:
            raise ValueError('native source does not bind final release: '+name)
        manifest = Path(item['manifest'])
        m = F.bind_snapshot(manifest,item['sha256'],stage)
        selected = item['files']
        if not selected or any(p not in m['files'] for p in selected):
            raise ValueError('source files not in exact manifest')
        paths = [manifest.parent/p for p in selected]
        con.execute(f'CREATE TEMP VIEW t6_{name} AS SELECT * FROM {S.parquet_relation(paths)}')
        if name=='rf' and item.get('adapter')=='french_rf':
            if set(item['frequency_files'])!={'D','M'} or any(v not in selected for v in item['frequency_files'].values()):
                raise ValueError('explicit French daily/monthly RF files required')
            daily=S.parquet_relation([manifest.parent/item['frequency_files']['D']])
            monthly=S.parquet_relation([manifest.parent/item['frequency_files']['M']])
            con.execute(f"CREATE OR REPLACE TEMP VIEW t6_rf AS SELECT 'D' AS freq,date AS session_date,rf FROM {daily} UNION ALL SELECT 'M',month,rf FROM {monthly}")
        if name=='funda':
            columns={r[0] for r in con.execute('DESCRIBE t6_funda').fetchall()}
            absent=[k for k in ('revt','drc','drlt') if k not in columns]
            if absent:
                con.execute(f"CREATE OR REPLACE TEMP VIEW t6_funda AS SELECT *,"+
                    ','.join(f'CAST(NULL AS DOUBLE) AS {k}' for k in absent)+f' FROM {S.parquet_relation(paths)}')
        identities[name] = item['sha256']
        if name == 'cap_quality':
            cap_audit=m
            # This source must be an independently reviewed audit, bound to the
            # actual share/membership output hashes; Boolean assertions alone fail.
            if stage != 'cap_basis_audit' or m.get('audit_status') != 'independently_reviewed':
                raise ValueError('cap quality requires independently reviewed audit manifest')
            for evidence in m.get('evidence_files',[]):
                p = manifest.parent/evidence['path']
                if C.sha256_file(p) != evidence['sha256']:
                    raise ValueError('stale primary-source audit evidence')
            if not m.get('evidence_files') or not m.get('input_manifests_sha256'):
                raise ValueError('audit lineage/evidence required')
            shares=spec.get('audit_share_source')
            if shares is None:
                raise ValueError('exact audited share producer binding required')
            F.bind_snapshot(Path(shares['manifest']),shares['sha256'],'market_shares')
            audit_inputs=m['input_manifests_sha256']
            expected={'market_shares':shares['sha256'],'universe':spec['release']['universe']['sha256'],
                      'prices_history':spec['release']['prices_history']['sha256']}
            if any(audit_inputs.get(k)!=v for k,v in expected.items()):
                raise ValueError('cap audit bound to stale source producers')
            evidence=','.join("'"+v['sha256']+"'" for v in m['evidence_files'])
            con.execute(f"CREATE OR REPLACE TEMP VIEW t6_cap_quality AS SELECT * FROM {S.parquet_relation(paths)} WHERE shares_source_sha256='{shares['sha256']}' AND membership_source_sha256='{expected['universe']}' AND evidence_sha256 IN({evidence})")
    if cap_audit is not None and 'CIK77159/share-basis' not in cap_audit.get('resolved_issues',[]):
        con.execute('CREATE TEMP TABLE t6_qualified_cap_rows AS SELECT q.* FROM t6_cap_quality q WHERE q.security_id NOT IN(SELECT DISTINCT security_id FROM t6_universe WHERE cik=77159)')
        con.execute('CREATE OR REPLACE TEMP VIEW t6_cap_quality AS SELECT * FROM t6_qualified_cap_rows')
    # The native history source extends the dated session grid; no current
    # membership or return values are inferred from this date-only union.
    con.execute('CREATE TEMP TABLE t6_calendar_dates AS SELECT DISTINCT session_date FROM (SELECT session_date FROM t6_calendar UNION ALL SELECT session_date FROM t6_prices)')
    con.execute('CREATE OR REPLACE TEMP VIEW t6_calendar AS SELECT * FROM t6_calendar_dates')
    return identities


def eg_predictors_sql(formation,cutoff):
    tau=cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    quarter=S.quarterly_sql(cutoff)
    return f"""WITH visible AS (SELECT * FROM t6_funda WHERE available_at<TIMESTAMP '{tau}'
        AND datadate<=CAST(DATE '{formation}'-INTERVAL 4 MONTH AS DATE)
        QUALIFY row_number() OVER(PARTITION BY cik,datadate ORDER BY available_at DESC,accession DESC)=1),
      lagged AS (SELECT *,lag(rect) OVER w AS rect1,lag(invt) OVER w AS invt1,
        lag(xpp) OVER w AS xpp1,lag(drc) OVER w AS drc1,lag(drlt) OVER w AS drlt1,
        lag(ap) OVER w AS ap1,lag(xacc) OVER w AS xacc1 FROM visible WINDOW w AS(PARTITION BY cik ORDER BY datadate)),
      a AS (SELECT * FROM lagged QUALIFY row_number() OVER(PARTITION BY cik ORDER BY datadate DESC)=1),
      q AS ({quarter}),
      fiscal_me AS (SELECT a.cik,sum(u.me) AS me FROM a JOIN t6_universe u ON a.cik=u.cik
        AND u.session_date=(SELECT max(c.session_date) FROM t6_calendar c WHERE c.session_date<=a.datadate)
        JOIN t6_cap_quality cq ON cq.security_id=u.security_id AND cq.session_date=u.session_date
        AND cq.available_at<TIMESTAMP '{tau}' AND cq.source_available_at<TIMESTAMP '{tau}'
        AND cq.certified AND cq.membership_certified AND cq.audit_status='independently_verified'
        AND cq.evidence_sha256 IS NOT NULL AND cq.shares_source_sha256 IS NOT NULL
        AND cq.membership_source_sha256 IS NOT NULL GROUP BY a.cik)
      SELECT a.cik,greatest(a.available_at,q.available_at) AT TIME ZONE 'UTC' AS available_at,
        CASE WHEN a.at>0 AND (fm.me+a.dltt+a.dlc)>0 THEN ln((fm.me+a.dltt+a.dlc)/a.at) END AS log_q,
        CASE WHEN a.at>0 THEN (a.revt-a.cogs-a.xsga+coalesce(a.xrd,0)
          -coalesce(a.rect-a.rect1,0)-coalesce(a.invt-a.invt1,0)-coalesce(a.xpp-a.xpp1,0)
          +coalesce(a.drc-a.drc1,0)+coalesce(a.drlt-a.drlt1,0)
          +coalesce(a.ap-a.ap1,0)+coalesce(a.xacc-a.xacc1,0))/a.at END AS cop,
        q.droe FROM a LEFT JOIN q USING(cik) LEFT JOIN fiscal_me fm USING(cik)
      WHERE NOT coalesce(a.sic BETWEEN 6000 AND 6999,false)
        AND coalesce(a.seq,a.ceq+coalesce(a.pstk,0),a.at-a.lt)+coalesce(a.txditc,0)-coalesce(a.pstk,0)>0"""


def native_training(con,formation,dates):
    """Yield bounded historical monthly WLS samples with separately visible labels."""
    current=formation.year*12+formation.month-1
    for ordinal in range(current-120,current):
        first=dt.date(ordinal//12,ordinal%12+1,1)
        fitday=max((d for d in dates if d<first),default=None)
        predictorfirst=dt.date(first.year-1,first.month,1)
        predictorday=max((d for d in dates if d<predictorfirst),default=None)
        if fitday is None or predictorday is None:
            continue
        tau=_cutoff(fitday).replace(tzinfo=None).isoformat()
        sql=f"""WITH v AS (SELECT * FROM t6_funda WHERE available_at<TIMESTAMP '{tau}'
          AND datadate<=CAST(DATE '{fitday}'-INTERVAL 4 MONTH AS DATE)
          QUALIFY row_number() OVER(PARTITION BY cik,datadate ORDER BY available_at DESC,accession DESC)=1),
        lagged AS (SELECT *,lag("at") OVER w AS at1,lag("at",2) OVER w AS at2,
          lag(datadate) OVER w AS date1,lag(datadate,2) OVER w AS date2 FROM v WINDOW w AS(PARTITION BY cik ORDER BY datadate)),
        a AS (SELECT * FROM lagged QUALIFY row_number() OVER(PARTITION BY cik ORDER BY datadate DESC)=1),
        x AS ({eg_predictors_sql(predictorday,_cutoff(predictorday))}),
        me AS (SELECT u.cik,sum(u.me) AS me FROM t6_universe u JOIN t6_cap_quality cq
          USING(security_id,session_date) WHERE u.session_date=DATE '{fitday}' AND u.univ_all_common
          AND cq.available_at<TIMESTAMP '{tau}' AND cq.source_available_at<TIMESTAMP '{tau}'
          AND cq.certified AND cq.membership_certified AND cq.audit_status='independently_verified'
          GROUP BY u.cik)
        SELECT a.cik,a.datadate AS outcome_period_end,a.available_at AT TIME ZONE 'UTC' AS outcome_available_at,
          x.available_at AS predictor_available_at,x.log_q,x.cop,x.droe,me.me,
          CASE WHEN at1>0 AND at2>0 AND date_diff('day',date1,datadate) BETWEEN 300 AND 430
            AND date_diff('day',date2,date1) BETWEEN 300 AND 430 THEN a.at/at1-at1/at2 END AS dia
        FROM a JOIN x USING(cik) LEFT JOIN me USING(cik) ORDER BY cik"""
        for r in rows(con,sql):
            if r['predictor_available_at'] is None:
                continue
            yield {**r,'fit_month':first,'fit_cutoff':_cutoff(fitday)}


def sessions(con) -> list[dt.date]:
    return [r[0] for r in con.execute("SELECT DISTINCT session_date FROM (SELECT session_date FROM t6_calendar UNION ALL SELECT session_date FROM t6_prices) WHERE session_date<DATE '2023-01-01' ORDER BY session_date").fetchall()]


def _cutoff(day):
    return dt.datetime.combine(day,dt.time(22),dt.UTC)


def issuer_period(con, first, last, formation):
    if formation is None:
        return []
    data = list(rows(con,S.issuer_period_sql(first,last,formation,_cutoff(formation))))
    if len(data)>20000:
        raise ValueError('issuer cross-section limit exceeded')
    return data


def formation_issuers(con,formation):
    tau=_cutoff(formation).replace(tzinfo=None).isoformat()
    sql=f"""WITH q AS(SELECT * FROM t6_cap_quality WHERE available_at<TIMESTAMP '{tau}'
      AND source_available_at<TIMESTAMP '{tau}' AND audit_status='independently_verified'
      AND evidence_sha256 IS NOT NULL AND shares_source_sha256 IS NOT NULL AND membership_source_sha256 IS NOT NULL
      QUALIFY row_number() OVER(PARTITION BY security_id,session_date ORDER BY available_at DESC)=1)
      SELECT u.cik,sum(u.me) AS me,CAST(NULL AS DOUBLE) AS ret,bool_and(u.exchcd_like=1) AS nyse,
      bool_and(coalesce(q.certified AND q.membership_certified,false)) AS certified
      FROM t6_universe u LEFT JOIN q USING(security_id,session_date)
      WHERE u.session_date=DATE '{formation}' AND u.univ_all_common AND u.cik IS NOT NULL AND u.me>0 GROUP BY u.cik"""
    data=list(rows(con,sql))
    if len(data)>20000:
        raise ValueError('formation cross-section limit exceeded')
    return data


def signal_history(con, formation: dt.date, months: int, skip: int) -> dict:
    # Calendar month offsets, not observation offsets; all required months must exist.
    endordinal = formation.year*12+formation.month-1-skip
    startordinal = endordinal-months+1
    def date(o):
        return dt.date(o//12,o%12+1,1)
    start, nextmonth = date(startordinal), date(endordinal+1)
    sql = f"""WITH m AS (SELECT security_id,date_trunc('month',session_date) AS month,
      CASE WHEN bool_or(ret_guarded OR ret IS NULL OR ret< -1) THEN NULL
        WHEN bool_or(ret=-1) THEN -1 ELSE exp(sum(ln(1+ret)))-1 END AS ret
      FROM t6_prices WHERE session_date>=DATE '{start}' AND session_date<DATE '{nextmonth}'
        AND session_date<DATE '2023-01-01' GROUP BY 1,2),
    u AS (SELECT * FROM t6_universe WHERE session_date=DATE '{formation}' AND univ_all_common),
    r AS (SELECT security_id,CASE WHEN count(ret)={months} THEN
      CASE WHEN bool_or(ret=-1) THEN -1 ELSE exp(sum(ln(1+ret)))-1 END END AS value FROM m GROUP BY 1)
    SELECT cik,CASE WHEN count(value)=count(*) THEN sum(me*value)/sum(me) END AS value
    FROM u LEFT JOIN r USING(security_id) WHERE cik IS NOT NULL AND me>0 GROUP BY cik"""
    return {r['cik']:r['value'] for r in rows(con,sql)}


def labels(data, signals: dict, name: str):
    good = [r for r in data if r['certified'] and math.isfinite(r['me']) and r['me']>0 and
        signals.get(r['cik']) is not None and math.isfinite(signals[r['cik']])]
    reference = [r for r in good if r['nyse']]
    if not reference:
        return {}
    import numpy as np
    size = float(np.quantile([r['me'] for r in reference],.5))
    lo,hi = np.quantile([signals[r['cik']] for r in reference],[.3,.7])
    return {r['cik']:(int(r['me']>size),0 if signals[r['cik']]<=lo else 2 if signals[r['cik']]>hi else 1) for r in good}


def held_factor(data, held, sign=1):
    cells = {(s,v):[] for s in range(2) for v in range(3)}
    now = {r['cik']:r for r in data if r['certified']}
    for cik,tag in held.items():
        r = now.get(cik)
        if r is None:
            # Missing a previously formed member is visible missing portfolio input.
            cells[tag].append(F.Stock(cik,1,None,False,{},dt.datetime(2000,1,1,tzinfo=dt.UTC),True))
        else:
            cells[tag].append(F.Stock(cik,r['me'],r['ret'],r['nyse'],{},dt.datetime(2000,1,1,tzinfo=dt.UTC),True))
    p = {k:F.vw(v) for k,v in cells.items()}
    ls = F.spread([p[0,2],p[1,2]],[p[0,0],p[1,0]])
    size = F.spread([p[0,j] for j in range(3)],[p[1,j] for j in range(3)])
    return None if ls is None else sign*ls,size


def held_q(data,ia_labels,roe_labels):
    cells={(s,i,p):[] for s in range(2) for i in range(3) for p in range(3)}
    now={r['cik']:r for r in data if r['certified']}
    for cik,(size,ia) in ia_labels.items():
        if cik not in roe_labels:
            continue
        roe=roe_labels[cik][1]
        r=now.get(cik)
        cells[size,ia,roe].append(F.Stock(cik,r['me'] if r else 1,r['ret'] if r else None,
            False,{},dt.datetime(2000,1,1,tzinfo=dt.UTC),True))
    p={k:F.vw(v) for k,v in cells.items()}
    return {'q_ME':F.spread([p[0,i,j] for i in range(3) for j in range(3)],[p[1,i,j] for i in range(3) for j in range(3)]),
        'q_IA':F.spread([p[s,0,j] for s in range(2) for j in range(3)],[p[s,2,j] for s in range(2) for j in range(3)]),
        'q_ROE':F.spread([p[s,i,2] for s in range(2) for i in range(3)],[p[s,i,0] for s in range(2) for i in range(3)])}


def native_betas(con,formation,dates):
    keep=[d for d in dates if d<=formation][-1260:]
    if len(keep)<752:
        return {}
    # Own market history from dated lag-cap certified holdings; never borrow
    # benchmark returns as own-factor histories or use a current membership set.
    sql=f"""WITH cal AS (SELECT session_date,lag(session_date) OVER(ORDER BY session_date) AS prev FROM t6_calendar),
      q AS (SELECT * FROM t6_cap_quality WHERE audit_status='independently_verified'
        AND evidence_sha256 IS NOT NULL AND shares_source_sha256 IS NOT NULL AND membership_source_sha256 IS NOT NULL),
      line AS (SELECT p.session_date,u.cik,u.me,p.ret,p.ret_guarded,
        coalesce(q.certified AND q.membership_certified,false) AS certified
        FROM cal c JOIN t6_prices p USING(session_date)
        JOIN t6_universe u ON u.session_date=c.prev AND u.security_id=p.security_id
        LEFT JOIN q ON q.session_date=u.session_date AND q.security_id=u.security_id
          AND q.available_at<CAST(c.prev AS TIMESTAMP)+INTERVAL 22 HOUR
          AND q.source_available_at<CAST(c.prev AS TIMESTAMP)+INTERVAL 22 HOUR
        WHERE p.session_date BETWEEN DATE '{keep[0]}' AND DATE '{formation}'
          AND p.session_date<DATE '2023-01-01' AND u.univ_all_common AND u.me>0 AND u.cik IS NOT NULL),
      issuer AS (SELECT session_date,cik,sum(me) AS me,CASE WHEN bool_and(certified)
        AND count(ret)=count(*) AND NOT bool_or(ret_guarded) THEN sum(me*ret)/sum(me) END AS ret
        FROM line GROUP BY 1,2),
      market AS (SELECT session_date,CASE WHEN count(ret)=count(*) THEN sum(me*ret)/sum(me) END AS mkt
        FROM issuer GROUP BY 1)
      SELECT i.*,m.mkt FROM issuer i JOIN market m USING(session_date) ORDER BY cik,session_date"""
    output,active,series,market={},None,{},{}
    def flush():
        if active is not None:
            output[active]=F.bab_beta([series.get(d) for d in keep],[market.get(d) for d in keep])
    for r in rows(con,sql):
        if r['cik']!=active:
            flush()
            active,series,market=r['cik'],{},{}
        series[r['session_date']]=r['ret']
        market[r['session_date']]=r['mkt']
    flush()
    return output


def native_factors(spec: dict,destination: Path):
    from . import _factor_sources as sources
    con = C.connect(memory='350MB',threads=1,db_file='task6-factors.duckdb')
    try:
        identity = setup(con,spec)
        dates = sessions(con)
        ends = {}
        for d in dates:
            ends[d.year,d.month] = d
        identity.update(config_sha256=F.config_hash(),kind='factors',
            code=code_identity())
        with publisher_lock(destination):
            generation = Generation(destination,'factors',identity,FACTOR_SCHEMA)
            required = []
            for year in range(2013,2023):
                for month in range(1,13):
                    unit = f'month={year:04d}-{month:02d}'
                    required.append(unit)
                    if generation.done(unit):
                        continue
                    first = dt.date(year,month,1)
                    last = ends.get((year,month))
                    prior = max((d for d in dates if d<first),default=None)
                    june_year = year if month>=7 else year-1
                    june = ends.get((june_year,6))
                    annual_labels,q_ia_labels = {},{}
                    if june:
                        a = {r['cik']:r for r in rows(con,sources.annual_sql(june,_cutoff(june)))}
                        jd = formation_issuers(con,june)
                        dec = ends.get((june_year-1,12))
                        dd = {r['cik']:r['me'] for r in formation_issuers(con,dec) if r['certified']} if dec else {}
                        signals = {
                            'bm':{k:(r['be']/dd[k] if r['be'] and r['be']>0 and dd.get(k,0)>0 else None) for k,r in a.items()},
                            'op':{k:r['op'] for k,r in a.items()},'investment':{k:r['investment'] for k,r in a.items()}}
                        annual_labels = {k:labels(jd,v,k) for k,v in signals.items()}
                        q_ia_labels=labels(jd,{k:r['investment'] if r['be'] is not None and r['be']>0 and
                            not (r['sic'] is not None and 6000<=r['sic']<=6999) else None for k,r in a.items()},'investment')
                    monthly_labels = {}
                    roe_labels,eg_labels,beta_values={},{},{}
                    if prior:
                        pd = formation_issuers(con,prior)
                        for k,n,skip in [('momentum',11,1),('strev',1,0),('ltrev',48,12)]:
                            monthly_labels[k] = labels(pd,signal_history(con,prior,n,skip),k)
                        quarter={r['cik']:r for r in rows(con,sources.quarterly_sql(_cutoff(prior)))}
                        roe_labels=labels(pd,{k:r['roe'] for k,r in quarter.items()},'roe')
                        training=(native_training(con,prior,dates) if spec['sources']['eg_training'] is None else
                            rows(con,f"SELECT * FROM t6_eg_training WHERE fit_cutoff<TIMESTAMPTZ '{_cutoff(prior).isoformat()}' ORDER BY fit_month,cik"))
                        # The native predictor adapter leaves unsupported primitive
                        # fields NULL; EG only fits visible complete training outcomes.
                        predictors=list(rows(con,eg_predictors_sql(prior,_cutoff(prior))))
                        eg=F.eg_forecast(training,predictors,_cutoff(prior))
                        eg_labels=labels(pd,eg,'eg')
                        beta_values=native_betas(con,prior,dates)
                    results = []
                    periods = [('M',first,last)] + [('D',d,d) for d in dates if d.year==year and d.month==month]
                    for freq,start,end in periods:
                        stamp = end or dt.date(year,month,28)
                        lagday = prior if freq=='M' else max((d for d in dates if d<start),default=None)
                        data = issuer_period(con,start,end,lagday) if end else []
                        match=(f"date_trunc('month',session_date)=date_trunc('month',DATE '{stamp}')" if freq=='M' else f"session_date=DATE '{stamp}'")
                        rf_rows = list(rows(con,f"SELECT rf FROM t6_rf WHERE freq='{freq}' AND {match}"))
                        rf = rf_rows[0]['rf'] if len(rf_rows)==1 else None
                        eligible = [F.Stock(r['cik'],r['me'],r['ret'],r['nyse'],{},_cutoff(lagday),True) for r in data if r['certified']]
                        values = dict.fromkeys(F.FACTORS)
                        market = F.vw(eligible)
                        values['MktRF'] = market-rf if market is not None and rf is not None else None
                        smb = []
                        for name,signal,sign in [('HML','bm',1),('RMW','op',1),('CMA','investment',-1),
                            ('UMD','momentum',1),('STREV','strev',-1),('LTREV','ltrev',-1)]:
                            held = (annual_labels if signal in annual_labels else monthly_labels).get(signal,{})
                            values[name],size = held_factor(data,held,sign)
                            if name in ('HML','RMW','CMA'):
                                smb.append(size)
                        values['SMB'] = F.mean(smb)
                        values.update(held_q(data,q_ia_labels,roe_labels))
                        values['q_EG'],_=held_factor(data,eg_labels)
                        bab_stocks=[F.Stock(r['cik'],r['me'],r['ret'],r['nyse'],{'beta':beta_values.get(r['cik'])},
                            _cutoff(lagday),True) for r in data if r['certified']]
                        values['BAB']=F.bab(bab_stocks,rf)
                        results.append({'freq':freq,'session_date':stamp,'available_at':_cutoff(stamp),
                            'n_universe':len(data),'n_qualified':len(eligible),
                            'missing_reason':None if all(v is not None for v in values.values()) else
                                'missing_qualified_history_or_required_portfolio_leg',**values})
                    generation.write(unit,results)
            setup_check(spec)
            return generation.publish(required,{'schema_id':'atx.alpha-panel.factors/v1',
                'config':F.CONFIG,'config_sha256':F.config_hash(),
                'input_manifests_sha256':{k:v['sha256'] for k,v in spec['release'].items()},
                'gate_status':{'real_benchmark_gates':'FAIL_unmeasured','q_BAB_source_history':'FAIL_unmeasured'}})
    finally:
        con.close()


def setup_check(spec):
    F.require_release(spec['release'])
    for item in spec['sources'].values():
        if item is not None:
            for part in item.get('parts',[item]):
                F.bind_snapshot(Path(part['manifest']),part['sha256'],part['stage'])
    if spec.get('audit_share_source'):
        item=spec['audit_share_source']
        F.bind_snapshot(Path(item['manifest']),item['sha256'],'market_shares')


def validate_benchmarks(spec,destination):
    """Read only explicit frozen source mappings; no data-dependent column matching."""
    import re
    con=C.connect(memory='250MB',threads=1,db_file='task6-validation.duckdb')
    try:
        own=F.bind_snapshot(Path(spec['own_factors']['manifest']),spec['own_factors']['sha256'],'factors')
        if own.get('config_sha256')!=F.config_hash():
            raise ValueError('own factor snapshot has stale formula configuration')
        paths=[Path(spec['own_factors']['manifest']).parent/p for p in own['files'] if p.endswith('.parquet')]
        data=list(rows(con,f"SELECT * FROM {S.parquet_relation(paths)} WHERE freq='M' AND session_date>=DATE '2013-01-01' AND session_date<DATE '2023-01-01' ORDER BY session_date"))
        merged={}
        for item in spec['benchmarks']:
            manifest=Path(item['manifest'])
            m=F.bind_snapshot(manifest,item['sha256'],item['stage'])
            if item['file'] not in m['files'] or item['units'] not in ('decimal','percent'):
                raise ValueError('benchmark output/units not explicitly bound')
            mapping=item['columns']
            if any(not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*',v) for v in [item['date_column'],*mapping.values()]):
                raise ValueError('benchmark mapping requires column identifiers')
            if any(k not in F.FACTORS for k in mapping):
                raise ValueError('unknown canonical factor mapping')
            factor=1 if item['units']=='decimal' else .01
            sel=','.join(f'{v}*{factor} AS {k}' for k,v in mapping.items())
            dc=item['date_column']
            sql=f"SELECT CAST({dc} AS DATE) AS session_date,{sel} FROM {S.parquet_relation([manifest.parent/item['file']])} WHERE CAST({dc} AS DATE)>=DATE '2013-01-01' AND CAST({dc} AS DATE)<DATE '2023-01-01'"
            seen=set()
            for r in rows(con,sql):
                key=r['session_date'].year,r['session_date'].month
                if key in seen:
                    raise ValueError('duplicate benchmark source month')
                seen.add(key)
                target=merged.setdefault(key,{'session_date':r['session_date']})
                if any(k in target for k in mapping):
                    raise ValueError('overlapping benchmark source definitions')
                target.update({k:r[k] for k in mapping})
        gates=F.benchmark_gates(data,list(merged.values()))
        artifact={'window':'2013-01..2022-12','required_months':120,'gates':gates,
            'own_manifest_sha256':spec['own_factors']['sha256'],'benchmark_bindings':spec['benchmarks'],
            'config_sha256':F.config_hash()}
        C.write_json_atomic(destination/'validation.json',artifact)
        return artifact
    finally:
        con.close()


def native_risk(spec,destination,start,end):
    from . import risk_inputs as R
    if start>end:
        raise ValueError('risk window start exceeds end')
    schema=pa.schema([('session_date',pa.date32()),('security_id',pa.int64()),('cik',pa.int64()),
        ('available_at',pa.timestamp('us',tz='UTC')),('univ_all_common',pa.bool_()),('univ_r3000p',pa.bool_()),
        ('beta_252',pa.float64()),*[(f'beta_ff5_{k}',pa.float64()) for k in R.FF5],
        ('ivol_capm_252',pa.float64()),('ivol_ff5_252',pa.float64()),('ff49',pa.int64()),
        *[(k,pa.float64()) for k in R.STYLES],
        *[(k,pa.float64()) for k in ('adv_21','adv_63','spread_cs_21','si_to_io','si_shares','ftd_quantity')],
        ('on_threshold_list',pa.bool_()),('cap_qualified',pa.bool_()),('risk_missing_reason',pa.string())])
    con=C.connect(memory='350MB',threads=1,db_file='task6-risk.duckdb')
    try:
        identity=setup(con,spec)
        f=spec['own_factors']
        manifest=Path(f['manifest'])
        own=F.bind_snapshot(manifest,f['sha256'],'factors')
        if own.get('config_sha256')!=F.config_hash():
            raise ValueError('risk factor snapshot has stale formula configuration')
        con.execute(f"CREATE TEMP VIEW t6_own_factors AS SELECT * FROM {S.parquet_relation([manifest.parent/p for p in own['files'] if p.endswith('.parquet')])} WHERE freq='D' AND session_date<DATE '2023-01-01'")
        all_dates=[r[0] for r in con.execute('SELECT session_date FROM t6_calendar ORDER BY session_date').fetchall()]
        dates=[d for d in all_dates if start<=d<=end]
        identity.update(own_factors=f['sha256'],config_sha256=F.config_hash(),kind='risk_inputs',start=str(start),end=str(end),
            code=code_identity())
        with publisher_lock(destination):
            generation=Generation(destination,'risk_inputs',identity,schema)
            required=[]
            counts={}
            for day in dates:
                unit='session='+str(day)
                required.append(unit)
                if generation.done(unit):
                    continue
                previous=max((d for d in all_dates if d<day),default=None)
                if previous is None:
                    continue
                cutoff=_cutoff(previous)
                primitive=list(rows(con,S.risk_primitives_sql(day,cutoff)))
                if len(primitive)>20000:
                    raise ValueError('bounded risk cross-section exceeded')
                caprows=list(rows(con,f"SELECT security_id,bool_and(certified AND membership_certified) AS qualified FROM t6_cap_quality WHERE session_date=DATE '{previous}' AND available_at<TIMESTAMP '{cutoff.replace(tzinfo=None).isoformat()}' AND source_available_at<TIMESTAMP '{cutoff.replace(tzinfo=None).isoformat()}' AND audit_status='independently_verified' AND evidence_sha256 IS NOT NULL AND shares_source_sha256 IS NOT NULL AND membership_source_sha256 IS NOT NULL GROUP BY security_id"))
                caps={r['security_id']:r['qualified'] for r in caprows}
                exposures={}
                if day<=F.END:
                    keep=[d for d in all_dates if d<=day][-252:]
                    if keep:
                        sql=f"""SELECT p.security_id,p.session_date,CASE WHEN p.ret_guarded THEN NULL ELSE p.ret END AS ret,rf.rf AS RF,
                          f.MktRF,f.SMB,f.HML,f.RMW,f.CMA FROM t6_prices p
                          LEFT JOIN t6_own_factors f USING(session_date)
                          LEFT JOIN t6_rf rf ON rf.session_date=p.session_date AND rf.freq='D'
                          WHERE p.session_date BETWEEN DATE '{keep[0]}' AND DATE '{day}'
                            AND p.session_date<DATE '2023-01-01'
                            AND p.security_id IN(SELECT security_id FROM t6_universe WHERE session_date=DATE '{day}')
                          ORDER BY p.security_id,p.session_date"""
                        active,hist=None,[]
                        def flush():
                            if active is not None:
                                clean=[{k:(v if v is not None else float('nan')) for k,v in r.items()} for r in hist]
                                ff=R.exposure(day,clean)
                                capm=R.regress([r['ret'] for r in clean],[[r['MktRF']] for r in clean],[r['RF'] for r in clean])
                                exposures[active]={**ff,'ivol_capm_252':capm['idio_vol'],
                                    'volatility':float(__import__('numpy').std([r['ret'] for r in clean[-21:]],ddof=1)) if len(clean)>=21 and all(math.isfinite(r['ret']) for r in clean[-21:]) else None}
                        for r in rows(con,sql):
                            if r['security_id']!=active:
                                flush()
                                active,hist=r['security_id'],[]
                            hist.append(r)
                            if len(hist)>252:
                                raise ValueError('duplicate price session or oversized exposure window')
                        flush()
                output=[]
                momentum=signal_history(con,previous,11,1) if day<=F.END else {}
                for row in primitive:
                    ex=exposures.get(row['security_id'],{})
                    qualified=bool(caps.get(row['security_id'],False))
                    value=row['be_q0']/row['me'] if row['be_q0'] is not None and row['me'] and row['me']>0 else None
                    style={'size':math.log(row['me']) if row['me'] and row['me']>0 else None,
                        'value':value,'momentum':momentum.get(row['cik']),'volatility':ex.get('volatility'),
                        'liquidity':-row['amihud_21'] if row['amihud_21'] is not None else None,
                        'growth':row['at_q0']/row['at_q4']-1 if row['at_q0'] is not None and row['at_q4'] and row['at_q4']>0 else None,
                        'leverage':row['debt_at'],'quality':row['gprof']}
                    if not qualified:
                        style=dict.fromkeys(R.STYLES)
                    result={k:row.get(k) for k in ('session_date','security_id','cik','univ_all_common','univ_r3000p','ff49','adv_21','adv_63','spread_cs_21','si_to_io','si_shares','ftd_quantity','on_threshold_list')}
                    result.update(style,available_at=_cutoff(day),cap_qualified=qualified,beta_252=ex.get('beta_252'),
                        ivol_ff5_252=ex.get('ivol_ff5_252'),ivol_capm_252=ex.get('ivol_capm_252'),
                        risk_missing_reason=None if ex.get('beta_252') is not None else 'missing_own_factor_history')
                    result.update({f'beta_ff5_{k}':v for k,v in zip(R.FF5,ex.get('ff5_betas',[None]*5))})
                    output.append(result)
                for style in R.STYLES:
                    normalized=R.zscores([r[style] if r['univ_all_common'] and r['cap_qualified'] else None for r in output])
                    for r,v in zip(output,normalized):
                        r[style]=v
                generation.write(unit,output)
            setup_check(spec)
            F.bind_snapshot(manifest,f['sha256'],'factors')
            # Measure output nulls on every member cell, including incomplete rows.
            files=[generation.path/v['path'] for v in generation.state['units'].values()]
            view=S.parquet_relation(files)
            required_fields=['beta_252',*[f'beta_ff5_{k}' for k in R.FF5],'ivol_ff5_252','ff49',*R.STYLES,'adv_21','adv_63','spread_cs_21','si_to_io']
            checks=' AND '.join(k+' IS NOT NULL' for k in required_fields)
            coverage=list(rows(con,f"SELECT year(session_date) AS year,count(*) FILTER(WHERE univ_all_common) AS all_common_denominator,count(*) FILTER(WHERE univ_all_common AND {checks}) AS all_common_complete,count(*) FILTER(WHERE univ_r3000p) AS r3000p_denominator,count(*) FILTER(WHERE univ_r3000p AND {checks}) AS r3000p_complete FROM {view} GROUP BY 1 ORDER BY 1"))
            gate='PASS' if {r['year'] for r in coverage}>={2020,2021,2022,2023,2024,2025} and all(r['all_common_denominator']>0 and r['all_common_complete']/r['all_common_denominator']>=.99 for r in coverage) else 'FAIL'
            return generation.publish(required,{'schema_id':'atx.alpha-panel.risk-inputs/v1','config':F.CONFIG,
                'config_sha256':F.config_hash(),'input_manifests_sha256':{k:v['sha256'] for k,v in spec['release'].items()},
                'coverage_by_year':coverage,'gate_status':{'exposure_coverage':gate,'threshold':.99}})
    finally:
        con.close()
