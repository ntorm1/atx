"""Explicit-relation original liquidity-v1 projection for R26.

Original liquidity.bucket_sql:76-160 binds native global scratch paths. This
projection preserves that SQL, constants, missing sessions and all output fields,
while the caller binds independently admitted buckets/calendar/shares relations.
It neither qualifies originals nor opens or modifies native producer scratch.
"""
from . import liquidity as L, market_common as M, common as C


def calendar_query(calendar):
    """Original market_common.calendar_sql:48-52 on an admitted relation."""
    return f'(SELECT session_date,CAST(row_number() OVER(ORDER BY session_date) AS INTEGER) AS sidx,lag(session_date) OVER(ORDER BY session_date) AS prev_session FROM {calendar})'


def price_query(prices,calendar,b):
    """Original market_common.project_prices:69-75, without global scratch IO."""
    if type(b) is not int or not 0<=b<M.BUCKETS:raise ValueError('original native price projection bucket required')
    columns=', '.join('p.'+column for column in M.PROJ_COLUMNS)
    return f'SELECT {columns},cal.sidx,cal.prev_session FROM {prices} p JOIN {calendar_query(calendar)} cal USING(session_date) WHERE p.security_id%{M.BUCKETS}={b} ORDER BY p.security_id,p.session_date'


def query(bucket,shares,calendar,b,k=None):
    if type(b) is not int or not 0<=b<M.BUCKETS:raise ValueError('original native liquidity bucket required')
    condition=M.sub_filter(k);constant=repr(L.K)
    return f'''WITH bars AS (
        SELECT security_id,session_date,sidx,high,low,close,volume,dollar_volume,ret,ret_guarded,
            CASE WHEN fb_action='repaired' OR NOT (return_factor>0 AND isfinite(return_factor)) THEN 1.0
                ELSE return_factor END AS f FROM {bucket} WHERE {condition}),
    span AS (SELECT security_id,min(sidx) AS s0,max(sidx) AS s1 FROM bars GROUP BY 1),
    grid AS (SELECT s.security_id,cal.session_date,cal.sidx FROM span s
        JOIN {calendar} cal ON cal.sidx BETWEEN s.s0 AND s.s1),
    g AS (SELECT grid.security_id,grid.session_date,grid.sidx,b.high,b.low,b.close,b.volume,b.dollar_volume,
        b.ret,b.ret_guarded,b.f,b.security_id IS NOT NULL AS has_bar,sh.shrout
        FROM grid LEFT JOIN bars b USING(security_id,sidx)
        LEFT JOIN (SELECT security_id,session_date,shrout FROM {shares} WHERE security_id%{M.BUCKETS}={b}) sh
        ON sh.security_id=grid.security_id AND sh.session_date=grid.session_date),
    lg AS (SELECT *,lag(high) OVER w*f AS h0,lag(low) OVER w*f AS l0,lag(close) OVER w*f AS c0
        FROM g WINDOW w AS(PARTITION BY security_id ORDER BY sidx)),
    adj AS (SELECT *,CASE WHEN low>c0 THEN high-(low-c0) WHEN high<c0 THEN c0 ELSE high END AS h1,
        CASE WHEN low>c0 THEN c0 WHEN high<c0 THEN low+(c0-high) ELSE low END AS l1 FROM lg),
    pair AS (SELECT *,CASE WHEN h0>=l0 AND l0>0 AND high>=low AND low>0 AND c0>0
        THEN ln(h0/l0)^2+ln(h1/l1)^2 END AS beta,
        CASE WHEN h0>=l0 AND l0>0 AND high>=low AND low>0 AND c0>0
        THEN ln(greatest(h0,h1)/least(l0,l1))^2 END AS gamma,
        CASE WHEN c0>0 AND h0>=l0 AND l0>0 AND high>=low AND low>0
        THEN 4*(ln(c0)-(ln(h0)+ln(l0))/2)*(ln(c0)-(ln(high)+ln(low))/2) END AS ar,
        CASE WHEN has_bar AND dollar_volume>0 AND ret IS NOT NULL AND NOT ret_guarded
        THEN abs(ret)/dollar_volume*1e6 END AS illiq,NOT has_bar OR coalesce(volume,0)<=0 AS no_trade FROM adj),
    cs AS (SELECT *,CASE WHEN beta IS NOT NULL THEN greatest(
        2*(exp((sqrt(2*beta)-sqrt(beta))/{constant}-sqrt(gamma/{constant}))-1)
        /(1+exp((sqrt(2*beta)-sqrt(beta))/{constant}-sqrt(gamma/{constant}))),0) END AS cs2 FROM pair),
    win AS (SELECT security_id,session_date,has_bar,no_trade,
        avg(cs2) OVER w21 AS cs_mean,count(cs2) OVER w21 AS n_cs,
        avg(ar) OVER w21 AS ar_mean,count(ar) OVER w21 AS n_ar,
        avg(illiq) OVER w21 AS a21,count(illiq) OVER w21 AS n21,
        avg(illiq) OVER w63 AS a63,count(illiq) OVER w63 AS n63,
        avg(illiq) OVER w252 AS a252,count(illiq) OVER w252 AS n252,
        CASE WHEN shrout>0 THEN coalesce(volume,0)/shrout END AS turnover,
        sum(CASE WHEN shrout>0 THEN coalesce(volume,0)/shrout END) OVER w21/count(*) OVER w21 AS t21,
        count(shrout) OVER w21 AS ns21,count(*) OVER w21 AS len21,
        sum(CASE WHEN shrout>0 THEN coalesce(volume,0)/shrout END) OVER w63/count(*) OVER w63 AS t63,
        count(shrout) OVER w63 AS ns63,count(*) OVER w63 AS len63,
        avg(CAST(no_trade AS DOUBLE)) OVER w21 AS zero_vol_21,
        avg(CAST(no_trade AS DOUBLE)) OVER w63 AS zero_vol_63,
        avg(coalesce(dollar_volume,0)) OVER w21 AS adv_21,
        avg(coalesce(dollar_volume,0)) OVER w63 AS adv_63,
        CAST(sum(CAST(no_trade AS INTEGER)) OVER w21 AS INTEGER) AS halt_days_21,shrout FROM cs
        WINDOW w21 AS(PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 20 PRECEDING AND CURRENT ROW),
        w63 AS(PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 62 PRECEDING AND CURRENT ROW),
        w252 AS(PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 251 PRECEDING AND CURRENT ROW))
    SELECT security_id,session_date,has_bar,no_trade AS halt_proxy,halt_days_21,
        CASE WHEN n_cs>={L.MIN_PAIRS} THEN cs_mean END AS spread_cs_21,CAST(n_cs AS INTEGER) AS n_pairs_cs_21,
        CASE WHEN n_ar>={L.MIN_PAIRS} THEN sqrt(greatest(ar_mean,0)) END AS spread_ar_21,
        CASE WHEN n21>=10 THEN a21 END AS amihud_21,CASE WHEN n63>=30 THEN a63 END AS amihud_63,
        CASE WHEN n252>=60 THEN a252 END AS amihud_252,CAST(n252 AS INTEGER) AS n_amihud_252,
        turnover,CASE WHEN ns21*2>=len21 THEN t21 END AS turnover_21,
        CASE WHEN ns63*2>=len63 THEN t63 END AS turnover_63,zero_vol_21,zero_vol_63,adv_21,adv_63,
        shrout AS shrout_used,CAST(session_date AS TIMESTAMP)+{M.MARK} AS available_at
        FROM win WHERE session_date>=DATE '{C.WARMUP_START}' '''
