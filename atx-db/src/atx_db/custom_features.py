"""Bounded causal price/liquidity features and predeclared decile research.

Only aggregate daily spread series leave SQL. Research eligibility is deliberately
separate from production certification: the current source has neither certified
historical US-common listing membership nor historical delivery vintages.
"""

from __future__ import annotations

import datetime as dt
import json
import math
import re
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore
from .research.stats import calendar_hac_statistics, holm

FEATURE_VERSION = "cf1_v1"
FEATURE_SOURCE = "atx_custom_price_liquidity_v1"
PRICE_SOURCE = "tbltickerhistory3_10y"
LABEL_SOURCE = "atx_forward_returns_survivorship_safe_v1"
HORIZONS = (21, 5, 63)
SPLITS = ("train", "validation", "holdout")
COHORT = "bar_observed_price5_adv63_1m_v1_not_certified_us_common"
FEATURE_DEFINITIONS = {
    "reversal_5": "1-A[t]/A[t-5]; six valid adjusted closes in six calendar sessions",
    "momentum_126_skip_21": "A[t-21]/A[t-126]-1; exact endpoints and >=100 valid closes in 106 sessions t-126..t-21",
    "volatility_scaled_momentum_63": "(A[t]/A[t-63]-1)/(sample_sd(log(A[s]/A[s-1]))*sqrt(63)); >=50 exact adjacent returns, positive sd",
    "dollar_volume_shock": "mean(DV[t-4:t])/mean(DV[t-62:t])-1; five/at least50 valid sessions, positive long mean",
    "close_location_pressure_21": "sum(CLV*DV)/sum(DV) over t-20..t; >=17 valid sessions; CLV=(2*close-high-low)/(high-low), high>low",
    "range_compression": "1-mean((high-low)/close,t-4:t)/mean((high-low)/close,t-62:t); five/at least50 valid sessions, positive long mean",
    "liquidity_conditioned_reversal": "reversal_5/(1+ln(1+mean(DV,t-62:t)/1000000)); >=50 valid DV sessions",
    "compression_accumulation": "range_compression*close_location_pressure_21; both components valid",
}
PRODUCTION_BLOCKERS = (
    "historical US-common listing membership is not certified",
    "source economic adjustments and identity history are not certified",
    "modeled availability is not a verified historical delivery vintage",
    "complete delisting event and terminal-label coverage is not certified",
    "cost scenarios do not verify execution, borrow, capacity or realized turnover",
)


@dataclass(frozen=True)
class CustomFeatureOptions:
    as_of_date: dt.date
    run_at: dt.datetime
    run_id: str
    source_sha256: str
    feature_source: str = FEATURE_SOURCE
    price_source: str = PRICE_SOURCE
    partitions: int = 16


@dataclass(frozen=True)
class CustomEvaluationOptions:
    as_of_date: dt.date
    run_at: dt.datetime
    run_id: str
    build_run_id: str
    label_source: str = LABEL_SOURCE


def _json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _validate_run(as_of: dt.date, run_at: dt.datetime, run_id: str) -> dt.datetime:
    if not run_id.strip():
        raise ValueError("run_id must be nonempty")
    if run_at.tzinfo is not None:
        run_at = run_at.astimezone(dt.UTC).replace(tzinfo=None)
    if run_at.date() < as_of:
        raise ValueError("run_at cannot precede as_of_date")
    return run_at


def _new_run(store: DuckDBStore, run_id: str) -> None:
    if store.con.execute("SELECT 1 FROM custom_feature_runs WHERE run_id=?", [run_id]).fetchone():
        raise ValueError("run_id already exists; use a new explicit run id")


def _calendar(store: DuckDBStore, price_source: str, as_of: dt.date) -> None:
    store.con.execute("""
        CREATE TEMP TABLE _cf_calendar AS
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (SELECT DISTINCT trade_date FROM equity_daily_bars
              WHERE source=? AND trade_date<=?)
    """, [price_source, as_of])


def _calendar_digest(store: DuckDBStore, as_of: dt.date) -> str:
    row = store.con.execute(
        "SELECT sha256(coalesce(string_agg(CAST(trade_date AS VARCHAR),',' ORDER BY trade_date),'')) "
        "FROM _cf_calendar WHERE trade_date<=?", [as_of],
    ).fetchone()
    assert row is not None
    return str(row[0])


def _definitions() -> list[list[str]]:
    return [[FEATURE_VERSION, feature, _json({
        "formula": formula, "direction": "+1; higher predicts higher forward return",
        "adjusted_price": "A=corrected same-row raw close*cumulative total-return factor; no split-factor inference",
        "dollar_volume": "DV=raw close*raw volume, finite positive close, finite nonnegative volume",
        "windows": "inclusive calendar-session ranges; no missing-session compression or filling",
        "timing": "decision T22UTC uses through previous observed session; entry next observed session close",
        "availability": "max(stored available_at, observation date+1 calendar day noon UTC); max over selected 127-session input window",
        "primary_horizon": 21, "secondary_horizons": [5, 63],
    })] for feature, formula in FEATURE_DEFINITIONS.items()]


def refresh_custom_features(store: DuckDBStore, options: CustomFeatureOptions) -> dict[str, Any]:
    """Replace one feature source/version atomically using sequential SQL partitions.

    Caller applies governed migrations and configures the connection (1GB/1thread
    recommended). The supplied digest identifies the published bulk input; CF1 does
    not re-ingest or rehash that multi-GB source. It is retained as operator evidence.
    """
    run_at = _validate_run(options.as_of_date, options.run_at, options.run_id)
    if not re.fullmatch(r"[0-9a-f]{64}", options.source_sha256):
        raise ValueError("source_sha256 must be a lowercase 64-character SHA256")
    if not 1 <= options.partitions <= 256:
        raise ValueError("partitions must be in 1..256; these execute sequentially")
    _new_run(store, options.run_id)
    con = store.con
    definitions = _definitions()
    existing_definitions = dict(con.execute(
        "SELECT feature_id,definition_json FROM custom_feature_definitions WHERE feature_version=?",
        [FEATURE_VERSION],
    ).fetchall())
    if any(feature in existing_definitions and existing_definitions[feature] != definition
           for _, feature, definition in definitions):
        raise ValueError("pinned feature definitions changed; bump FEATURE_VERSION before rebuilding")
    try:
        _calendar(store, options.price_source, options.as_of_date)
        con.execute("CREATE TEMP TABLE _cf_output AS SELECT * FROM custom_features_daily WHERE false")
        for partition in range(options.partitions):
            con.execute("""
                CREATE OR REPLACE TEMP TABLE _cf_bars AS
                WITH unique_bars AS (
                    SELECT b.*, c.session_number,
                           count(*) OVER (PARTITION BY security_id,trade_date) AS key_count
                    FROM equity_daily_bars b JOIN _cf_calendar c USING(trade_date)
                    WHERE source=? AND hash(security_id)%?=?
                )
                SELECT security_id, trade_date, session_number,
                       CASE WHEN isfinite(adjusted_close) AND adjusted_close>0 THEN adjusted_close END AS a,
                       CASE WHEN isfinite(close) AND close>0 THEN close END AS raw_close,
                       CASE WHEN isfinite(close) AND close>0 AND volume>=0
                            AND isfinite(close*volume) THEN close*volume END AS dv,
                       CASE WHEN high>=greatest(open,close,low) AND low<=least(open,close,high)
                                 AND low>0 AND close>0 AND isfinite(high) AND isfinite(low)
                                 AND isfinite(close) THEN (high-low)/close END AS intraday_range,
                       CASE WHEN high>low AND close BETWEEN low AND high AND low>0
                                 AND isfinite(high) AND isfinite(low) AND isfinite(close)
                            THEN (2*close-high-low)/(high-low) END AS clv,
                       greatest(available_at,trade_date::TIMESTAMP+INTERVAL 1 DAY+INTERVAL 12 HOUR) AS observed_at,
                       available_at IS NOT NULL AS has_stored_clock
                FROM unique_bars WHERE key_count=1
            """, [options.price_source, options.partitions, partition])
            con.execute("""
                INSERT INTO _cf_output
                WITH legs AS (
                    SELECT b.*, l5.a AS a5, l21.a AS a21, l63.a AS a63, l126.a AS a126,
                           CASE WHEN p.a>0 AND b.a>0 THEN ln(b.a/p.a) END AS daily_log_return
                    FROM _cf_bars b
                    LEFT JOIN _cf_bars p ON p.security_id=b.security_id AND p.session_number=b.session_number-1
                    LEFT JOIN _cf_bars l5 ON l5.security_id=b.security_id AND l5.session_number=b.session_number-5
                    LEFT JOIN _cf_bars l21 ON l21.security_id=b.security_id AND l21.session_number=b.session_number-21
                    LEFT JOIN _cf_bars l63 ON l63.security_id=b.security_id AND l63.session_number=b.session_number-63
                    LEFT JOIN _cf_bars l126 ON l126.security_id=b.security_id AND l126.session_number=b.session_number-126
                ), windows AS (
                    SELECT *, count(a) OVER w6 AS n6, count(a) OVER w127 AS n127,
                           count(a) OVER w_mom AS n_mom,
                           count(daily_log_return) OVER w63 AS nr63,
                           stddev_samp(daily_log_return) OVER w63 AS sd63,
                           count(dv) OVER w5 AS nd5, count(dv) OVER w63 AS nd63,
                           avg(dv) OVER w5 AS dv5, avg(dv) OVER w63 AS dv63,
                           count(intraday_range) OVER w5 AS ng5,
                           count(intraday_range) OVER w63 AS ng63,
                           avg(intraday_range) OVER w5 AS g5, avg(intraday_range) OVER w63 AS g63,
                           count(CASE WHEN clv IS NOT NULL THEN dv END) OVER w21 AS nc21,
                           sum(clv*dv) OVER w21 AS pressure_sum,
                           sum(CASE WHEN clv IS NOT NULL THEN dv END) OVER w21 AS pressure_dv,
                           max(observed_at) OVER w127 AS input_available_at,
                           bool_and(has_stored_clock) OVER w127 AS clocks_present
                    FROM legs WINDOW
                        w5 AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 4 PRECEDING AND CURRENT ROW),
                        w6 AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 5 PRECEDING AND CURRENT ROW),
                        w21 AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 20 PRECEDING AND CURRENT ROW),
                        w63 AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 62 PRECEDING AND CURRENT ROW),
                        w127 AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 126 PRECEDING AND CURRENT ROW),
                        w_mom AS (PARTITION BY security_id ORDER BY session_number RANGE BETWEEN 126 PRECEDING AND 21 PRECEDING)
                ), features AS (
                    SELECT *,
                        CASE WHEN n6=6 THEN 1-a/a5 END AS reversal,
                        CASE WHEN n_mom>=100 THEN a21/a126-1 END AS momentum,
                        CASE WHEN nr63>=50 AND sd63>0 THEN (a/a63-1)/(sd63*sqrt(63)) END AS scaled_momentum,
                        CASE WHEN nd5=5 AND nd63>=50 AND dv63>0 THEN dv5/dv63-1 END AS dv_shock,
                        CASE WHEN nc21>=17 AND pressure_dv>0 THEN pressure_sum/pressure_dv END AS pressure,
                        CASE WHEN ng5=5 AND ng63>=50 AND g63>0 THEN 1-g5/g63 END AS compression
                    FROM windows
                )
                SELECT ?, ?, f.security_id, d.trade_date,
                       d.trade_date::TIMESTAMP+INTERVAL 22 HOUR, f.trade_date,
                       f.input_available_at, e.trade_date, d.session_number,
                       clocks_present AND input_available_at<=d.trade_date::TIMESTAMP+INTERVAL 22 HOUR,
                       coalesce(raw_close>=5 AND nd63>=50 AND dv63>=1000000, false),
                       raw_close, dv63, nd63, n127, nr63,
                       CASE WHEN isfinite(reversal) THEN reversal END,
                       CASE WHEN isfinite(momentum) THEN momentum END,
                       CASE WHEN isfinite(scaled_momentum) THEN scaled_momentum END,
                       CASE WHEN isfinite(dv_shock) THEN dv_shock END,
                       CASE WHEN isfinite(pressure) THEN pressure END,
                       CASE WHEN isfinite(compression) THEN compression END,
                       CASE WHEN nd63>=50 AND isfinite(reversal) AND dv63>=0
                            THEN reversal/(1+ln(1+dv63/1000000)) END,
                       CASE WHEN isfinite(compression*pressure) THEN compression*pressure END, ?
                FROM features f
                JOIN _cf_calendar d ON d.session_number=f.session_number+1
                JOIN _cf_calendar e ON e.session_number=f.session_number+2
            """, [options.feature_source, FEATURE_VERSION, options.run_id])
        counts = con.execute("""
            SELECT count(*), count(*) FILTER (WHERE known_by_decision),
                   count(*) FILTER (WHERE known_by_decision AND cohort_eligible),
                   min(decision_date),max(decision_date) FROM _cf_output
        """).fetchone()
        assert counts is not None
        diagnostics = dict(zip(("rows", "known_by_decision_rows", "cohort_rows", "first_decision", "last_decision"), counts, strict=True))
        for field in ("first_decision", "last_decision"):
            diagnostics[field] = str(diagnostics[field]) if diagnostics[field] else None
        diagnostics["production_blockers"] = list(PRODUCTION_BLOCKERS)
        config = {"cohort": COHORT, "partitions_sequential": options.partitions,
                  "price_basis": "adjusted_close", "definition_count": 8,
                  "calendar_sha256": _calendar_digest(store, options.as_of_date),
                  "stored_source_loaded_at": "operational load clock, not historical availability proof",
                  "source_sha256_evidence": "operator-supplied published bulk artifact digest"}
        with store.transaction():
            con.execute("DELETE FROM custom_features_daily WHERE feature_source=? AND feature_version=?",
                        [options.feature_source, FEATURE_VERSION])
            con.execute("INSERT INTO custom_features_daily SELECT * FROM _cf_output")
            con.executemany("INSERT INTO custom_feature_definitions VALUES (?,?,?) ON CONFLICT DO NOTHING", definitions)
            con.execute("INSERT INTO custom_feature_runs VALUES (?,?,?,?,?,?,?,?,?,?)", [
                options.run_id, "build", options.feature_source, FEATURE_VERSION,
                options.price_source, options.source_sha256, options.as_of_date, run_at,
                _json(config), _json(diagnostics),
            ])
        return diagnostics
    finally:
        for table in ("_cf_output", "_cf_bars", "_cf_calendar"):
            con.execute(f"DROP TABLE IF EXISTS {table}")


def holm_eight(p_values: dict[str, float | None]) -> dict[str, float | None]:
    """Eight predeclared primary hypotheses; missing tests remain in the family.

    Delegates to ``research.stats.holm`` over exactly ``FEATURE_DEFINITIONS``;
    ``calendar_hac_statistics`` is re-exported from ``research.stats`` unchanged.
    """
    return holm({feature: p_values.get(feature) for feature in FEATURE_DEFINITIONS})


def evaluate_custom_features(store: DuckDBStore, options: CustomEvaluationOptions) -> dict[str, Any]:
    """Persist every predeclared result, including empty and unsuccessful hypotheses.

    Only a currently materialized build can be evaluated. Outcomes are ranked AFTER
    feature deciles are fixed. Evaluation results reference their immutable build
    manifest even after a later feature refresh replaces the wide daily snapshot.
    """
    run_at = _validate_run(options.as_of_date, options.run_at, options.run_id)
    _new_run(store, options.run_id)
    con = store.con
    build = con.execute("""
        SELECT feature_source,feature_version,price_source,source_sha256,as_of_date,
               configuration_json,diagnostics_json
        FROM custom_feature_runs WHERE run_id=? AND run_kind='build'
    """, [options.build_run_id]).fetchone()
    if build is None or build[1] != FEATURE_VERSION:
        raise ValueError("build_run_id must identify a CF1 build manifest")
    if options.as_of_date < build[4]:
        raise ValueError("evaluation as_of_date cannot precede the build snapshot")
    actual = con.execute("""
        SELECT count(*),count(*) FILTER (WHERE build_run_id<>?)
        FROM custom_features_daily WHERE feature_source=? AND feature_version=?
    """, [options.build_run_id, build[0], FEATURE_VERSION]).fetchone()
    assert actual is not None
    if actual[1] or actual[0] != json.loads(build[6])["rows"]:
        raise ValueError("requested build was replaced or its daily rows changed; rebuild before evaluating")
    cutoff = dt.datetime.combine(options.as_of_date, dt.time(22))
    available_horizons = {row[0] for row in con.execute(
        "SELECT DISTINCT horizon_days FROM forward_returns_survivorship_safe "
        "WHERE source=? AND available_at<=?", [options.label_source, cutoff],
    ).fetchall()}
    missing_horizons = sorted(set(HORIZONS)-available_horizons)
    if missing_horizons:
        raise ValueError(f"production label prerequisite failed: missing required horizons {missing_horizons}; "
                         "build the corrected adjusted-price survivorship panel before evaluation")
    try:
        _calendar(store, build[2], options.as_of_date)
        calendar_hash = _calendar_digest(store, build[4])
        if calendar_hash != json.loads(build[5])["calendar_sha256"]:
            raise ValueError("source calendar changed since the feature build; rebuild features")
        con.execute("CREATE TEMP TABLE _cf_deciles AS SELECT * FROM custom_feature_deciles WHERE false")
        con.execute("""
            CREATE TEMP TABLE _cf_dates AS
            SELECT f.decision_date, min(f.session_number) AS session_number,
                   min(f.entry_date) AS entry_date,
                   count(*) FILTER (WHERE f.known_by_decision AND f.cohort_eligible) AS cohort_count,
                   CASE WHEN f.decision_date<DATE '2021-01-01' THEN 'train'
                        WHEN f.decision_date<DATE '2024-01-01' THEN 'validation' ELSE 'holdout' END AS split
            FROM custom_features_daily f WHERE f.build_run_id=?
            GROUP BY f.decision_date
        """, [options.build_run_id])
        # Labels remain in SQL, scoped to the source/cutoff. Select revisions
        # before checking economic validity; the later join uses exact entry keys.
        con.execute("""
            CREATE TEMP VIEW _cf_labels AS
            SELECT * EXCLUDE (revision) FROM (
                SELECT l.*, row_number() OVER (
                    PARTITION BY security_id,as_of_date,horizon_days
                    ORDER BY available_at DESC,source_loaded_at DESC,forward_return_id DESC
                ) AS revision
                FROM forward_returns_survivorship_safe l
                WHERE source=$label_source AND available_at<=$cutoff
                  AND horizon_days IN (5,21,63)
            ) WHERE revision=1
        """.replace("$label_source", "'" + options.label_source.replace("'", "''") + "'")
              .replace("$cutoff", "TIMESTAMP '" + cutoff.isoformat() + "'"))
        for feature in FEATURE_DEFINITIONS:
            # SQL identifiers are solely our fixed dictionary keys, never caller text.
            con.execute(f"""
                CREATE OR REPLACE TEMP TABLE _cf_ranks AS
                WITH eligible AS (
                    SELECT security_id,decision_date,entry_date,{feature} AS feature_value
                    FROM custom_features_daily
                    WHERE build_run_id=? AND known_by_decision AND cohort_eligible
                      AND isfinite({feature})
                ), ranks AS (
                    SELECT *,count(*) OVER (PARTITION BY decision_date) AS n,
                           min(feature_value) OVER (PARTITION BY decision_date) AS lo,
                           max(feature_value) OVER (PARTITION BY decision_date) AS hi,
                           ntile(10) OVER (PARTITION BY decision_date ORDER BY feature_value,security_id) AS decile
                    FROM eligible
                ) SELECT * FROM ranks
            """, [options.build_run_id])
            for horizon in HORIZONS:
                con.execute("""
                    INSERT INTO _cf_deciles
                    WITH date_rules AS (
                        SELECT d.*, ending.trade_date AS expected_end_date,
                            coalesce(r.n,0) AS feature_eligible_count,
                            CASE
                              WHEN ending.trade_date IS NULL THEN 'not_matured'
                              WHEN d.split='train' AND ending.trade_date>=DATE '2021-01-01' THEN 'purged_split_crossing'
                              WHEN d.split='validation' AND ending.trade_date>=DATE '2024-01-01' THEN 'purged_split_crossing'
                              WHEN d.split='validation' AND d.session_number < (
                                SELECT min(session_number)+63 FROM _cf_calendar WHERE trade_date>=DATE '2021-01-01'
                              ) THEN 'embargo'
                              WHEN d.split='holdout' AND d.session_number < (
                                SELECT min(session_number)+63 FROM _cf_calendar WHERE trade_date>=DATE '2024-01-01'
                              ) THEN 'embargo'
                              WHEN coalesce(r.n,0)<200 THEN 'insufficient_names'
                              WHEN r.lo=r.hi THEN 'constant_features'
                              ELSE 'evaluated' END AS status
                        FROM _cf_dates d
                        JOIN _cf_calendar entry ON entry.trade_date=d.entry_date
                        LEFT JOIN _cf_calendar ending ON ending.session_number=entry.session_number+?
                        LEFT JOIN (SELECT decision_date,max(n) AS n,min(lo) AS lo,max(hi) AS hi
                                   FROM _cf_ranks GROUP BY decision_date) r USING(decision_date)
                    ), joined AS (
                        SELECT d.*,r.decile,r.feature_value,r.security_id,
                               l.forward_return_id,l.forward_return,l.is_delisted_in_horizon,
                               l.terminal_return_source,l.available_at,
                               coalesce(isfinite(l.forward_return) AND l.forward_return>=-1
                                 AND l.forward_end_date=d.expected_end_date
                                 AND (NOT l.is_delisted_in_horizon
                                      OR l.terminal_return_source IN ('observed','policy'))
                                 AND greatest(l.available_at,l.forward_end_date::TIMESTAMP+INTERVAL 1 DAY+INTERVAL 12 HOUR)<=?, false) AS valid_label
                        FROM date_rules d
                        JOIN _cf_ranks r USING(decision_date)
                        LEFT JOIN _cf_labels l ON l.security_id=r.security_id
                            AND l.as_of_date=d.entry_date AND l.horizon_days=?
                        WHERE d.status='evaluated'
                    )
                    SELECT ?,?, ?,decision_date,session_number,entry_date,expected_end_date,split,status,
                           decile,cohort_count,feature_eligible_count,count(*),
                           count(*) FILTER (WHERE valid_label),count(*) FILTER (WHERE NOT valid_label),
                           count(*) FILTER (WHERE valid_label AND is_delisted_in_horizon),
                           count(*) FILTER (WHERE valid_label AND is_delisted_in_horizon AND terminal_return_source='policy'),
                           count(*) FILTER (WHERE forward_return_id IS NOT NULL AND NOT valid_label),
                           avg(feature_value),avg(forward_return) FILTER (WHERE valid_label),
                           max(available_at) FILTER (WHERE valid_label)
                    FROM joined GROUP BY decision_date,session_number,entry_date,expected_end_date,split,status,
                                         decile,cohort_count,feature_eligible_count
                    UNION ALL
                    SELECT ?,?, ?,decision_date,session_number,entry_date,expected_end_date,split,status,
                           0,cohort_count,feature_eligible_count,0,0,0,0,0,0,NULL,NULL,NULL
                    FROM date_rules WHERE status<>'evaluated'
                """, [horizon, cutoff, horizon, options.run_id, feature, horizon,
                      options.run_id, feature, horizon])
        summaries = _evaluation_summaries(store, options.run_id)
        decile_count = con.execute("SELECT count(*) FROM _cf_deciles").fetchone()
        assert decile_count is not None
        diagnostics = {
            "build_run_id": options.build_run_id,
            "decile_rows": decile_count[0],
            "evaluation_rows": len(summaries),
            "holdout_primary_statistically_qualified": [row[1] for row in summaries if row[2] == 21 and row[3] == "holdout" and row[-3]],
            "production_eligible": [], "production_blockers": list(PRODUCTION_BLOCKERS),
            "date_status_counts": [list(row) for row in con.execute(
                "SELECT status,count(DISTINCT (feature_id,horizon_sessions,decision_date)) "
                "FROM _cf_deciles GROUP BY status ORDER BY status").fetchall()],
        }
        config = {"build_run_id": options.build_run_id, "label_source": options.label_source,
                  "label_price_basis_contract": "corrected adjusted_close; operator must run production forward panel in its default adjusted mode",
                  "primary_horizon": 21, "secondary_horizons": [5, 63], "cohort": COHORT,
                  "min_names": 200, "min_names_per_decile_before_labels": 20,
                  "split_rule": "train<=2020; validation2021..2023; holdout>=2024; purge crossing labels; first63 market sessions embargoed at validation/holdout start",
                  "inference": "calendar-aware Bartlett/Newey-West mean SE; lag=horizon-1; asymptotic normal two-sided p; eight-test Holm within each primary split",
                  "costs": "10/25/50bp per side per security: two legs times entry and exit => 4*c from Q10-Q1 horizon spread",
                  "portfolio_claim": "overlapping horizon return spread, not daily PnL or annualized trading Sharpe"}
        with store.transaction():
            con.execute("INSERT INTO custom_feature_deciles SELECT * FROM _cf_deciles")
            con.executemany("INSERT INTO custom_feature_evaluations VALUES (" + ",".join(["?"]*26) + ")", summaries)
            con.execute("INSERT INTO custom_feature_runs VALUES (?,?,?,?,?,?,?,?,?,?)", [
                options.run_id, "evaluate", build[0], FEATURE_VERSION, build[2], build[3],
                options.as_of_date, run_at, _json(config), _json(diagnostics),
            ])
        return diagnostics
    finally:
        for table in ("_cf_ranks", "_cf_dates", "_cf_deciles", "_cf_calendar"):
            con.execute(f"DROP TABLE IF EXISTS {table}")
        con.execute("DROP VIEW IF EXISTS _cf_labels")


def _evaluation_summaries(store: DuckDBStore, run_id: str) -> list[list[Any]]:
    """Only small date/feature/horizon aggregates cross the Python boundary."""
    rows = store.con.execute("""
        SELECT feature_id,horizon_sessions,split,session_number,year(decision_date),
               max(mean_forward_return) FILTER (WHERE decile=10)
                   -max(mean_forward_return) FILTER (WHERE decile=1) AS spread,
               sum(eligible_count),sum(labeled_count),sum(terminal_count),sum(imputed_count)
        FROM _cf_deciles WHERE status='evaluated'
        GROUP BY feature_id,horizon_sessions,split,session_number,year(decision_date)
        ORDER BY feature_id,horizon_sessions,split,session_number
    """).fetchall()
    groups: dict[tuple[str, int, str], list[tuple[Any, ...]]] = {}
    for feature, horizon, split, *data in rows:
        groups.setdefault((feature, horizon, split), []).append(tuple(data))
    records: dict[tuple[str, int, str], dict[str, Any]] = {}
    for feature in FEATURE_DEFINITIONS:
        for horizon in HORIZONS:
            for split in SPLITS:
                group = groups.get((feature, horizon, split), [])
                values = [(row[0], row[2]) for row in group if row[2] is not None]
                statistics = calendar_hac_statistics(values, horizon)
                yearly: dict[int, list[float]] = {}
                for _, year, spread, *_ in group:
                    if spread is not None and math.isfinite(spread):
                        yearly.setdefault(year, []).append(spread)
                annual = {str(year): {"dates": len(values), "gross_mean": math.fsum(values)/len(values)}
                          for year, values in sorted(yearly.items())}
                eligible = sum(row[3] for row in group)
                labeled = sum(row[4] for row in group)
                statistics.update(eligible_count=eligible, labeled_count=labeled,
                                  label_coverage=labeled/eligible if eligible else None,
                                  terminal_count=sum(row[5] for row in group),
                                  imputed_count=sum(row[6] for row in group), annual=annual)
                records[(feature, horizon, split)] = statistics
    for split in SPLITS:
        corrected = holm_eight({feature: records[(feature, 21, split)]["p_value"] for feature in FEATURE_DEFINITIONS})
        for feature, adjusted in corrected.items():
            records[(feature, 21, split)]["holm_p_value"] = adjusted
    output = []
    for (feature, horizon, split), record in records.items():
        mean = record["gross_mean"]
        net = [None if mean is None else mean-4*bp/10000 for bp in (10, 25, 50)]
        annual_stable = len(record["annual"]) >= 2 and all(
            year["dates"] >= 60 and year["gross_mean"] > 0 for year in record["annual"].values()
        )
        # Fixed direction is positive; secondary horizons can never qualify a
        # hypothesis. Keep statistical/economic screening distinct from governance.
        qualified = bool(horizon == 21 and record.get("holm_p_value") is not None
                         and record["holm_p_value"] <= .05 and mean is not None and mean > 0
                         and net[1] is not None and net[1] > 0 and record["spread_dates"] >= 252 and annual_stable
                         and record["label_coverage"] is not None and record["label_coverage"] >= .99)
        output.append([
            run_id, feature, horizon, split, horizon == 21,
            record["spread_dates"], mean, record["hac_lags"], record["hac_standard_error"],
            record["z_statistic"], record["p_value"], record.get("holm_p_value"),
            record["ci95_low"], record["ci95_high"], *net,
            record["eligible_count"], record["labeled_count"], record["label_coverage"],
            record["terminal_count"], record["imputed_count"], _json(record["annual"]),
            qualified, False, _json(PRODUCTION_BLOCKERS),
        ])
    return output
