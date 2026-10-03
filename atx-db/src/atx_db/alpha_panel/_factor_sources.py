"""Task6 native source SQL. Explicit paths only, provisional inspected schemas.

Missing historical membership and cap certifications are left null, never supplied
from a current snapshot. Callers register the exact manifest-bound source relations.
"""
from __future__ import annotations
import datetime as dt
from pathlib import Path


def parquet_relation(paths: list[Path]) -> str:
    if not paths:
        raise ValueError('explicit source output list required')
    quoted = ','.join("'"+p.as_posix().replace("'", "''")+"'" for p in paths)
    return f'read_parquet([{quoted}], union_by_name=true, hive_partitioning=false)'


def register(con, sources: dict[str, list[Path]]):
    required = {'prices', 'universe', 'funda', 'fundq', 'ratios_state', 'calendar',
                'cap_quality', 'liquidity', 'borrow', 'industry', 'rf', 'eg_training'}
    if set(sources) != required:
        raise ValueError('all native source relations must be explicit (empty typed fixtures for unavailable sources)')
    for name, paths in sources.items():
        con.execute(f'CREATE OR REPLACE TEMP VIEW t6_{name} AS SELECT * FROM {parquet_relation(paths)}')


def annual_sql(formation: dt.date, cutoff: dt.datetime) -> str:
    """Latest visible annual vintage per period; prior fiscal year and prior assets."""
    y = formation.year-1
    tau = cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    return f"""
    WITH visible AS (
      SELECT * FROM t6_funda WHERE available_at < TIMESTAMP '{tau}'
      QUALIFY row_number() OVER(PARTITION BY cik,datadate ORDER BY available_at DESC,accession DESC)=1),
    a AS (SELECT *, coalesce(seq,ceq+coalesce(pstk,0),"at"-lt)+coalesce(txditc,0)-coalesce(pstk,0) AS be
      FROM visible WHERE year(datadate)={y}
      QUALIFY row_number() OVER(PARTITION BY cik ORDER BY datadate DESC)=1),
    p AS (SELECT cik, "at" AS at_prior,available_at AS prior_available_at FROM visible WHERE year(datadate)={y-1}
      QUALIFY row_number() OVER(PARTITION BY cik ORDER BY datadate DESC)=1)
    SELECT a.cik, a.datadate,greatest(a.available_at,p.prior_available_at) AS available_at, a.be, a.sic,
      CASE WHEN a.be>0 AND sale IS NOT NULL AND (cogs IS NOT NULL OR xsga IS NOT NULL OR xint IS NOT NULL)
           THEN (sale-coalesce(cogs,0)-coalesce(xsga,0)-coalesce(xint,0))/a.be END AS op,
      CASE WHEN at_prior>0 THEN a.at/at_prior-1 END AS investment,
      CASE WHEN a.at>0 THEN (a.sale-coalesce(a.cogs,0)-coalesce(a.xsga,0))/a.at END AS quality
    FROM a LEFT JOIN p USING(cik)"""


def quarterly_sql(cutoff: dt.datetime) -> str:
    tau = cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    return f"""
    WITH q AS (SELECT * FROM t6_fundq WHERE available_at < TIMESTAMP '{tau}'
      QUALIFY row_number() OVER(PARTITION BY cik,datadate ORDER BY available_at DESC,accession DESC)=1),
    lagged AS (SELECT *, lag(coalesce(seqq,ceqq+coalesce(pstkq,0),atq-ltq)-coalesce(pstkq,0)) OVER(PARTITION BY cik ORDER BY datadate) AS be_prior,
      lag(available_at) OVER(PARTITION BY cik ORDER BY datadate) AS be_prior_available_at,
      lag(datadate) OVER(PARTITION BY cik ORDER BY datadate) AS prior_date FROM q),
    calc AS (SELECT *, CASE WHEN be_prior>0 AND date_diff('day',prior_date,datadate) BETWEEN 60 AND 120
      THEN ibq/be_prior END AS roe FROM lagged)
    SELECT cik, datadate,greatest(available_at,be_prior_available_at,
      lag(greatest(available_at,be_prior_available_at),4) OVER(PARTITION BY cik ORDER BY datadate)) AS available_at, roe,
      CASE WHEN date_diff('day',lag(datadate,4) OVER(PARTITION BY cik ORDER BY datadate),datadate) BETWEEN 300 AND 430
      THEN roe-lag(roe,4) OVER(PARTITION BY cik ORDER BY datadate) END AS droe
    FROM calc QUALIFY row_number() OVER(PARTITION BY cik ORDER BY datadate DESC)=1"""


def quality_sql(cutoff: dt.datetime) -> str:
    """Select newest visible audit first; revocations supersede old passes."""
    tau=cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    return quality_sql_at("TIMESTAMP '"+tau+"'")


def quality_sql_at(clock: str) -> str:
    return f"""SELECT * EXCLUDE(n) REPLACE(
      CASE WHEN n>1 THEN error('ambiguous equal-clock cap audit') ELSE
      coalesce(certified AND membership_certified AND audit_status='independently_verified'
        AND evidence_sha256 IS NOT NULL AND shares_source_sha256 IS NOT NULL
        AND membership_source_sha256 IS NOT NULL AND source_available_at<{clock},false)
      END AS certified) FROM (
      SELECT *,count(*) OVER(PARTITION BY security_id,session_date,available_at) AS n
      FROM t6_cap_quality WHERE available_at<{clock}
      QUALIFY row_number() OVER(PARTITION BY security_id,session_date ORDER BY available_at DESC)=1)"""


def assert_price_keys(con):
    if con.execute("SELECT 1 FROM t6_prices WHERE session_date<DATE '2023-01-01' GROUP BY security_id,session_date HAVING count(*)>1 LIMIT 1").fetchone():
        raise ValueError('duplicate/overlapping price line session')


def cap_basis_sql(column='me') -> str:
    """Every eligible constituent must have finite positive ME, including NULLs."""
    return f"(bool_and(coalesce(isfinite({column}) AND {column}>0,false)) AND coalesce(isfinite(sum({column})) AND sum({column})>0,false))"


def issuer_period_sql(start: dt.date, end: dt.date, formation: dt.date, cutoff: dt.datetime) -> str:
    if end >= dt.date(2023,1,1) or start > end or formation >= start:
        raise ValueError('invalid factor period/holdout boundary')
    tau = cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    # Never filter using present membership. Quality is an independently audited
    # line/date observation with its own strict clock, not vendor-share numeric fit.
    return f"""
    WITH u AS (SELECT * FROM t6_universe WHERE session_date=DATE '{formation}' AND univ_all_common),
    quality AS ({quality_sql(cutoff)}),
    grid AS (SELECT u.security_id,c.session_date FROM u CROSS JOIN t6_calendar c
      WHERE c.session_date BETWEEN DATE '{start}' AND DATE '{end}'),
    bars AS (SELECT g.security_id,g.session_date,p.ret,p.ret_guarded,p.close,
      count(p.security_id) OVER(PARTITION BY g.security_id,g.session_date) AS n
      FROM grid g LEFT JOIN t6_prices p USING(security_id,session_date)
      WHERE g.session_date<DATE '2023-01-01'),
    r AS (SELECT security_id, CASE WHEN bool_or(n>1) THEN error('duplicate price session')
      WHEN bool_or(n=0 OR coalesce(ret_guarded,true) OR ret IS NULL OR ret< -1) THEN NULL
      WHEN bool_or(ret=-1) THEN -1 ELSE exp(sum(ln(1+ret)))-1 END AS ret FROM bars GROUP BY security_id),
    joined AS (SELECT u.*,r.ret,q.certified,q.membership_certified,q.reason AS cap_reason
      FROM u LEFT JOIN r USING(security_id)
      LEFT JOIN quality q ON q.security_id=u.security_id AND q.session_date=u.session_date)
    SELECT cik, CASE WHEN {cap_basis_sql()} THEN sum(me) END AS me,
      CASE WHEN {cap_basis_sql()} AND count(ret)=count(*) THEN sum(me*ret)/sum(me) END AS ret,
      bool_and(exchcd_like=1) AS nyse,
      ({cap_basis_sql()} AND bool_and(coalesce(certified,false) AND coalesce(membership_certified,false))) AS certified,
      CASE WHEN NOT {cap_basis_sql()} THEN 'invalid_eligible_constituent_me'
        ELSE string_agg(DISTINCT coalesce(cap_reason,'missing_cap_or_membership_certification'),';') END AS quality_reason
    FROM joined WHERE cik IS NOT NULL GROUP BY cik"""


def risk_primitives_sql(day: dt.date, cutoff: dt.datetime) -> str:
    tau = cutoff.astimezone(dt.UTC).replace(tzinfo=None).isoformat()
    return f"""
    WITH u AS (SELECT * FROM t6_universe WHERE session_date=DATE '{day}'),
    q AS ({quality_sql(cutoff)}),
    lag_u AS (SELECT l.*,coalesce(q.certified AND l.available_at<TIMESTAMP '{tau}',false) AS cap_qualified FROM t6_universe l
      LEFT JOIN q USING(security_id,session_date)
      WHERE l.session_date=(SELECT max(session_date) FROM t6_calendar WHERE session_date<DATE '{day}')),
    issuer AS (SELECT cik,CASE WHEN bool_and(coalesce(cap_qualified,false)) AND {cap_basis_sql()} THEN sum(me) END AS me
      FROM lag_u WHERE univ_all_common GROUP BY cik),
    l AS (SELECT * FROM t6_liquidity WHERE available_at<TIMESTAMP '{tau}'
      AND date_diff('day',session_date,DATE '{day}') BETWEEN 0 AND 10
      QUALIFY row_number() OVER(PARTITION BY security_id ORDER BY session_date DESC)=1),
    b AS (SELECT * FROM t6_borrow WHERE cutoff_utc<TIMESTAMPTZ '{cutoff.isoformat()}'
      QUALIFY row_number() OVER(PARTITION BY security_id ORDER BY session_date DESC)=1),
    i AS (SELECT * FROM t6_industry WHERE available_at<TIMESTAMP '{tau}'
      QUALIFY row_number() OVER(PARTITION BY cik ORDER BY available_at DESC)=1),
    r AS (SELECT * FROM t6_ratios_state WHERE available_at<TIMESTAMP '{tau}'
      AND date_diff('day',datadate,DATE '{day}') BETWEEN 0 AND 400
      QUALIFY row_number() OVER(PARTITION BY cik ORDER BY available_at DESC)=1)
    SELECT u.session_date,u.security_id,u.cik,u.univ_all_common,u.univ_r3000p,issuer.me,coalesce(lag_u.cap_qualified AND isfinite(issuer.me) AND issuer.me>0,false) AS cap_qualified,
      i.ff49_no AS ff49,l.adv_21,l.adv_63,l.spread_cs_21,l.amihud_21,
      CASE WHEN b.si_to_io_available_at<TIMESTAMPTZ '{cutoff.isoformat()}' THEN b.si_to_io END AS si_to_io,
      CASE WHEN b.si_available_at<TIMESTAMPTZ '{cutoff.isoformat()}' THEN b.si_shares END AS si_shares,
      CASE WHEN b.ftd_available_at<TIMESTAMPTZ '{cutoff.isoformat()}' AND date_diff('day',b.ftd_settlement_date,DATE '{day}') BETWEEN 0 AND 60 THEN b.ftd_quantity END AS ftd_quantity,
      CASE WHEN b.threshold_available_at<TIMESTAMPTZ '{cutoff.isoformat()}' AND date_diff('day',b.threshold_list_date,DATE '{day}') BETWEEN 0 AND 10 THEN b.on_threshold_list END AS on_threshold_list,
      r.be_q0,r.at_q0,r.at_q4,r.debt_at,r.gprof,r.available_at AS accounting_available_at,
      l.available_at AS liquidity_available_at,i.available_at AS industry_available_at,
      b.si_to_io_available_at,b.si_available_at,b.ftd_available_at,b.threshold_available_at
    FROM u LEFT JOIN lag_u USING(security_id,cik) LEFT JOIN issuer USING(cik) LEFT JOIN l USING(security_id) LEFT JOIN b USING(security_id)
      LEFT JOIN i USING(cik) LEFT JOIN r USING(cik)"""
