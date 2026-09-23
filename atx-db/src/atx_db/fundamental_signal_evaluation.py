"""Bounded, frozen fundamental signal decile evaluation."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore
from .custom_features import calendar_hac_statistics
from .fundamental_signal_research import validate_fundamental_signal_panel

HORIZONS = (5, 21, 63)
SPLITS = ("train", "validation", "holdout")
LABEL_VERSION = "forward_return_publication_v1"
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_BLOCKERS = (
    "issuer_cohort_has_no_verified_price_or_adv_screen",
    "liquidity_borrow_and_capacity_not_qualified",
    "vendor_adjustment_and_historical_delivery_vintages_not_certified",
    "selected_label_revisions_are_digest_pinned_not_archived_for_replay",
    "overlapping_horizon_spreads_are_not_portfolio_pnl",
    "local_holm_does_not_correct_prior_research_searches",
)


def _json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _sha(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _digest_rows(con: Any, sql: str, params: list[Any]) -> tuple[int, str]:
    digest = hashlib.sha256()
    count = 0
    cursor = con.execute(sql, params)
    while batch := cursor.fetchmany(2048):
        for row in batch:
            encoded = _json([item.isoformat() if isinstance(item, (dt.date, dt.datetime))
                             else item for item in row])
            digest.update(encoded.encode("utf-8"))
            digest.update(b"\n")
            count += 1
    return count, digest.hexdigest()


def holm_family(p_values: dict[str, float | None]) -> dict[str, float | None]:
    """Holm step-down over every frozen hypothesis, including untestable p=1."""
    ordered = sorted(p_values, key=lambda key: (1 if p_values[key] is None else p_values[key], key))
    result: dict[str, float | None] = {}
    running = 0.0
    for index, key in enumerate(ordered):
        p = p_values[key]
        if p is not None and (not math.isfinite(p) or not 0 <= p <= 1):
            raise ValueError("p-values must be finite and within [0,1]")
        running = max(running, (len(ordered) - index) * (1.0 if p is None else p))
        result[key] = None if p is None else min(1.0, running)
    return result


@dataclass(frozen=True)
class FundamentalSignalEvaluationOptions:
    build_run_id: str
    run_id: str
    as_of_date: dt.date
    run_at: dt.datetime
    label_source: str
    memory_limit: str = "256MB"
    threads: int = 1


def _validate_options(options: FundamentalSignalEvaluationOptions) -> dt.datetime:
    if not _ID.fullmatch(options.run_id) or not _ID.fullmatch(options.build_run_id):
        raise ValueError("run_id and build_run_id must be lower-case identifiers")
    if not options.label_source or len(options.label_source) > 200:
        raise ValueError("explicit bounded label_source is required")
    if type(options.as_of_date) is not dt.date:
        raise ValueError("as_of_date must be a date")
    if options.run_at.tzinfo is None or options.run_at.utcoffset() != dt.timedelta(0):
        raise ValueError("run_at must be timezone-aware UTC")
    run_at = options.run_at.astimezone(dt.UTC).replace(tzinfo=None)
    if run_at < dt.datetime.combine(options.as_of_date, dt.time(22)):
        raise ValueError("run_at must follow evaluation cutoff at 22:00 UTC")
    if options.memory_limit != "256MB" or type(options.threads) is not int or options.threads != 1:
        raise ValueError("FQ2 requires memory_limit=256MB and threads=1")
    return run_at


def _calendar(con: Any, source: str, as_of_date: dt.date, cutoff: dt.datetime) -> None:
    con.execute("""
        CREATE TEMP TABLE _fq2_calendar AS
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (SELECT DISTINCT trade_date FROM market_daily_metrics
              WHERE source=? AND trade_date<=? AND as_of_date<=?
                AND available_at<=? AND available_at<=trade_date::TIMESTAMP+INTERVAL 22 HOUR
                AND as_of_date<=trade_date AND close IS NOT NULL
                AND isfinite(close) AND close>0)
    """, [source, as_of_date, as_of_date, cutoff])


def _split(day: dt.date) -> str:
    return "train" if day.year <= 2020 else "validation" if day.year <= 2023 else "holdout"


def _date_status(con: Any, day: dt.date, entry: dt.date | None, horizon: int,
                 n: int, constant: bool, cutoff: dt.datetime) -> tuple[int, dt.date | None, str]:
    anchor = con.execute("SELECT session_number FROM _fq2_calendar WHERE trade_date=?", [day]).fetchone()
    if anchor is None:
        raise ValueError("build decision calendar differs from evaluation calendar")
    session = int(anchor[0])
    next_day = con.execute("SELECT trade_date FROM _fq2_calendar WHERE session_number=?",
                           [session + 1]).fetchone()
    if entry is not None and (next_day[0] if next_day else None) != entry:
        raise ValueError("build entry calendar differs from evaluation calendar")
    ending = (con.execute("SELECT trade_date FROM _fq2_calendar WHERE session_number=?",
                          [session + 1 + horizon]).fetchone() if entry else None)
    end = ending[0] if ending else None
    status = "evaluated"
    if end is None or dt.datetime.combine(end, dt.time(12)) > cutoff:
        status = "not_matured"
    elif ((_split(day) == "train" and end >= dt.date(2021, 1, 1))
          or (_split(day) == "validation" and end >= dt.date(2024, 1, 1))):
        status = "purged_split_crossing"
    else:
        boundary = dt.date(2021, 1, 1) if _split(day) == "validation" else (
            dt.date(2024, 1, 1) if _split(day) == "holdout" else None)
        if boundary is not None:
            first = con.execute("SELECT min(session_number) FROM _fq2_calendar WHERE trade_date>=?",
                                [boundary]).fetchone()[0]
            if first is not None and session < first + 63:
                status = "embargo"
    if status == "evaluated" and n < 200:
        status = "insufficient_names"
    if status == "evaluated" and constant:
        status = "constant_scores"
    return session, end, status


def _rank_day(con: Any, build_id: str, signal: str, day: dt.date) -> tuple[int, bool, int]:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fq2_rank AS
        WITH scores AS (
          SELECT security_id,entry_date,score FROM fundamental_signal_values
          WHERE run_id=? AND signal_id=? AND decision_date=? AND eligible
            AND score IS NOT NULL AND isfinite(score)
        ), ranked AS (
          SELECT *,ntile(10) OVER (ORDER BY score,security_id) AS decile,
                 lag(score) OVER (ORDER BY score,security_id) AS prior_score
          FROM scores
        ) SELECT security_id,entry_date,score,decile,prior_score FROM ranked
    """, [build_id, signal, day])
    n, distinct_scores = con.execute("SELECT count(*),count(DISTINCT score) FROM _fq2_rank").fetchone()
    ties = con.execute("""
        SELECT count(*) FROM (SELECT score,decile,
                              lag(decile) OVER (ORDER BY score,security_id) AS prior_decile,
                              lag(score) OVER (ORDER BY score,security_id) AS prior_score
                              FROM _fq2_rank)
        WHERE decile<>prior_decile AND score=prior_score
    """).fetchone()[0]
    return int(n), bool(n and distinct_scores == 1), int(ties)


def _join_labels(con: Any, source: str, entry: dt.date, horizon: int,
                 cutoff: dt.datetime, end: dt.date | None) -> None:
    # Revisions are selected before economic validity. A newer invalid row
    # suppresses older valid rows; is_latest_revision is deliberately ignored.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fq2_join AS
        WITH revisions AS (
          SELECT l.*,row_number() OVER (
            PARTITION BY l.security_id,l.as_of_date,l.horizon_days
            ORDER BY l.available_at DESC,l.source_loaded_at DESC,l.forward_return_id DESC
          ) AS revision
          FROM forward_returns_survivorship_safe l JOIN _fq2_rank r
            ON r.security_id=l.security_id
          WHERE l.source=? AND l.as_of_date=? AND l.horizon_days=?
            AND l.available_at<=?
        ), selected AS (SELECT * FROM revisions WHERE revision=1)
        SELECT r.security_id,r.score,r.decile,l.forward_return_id,l.price_basis,
               l.calculation_version,l.raw_forward_return,l.terminal_return,
               l.forward_return,l.forward_end_date,l.delist_date,
               l.terminal_return_source,l.return_observation_id,
               l.is_delisted_in_horizon,l.available_at,
               CASE WHEN l.forward_return_id IS NULL THEN 'missing'
                    WHEN l.price_basis<>'adjusted_close' OR l.price_basis IS NULL
                      OR l.calculation_version<>? OR l.calculation_version IS NULL
                      THEN 'unsupported_basis'
                    WHEN l.forward_return IS NULL OR NOT isfinite(l.forward_return)
                      OR l.forward_return < -1 OR l.forward_end_date IS DISTINCT FROM ?
                      OR (l.is_delisted_in_horizon AND
                          (NOT l.is_stitched OR l.terminal_return IS NULL
                           OR NOT isfinite(l.terminal_return) OR l.terminal_return < -1
                           OR l.raw_forward_return IS NULL OR NOT isfinite(l.raw_forward_return)
                           OR l.raw_forward_return < -1 OR l.delist_date IS NULL
                           OR l.delist_date <= ? OR l.delist_date > ?
                           OR l.terminal_return_source NOT IN ('observed','policy')
                           OR l.terminal_return_source IS NULL
                           OR (l.terminal_return_source='observed'
                               AND l.return_observation_id IS NULL)
                           OR (l.terminal_return_source='policy'
                               AND l.return_observation_id IS NOT NULL)
                           OR abs(l.forward_return -
                             ((1+l.raw_forward_return)*(1+l.terminal_return)-1))
                              > 1e-10*greatest(1.0,abs(l.forward_return))))
                      OR (NOT l.is_delisted_in_horizon AND
                          (l.is_stitched OR l.terminal_return IS NOT NULL
                           OR l.delist_date IS NOT NULL OR l.terminal_return_source IS NOT NULL
                           OR l.return_observation_id IS NOT NULL
                           OR l.raw_forward_return IS NULL OR NOT isfinite(l.raw_forward_return)
                           OR l.raw_forward_return < -1
                           OR abs(l.forward_return-l.raw_forward_return)
                              > 1e-10*greatest(1.0,abs(l.forward_return))))
                      OR greatest(l.available_at,l.forward_end_date::TIMESTAMP+INTERVAL 1 DAY+INTERVAL 12 HOUR)>?
                      THEN 'invalid'
                    ELSE 'valid' END AS label_status
        FROM _fq2_rank r LEFT JOIN selected l USING(security_id)
    """, [source, entry, horizon, cutoff, LABEL_VERSION, end, entry, end, cutoff])


def _summaries(con: Any, run_id: str, signals: tuple[str, ...]) -> None:
    rows = con.execute("""
        SELECT signal_id,horizon_sessions,split,session_number,year(decision_date),
               max(mean_forward_return) FILTER (WHERE decile=10)
                 -max(mean_forward_return) FILTER (WHERE decile=1) AS spread,
               sum(eligible_count),sum(labeled_count),
               sum(observed_terminal_count),sum(policy_terminal_count)
        FROM fundamental_signal_evaluation_deciles
        WHERE run_id=? AND status='evaluated'
        GROUP BY signal_id,horizon_sessions,split,session_number,year(decision_date)
        ORDER BY signal_id,horizon_sessions,split,session_number
    """, [run_id]).fetchall()
    groups: dict[tuple[str, int, str], list[tuple[Any, ...]]] = {}
    for signal, horizon, split, *rest in rows:
        groups.setdefault((signal, horizon, split), []).append(tuple(rest))
    records: dict[tuple[str, int, str], dict[str, Any]] = {}
    for signal in signals:
        for horizon in HORIZONS:
            for split in SPLITS:
                group = groups.get((signal, horizon, split), [])
                values = [(r[0], r[2]) for r in group if r[2] is not None]
                stats = calendar_hac_statistics(values, horizon)
                yearly: dict[int, list[float]] = {}
                for _, year, spread, *_ in group:
                    if spread is not None and math.isfinite(spread):
                        yearly.setdefault(year, []).append(spread)
                annual = {str(year): {"dates": len(v), "gross_mean": math.fsum(v)/len(v)}
                          for year, v in sorted(yearly.items())}
                eligible = sum(r[3] for r in group)
                labeled = sum(r[4] for r in group)
                stats.update(annual=annual, eligible=eligible, labeled=labeled,
                             coverage=labeled/eligible if eligible else None,
                             observed=sum(r[5] for r in group), policy=sum(r[6] for r in group))
                records[(signal, horizon, split)] = stats
    for split in SPLITS:
        corrected = holm_family({signal: records[(signal, 21, split)]["p_value"]
                                 for signal in signals})
        for signal, p in corrected.items():
            records[(signal, 21, split)]["holm_p_value"] = p
    for (signal, horizon, split), record in records.items():
        mean = record["gross_mean"]
        net = [None if mean is None else mean - 4*bp/10000 for bp in (10, 25, 50)]
        annual = record["annual"]
        stable = len(annual) >= 2 and all(x["dates"] >= 60 and x["gross_mean"] > 0
                                          for x in annual.values())
        candidate = bool(horizon == 21 and record.get("holm_p_value") is not None
                         and record["holm_p_value"] <= .05 and mean is not None and mean > 0
                         and net[1] is not None and net[1] > 0 and record["spread_dates"] >= 252
                         and stable and record["coverage"] is not None and record["coverage"] >= .99)
        status = "candidate" if candidate else (
            "untestable" if record["p_value"] is None else "did_not_qualify")
        con.execute("""
            INSERT INTO fundamental_signal_evaluation_summaries VALUES
            (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """, [run_id, signal, horizon, split, horizon == 21, status,
              record["spread_dates"], mean, record["hac_lags"], record["hac_standard_error"],
              record["z_statistic"], record["p_value"], record.get("holm_p_value"),
              record["ci95_low"], record["ci95_high"], *net,
              record["eligible"], record["labeled"], record["coverage"],
              record["observed"], record["policy"],
              record["observed"]/record["labeled"] if record["labeled"] else None,
              record["policy"]/record["labeled"] if record["labeled"] else None,
              _json(annual), candidate,
              False, _json(_BLOCKERS)])


def _result_digest(con: Any, run_id: str) -> tuple[int, str]:
    """Seal every published decile and summary field in stable logical order."""
    result_rows, decile_sha = _digest_rows(con, """
        SELECT signal_id,decision_date,session_number,entry_date,expected_end_date,
               horizon_sessions,split,status,decile,cohort_count,tied_boundary_count,
               eligible_count,labeled_count,missing_count,invalid_count,
               unsupported_basis_count,observed_terminal_count,policy_terminal_count,
               mean_score,mean_forward_return
        FROM fundamental_signal_evaluation_deciles WHERE run_id=?
        ORDER BY signal_id,decision_date,horizon_sessions,decile
    """, [run_id])
    _, summary_sha = _digest_rows(con, """
        SELECT signal_id,horizon_sessions,split,primary_hypothesis,status,
               spread_dates,gross_mean,hac_lags,hac_standard_error,z_statistic,
               p_value,holm_p_value,ci95_low,ci95_high,net_10bp,net_25bp,
               net_50bp,eligible_count,labeled_count,label_coverage,
               observed_terminal_count,policy_terminal_count,
               observed_terminal_share,policy_terminal_share,annual_json,
               candidate,production_eligible,blockers_json
        FROM fundamental_signal_evaluation_summaries WHERE run_id=?
        ORDER BY signal_id,horizon_sessions,split
    """, [run_id])
    return result_rows, _sha(_json([decile_sha, summary_sha]))


def evaluate_fundamental_signals(
    store: DuckDBStore, options: FundamentalSignalEvaluationOptions,
) -> dict[str, Any]:
    """Evaluate all frozen signals in one bounded session partition at a time."""
    run_at = _validate_options(options)
    con = store.con
    con.execute("SET memory_limit = '256MB'")
    con.execute("SET threads = 1")
    con.execute("SET preserve_insertion_order = false")
    if con.execute("SELECT count(*) FROM fundamental_signal_evaluation_runs WHERE run_id=?",
                   [options.run_id]).fetchone()[0]:
        raise ValueError("evaluation run_id already exists")
    verified = validate_fundamental_signal_panel(con, options.build_run_id)
    if not 1 <= len(verified.signal_ids) <= 32:
        raise ValueError("frozen signal family exceeds 32")
    build = con.execute("""
        SELECT as_of_date,start_date,end_date,source_ids_json,blockers_json
        FROM fundamental_signal_runs WHERE run_id=?
    """, [options.build_run_id]).fetchone()
    if build is None or options.as_of_date < build[0]:
        raise ValueError("evaluation cutoff precedes completed build")
    source = json.loads(build[3])["market_source"]
    cutoff = dt.datetime.combine(options.as_of_date, dt.time(22))
    config = {"evaluation_version": "fq2_v1",
              "build_run_id": options.build_run_id, "label_source": options.label_source,
              "as_of_date": options.as_of_date.isoformat(), "label_version": LABEL_VERSION,
              "primary_horizon": 21, "secondary_horizons": [5, 63],
              "signals": list(verified.signal_ids), "cohort": "FQ1 dated US-common issuer-qualified",
              "min_names": 200, "min_per_decile": 20,
              "splits": "train<=2020; validation2021..2023; holdout>=2024; 63-session embargo; split-crossing purge",
              "cost_bp_per_side": [10, 25, 50], "calendar_source": source}
    with store.transaction():
        con.execute("""
            INSERT INTO fundamental_signal_evaluation_runs
            (run_id,build_run_id,status,as_of_date,run_at,label_source,config_json,
             config_sha256,build_sha256,calendar_sha256,blockers_json)
            VALUES (?,?,'building',?,?,?,?,?,?,?,?)
        """, [options.run_id, options.build_run_id, options.as_of_date, run_at,
              options.label_source, _json(config), _sha(_json(config)), verified.panel_sha256,
              verified.calendar_sha256, _json(_BLOCKERS)])
    try:
        _calendar(con, source, options.as_of_date, cutoff)
        _, evaluation_calendar_sha = _digest_rows(con, """
            SELECT trade_date,session_number FROM _fq2_calendar ORDER BY session_number
        """, [])
        sessions = verified.sessions
        for day, entry in sessions:
            for signal in verified.signal_ids:
                n, constant, ties = _rank_day(con, options.build_run_id, signal, day)
                for horizon in HORIZONS:
                    session, end, status = _date_status(con, day, entry, horizon, n,
                                                        constant, cutoff)
                    if status == "evaluated":
                        _join_labels(con, options.label_source, entry, horizon, cutoff, end)
                        selected_count, selected_sha = _digest_rows(con, """
                            SELECT security_id,forward_return_id,price_basis,calculation_version,
                                   raw_forward_return,terminal_return,forward_return,
                                   forward_end_date,delist_date,terminal_return_source,
                                   return_observation_id,is_delisted_in_horizon,available_at
                            FROM _fq2_join WHERE forward_return_id IS NOT NULL ORDER BY security_id
                        """, [])
                        with store.transaction():
                            con.execute("""
                                INSERT INTO fundamental_signal_evaluation_deciles
                                SELECT ?,?, ?,?,?,?, ?,?, ?,decile,?, ?,count(*),
                                       count(*) FILTER (WHERE label_status='valid'),
                                       count(*) FILTER (WHERE label_status='missing'),
                                       count(*) FILTER (WHERE label_status='invalid'),
                                       count(*) FILTER (WHERE label_status='unsupported_basis'),
                                       count(*) FILTER (WHERE label_status='valid' AND terminal_return_source='observed'),
                                       count(*) FILTER (WHERE label_status='valid' AND terminal_return_source='policy'),
                                       avg(score),avg(forward_return) FILTER (WHERE label_status='valid')
                                FROM _fq2_join GROUP BY decile
                            """, [options.run_id, signal, day, session, entry, end, horizon,
                                  _split(day), status, n, ties])
                            con.execute("""
                                INSERT INTO fundamental_signal_evaluation_label_evidence
                                VALUES (?,?,?,?,?,?)
                            """, [options.run_id, signal, day, horizon,
                                  selected_count, selected_sha])
                    else:
                        with store.transaction():
                            if n:
                                con.execute("""
                                    INSERT INTO fundamental_signal_evaluation_deciles
                                    SELECT ?,?,?,?,?,?,?,?,?,decile,?,?,count(*),
                                           0,0,0,0,0,0,avg(score),NULL
                                    FROM _fq2_rank GROUP BY decile
                                """, [options.run_id, signal, day, session, entry, end,
                                      horizon, _split(day), status, n, ties])
                            else:
                                con.execute("""
                                    INSERT INTO fundamental_signal_evaluation_deciles
                                    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
                                """, [options.run_id, signal, day, session, entry, end,
                                      horizon, _split(day), status, 0, n, ties,
                                      0, 0, 0, 0, 0, 0, 0, None, None])
                            con.execute("""
                                INSERT INTO fundamental_signal_evaluation_label_evidence
                                VALUES (?,?,?,?,0,?)
                            """, [options.run_id, signal, day, horizon,
                                  hashlib.sha256().hexdigest()])
        _summaries(con, options.run_id, verified.signal_ids)
        _, sample_sha = _digest_rows(con, """
            SELECT signal_id,decision_date,horizon_sessions,selected_rows,selected_sha256
            FROM fundamental_signal_evaluation_label_evidence WHERE run_id=?
            ORDER BY signal_id,decision_date,horizon_sessions
        """, [options.run_id])
        result_rows, result_sha = _result_digest(con, options.run_id)
        label_rows = con.execute("""
            SELECT coalesce(sum(selected_rows),0) FROM fundamental_signal_evaluation_label_evidence
            WHERE run_id=?
        """, [options.run_id]).fetchone()[0]
        diagnostic = {"sessions": len(sessions), "signals": len(verified.signal_ids),
                      "decile_rows": result_rows, "label_rows": label_rows,
                      "summary_rows": len(verified.signal_ids)*len(HORIZONS)*len(SPLITS)}
        with store.transaction():
            con.execute("""
                UPDATE fundamental_signal_evaluation_runs
                SET status='complete',evaluation_calendar_sha256=?,sample_sha256=?,
                    result_sha256=?,label_rows=?,diagnostic_json=?
                WHERE run_id=? AND status='building'
            """, [evaluation_calendar_sha, sample_sha, result_sha, label_rows,
                  _json(diagnostic), options.run_id])
        return diagnostic
    except Exception as exc:
        with store.transaction():
            con.execute("""
                UPDATE fundamental_signal_evaluation_runs
                SET status='failed',diagnostic_json=? WHERE run_id=? AND status='building'
            """, [_json({"error_type": type(exc).__name__}), options.run_id])
        raise
    finally:
        for table in ("_fq2_join", "_fq2_rank", "_fq2_calendar"):
            con.execute(f"DROP TABLE IF EXISTS {table}")
