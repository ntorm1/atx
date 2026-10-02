"""Stage ``market_shares`` (S3.4): daily shares outstanding per line, the CRSP ``shrout`` analog, point in time.

Output ``market/shares_daily/year=YYYY/shares_daily.parquet``, one row per vendor line-session (2018-01-02 on):

``security_id, session_date, cik, shrout, shrout_source, shrout_asof, obs_available_at, available_at, split_adj,
shares_sec, shares_sec_source, shares_sec_asof, shares_vendor_pit, shares_vendor_asof, shares_vendor_matched,
shares_vendor_current, issuer_equity_lines, shares_sec_line_level, shrout_conflict``.

Rule ``shrout-pit-v1``. Two candidate observations per line-session d, each split-adjusted to d by the product of
the vendor split / reverse-split ratios (``corporate_actions`` kinds ``split``/``reverse_split``) with ex-date in
(as-of, d]:

* ``shares_sec`` (issuer level): the fundamentals stage ``shrs_q`` of the latest filing event of the line's CIK with
  ``clock_utc`` < 22:00 UTC of session d-1 (``shrs_src``: dei cover count, else balance-sheet shares, weighted
  average, class sums), as-of the filing date (share counts reported after a split are restated for it; a dei cover
  date between a split and the filing is the documented residual), stale after ``SEC_STALE_DAYS``. The line's CIK
  comes from ``identity/link_table.parquet`` once the link is known (strict / name: ``available_at``; backfill:
  ``evidence_at``; the panel's rule).
* ``shares_vendor_pit``: the vendor series (TickerHistory3 ``shares``, thousands) is the dei cover count applied
  from the cover date or the period end, before the filing (AAPL 2020-01-17 for the 10-Q filed 2020-01-28; XOM
  quarter-ends for 10-Qs filed ~35 days later), so it is not point in time. A vendor change event on session c (a
  new value that is not the vendor's own split adjustment) is visible from the clock of the first SEC filing of the
  line's CIK, filed from c - 3 days on, whose ``shrs_q`` equals it to the vendor's thousand-share rounding
  (``vendor_sec_matched``), and never before c 22:00 UTC; an unmatched event is visible from c + ``VENDOR_LAG_DAYS``
  (measured: vendor change -> matching filing lag p50 6-7 d, p95 49-56 d, p99 115-124 d on 2019 / 2021 / 2023). At d
  the visible event with the latest c is used.

``shares_sec_line_level``: the SEC count measures the line itself: a dei cover count (one class: a multi-class
cover count is dimensional and absent from Company Facts, whose fallbacks ``cso`` / ``waso`` / ``cls_*`` are issuer
totals or period averages), the CIK has exactly one linked equity line that session (``issuer_equity_lines``;
lines whose modal FINRA type is preferred, warrant, unit, right, note, ETF, ETN or fund do not count; an unlisted
second class is invisible here, hence the dei-only rule), and the line is not an ADR (the cover counts ordinary
shares). ``shrout`` = ``shares_sec`` when line-level, not older than the vendor observation, and within a factor
``CONFLICT_RATIO`` of it when both exist; otherwise ``shares_vendor_pit`` (per class, the only per-line source for
multi-class issuers and ADRs). ``shrout_conflict`` marks the fallback on disagreement. Values outside [1e4, 5e10] are dropped. ``shrout_source`` is ``sec_<shrs_src>``
or ``vendor_sec_matched`` / ``vendor_lag``; ``shrout_asof`` the observation's as-of date; ``obs_available_at`` its
clock; ``available_at`` = max(obs clock, 22:00 UTC of the latest split ex-date applied). ``shares_vendor_current``
is the same-session vendor value (not point in time; the CRSP-style backfilled count) for comparison only.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from . import common as C
from . import market_common as M

STAGE = "market_shares"
SCHEMA = "atx.alpha-panel.market-shares/v1"
RULE = "shrout-pit-v1"
MODULES = ("shares_daily", "market_common", "common")
OUT_DIR = "market"
VENDOR_LAG_DAYS = 120
SEC_STALE_DAYS = 400
MATCH_BEFORE_DAYS = 3
MATCH_AFTER_DAYS = 200
CONFLICT_RATIO = 1.5
DOMAIN = (1e4, 5e10)
NON_EQUITY_TYPES = ("preferred", "warrant", "unit", "right", "note", "ETF", "ETN", "fund")
INPUTS = ("prices", "corporate_actions", "identity/link_table_manifest.json", "fundamentals", "security_master")

LAKE_STAGES = [
    {"name": "market_shares", "lane": "MKT", "schema": "atx.alpha-panel.market-shares/v1",
     "module": "atx_db.alpha_panel.shares_daily", "args": ["build"],
     "inputs": ["prices", "corporate_actions", "identity_table", "fundamentals", "security_master"],
     "manifest": "market/market_shares_manifest.json",
     "outputs": [{"glob": "market/shares_daily/year=*/shares_daily.parquet", "view": "market_shares_daily"}],
     "staleness": "daily; rebuilt with prices", "vintage": "daily", "guard_gb": 0.4},
]


def known_link_sql() -> str:
    """Link runs with the time they became usable (panel rule) and the first session they may be used."""
    lt = (C.build_root() / "identity" / "link_table.parquet").as_posix()
    kn = "CASE WHEN link_tier = 'backfill' THEN evidence_at ELSE available_at END"
    first = (f"CAST(({kn}) - INTERVAL 22 HOUR AS DATE) + CASE WHEN ({kn}) - INTERVAL 22 HOUR > "
             f"CAST(CAST(({kn}) - INTERVAL 22 HOUR AS DATE) AS TIMESTAMP) THEN 1 ELSE 0 END")
    return (f"(SELECT security_id, cik, valid_from, valid_to, link_tier, {kn} AS known_at, "
            f"greatest(valid_from, {first}) AS eff_from FROM read_parquet('{lt}'))")


def prepare(con, receipt: dict[str, Any]) -> None:
    """Issuer equity-line counts (piecewise constant per CIK) and SEC share events."""
    fn = (C.build_root() / "security_master" / "finra_names.parquet").as_posix()
    ev = (C.build_root() / "fundamentals" / "events.parquet").as_posix()
    bad = ", ".join(f"'{t}'" for t in NON_EQUITY_TYPES)
    con.execute(f"""
        CREATE OR REPLACE TABLE line_type AS
        SELECT security_id, mode(finra_type) AS line_type FROM read_parquet('{fn}') GROUP BY 1
    """)
    con.execute(f"CREATE OR REPLACE TABLE links AS SELECT * FROM {known_link_sql()}")
    con.execute(f"""
        CREATE OR REPLACE TABLE runs AS
        SELECT r.* FROM links r LEFT JOIN line_type t USING (security_id)
        WHERE coalesce(t.line_type NOT IN ({bad}), true) AND r.eff_from <= r.valid_to
    """)
    con.execute("""
        CREATE OR REPLACE TABLE eqcnt AS
        SELECT cik, d AS change_date,
               CAST(sum(delta) OVER (PARTITION BY cik ORDER BY d ROWS UNBOUNDED PRECEDING) AS INTEGER) AS n
        FROM (SELECT cik, d, sum(delta) AS delta FROM (
                SELECT cik, eff_from AS d, 1 AS delta FROM runs
                UNION ALL SELECT cik, valid_to + 1 AS d, -1 AS delta FROM runs) GROUP BY 1, 2)
    """)
    con.execute(f"""
        CREATE OR REPLACE TABLE sec AS
        SELECT cik, clock_utc, filed, shrs_q, shrs_src FROM read_parquet('{ev}')
        WHERE shrs_q > 0 AND clock_utc IS NOT NULL AND cik IS NOT NULL
    """)
    receipt["prepare"] = {"equity_link_runs": con.execute("SELECT count(*) FROM runs").fetchone()[0],
                          "sec_share_events": con.execute("SELECT count(*) FROM sec").fetchone()[0]}


def bucket_sql(b: int, k: int | None = None) -> str:
    lo, hi = DOMAIN
    return f"""
    WITH ld AS (
        SELECT security_id, session_date, prev_session, shares_vendor,
               CAST(prev_session AS TIMESTAMP) + {M.MARK} AS prev_mark
        FROM read_parquet('{M.bucket_path(b)}') WHERE {M.sub_filter(k)}
    ),
    spl AS (SELECT * FROM {M.split_events_sql()} WHERE security_id % {M.BUCKETS} = {b}),
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
        LEFT JOIN (SELECT DISTINCT security_id, cik, valid_from, valid_to FROM links
                   WHERE security_id % {M.BUCKETS} = {b}) r
          ON r.security_id = v.security_id AND v.c BETWEEN r.valid_from AND r.valid_to
    ),
    vmatch AS (
        SELECT v.security_id, v.c, any_value(v.s) AS s, any_value(v.csf_c) AS csf_c, min(e.clock_utc) AS match_clock
        FROM vcik v LEFT JOIN sec e
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
        LEFT JOIN (SELECT * FROM links WHERE security_id % {M.BUCKETS} = {b}) r
          ON r.security_id = cs.security_id AND cs.session_date BETWEEN r.valid_from AND r.valid_to
         AND r.known_at <= CAST(cs.session_date AS TIMESTAMP) + {M.MARK}
        QUALIFY row_number() OVER (PARTITION BY cs.security_id, cs.session_date ORDER BY r.known_at) = 1
    ),
    fs AS (
        SELECT lk.*, e.shrs_q, e.shrs_src, e.filed, e.clock_utc AS sec_clock
        FROM lk ASOF LEFT JOIN sec e ON lk.cik = e.cik AND lk.prev_mark > e.clock_utc
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
        FROM va ASOF LEFT JOIN eqcnt n ON va.cik = n.cik AND va.session_date >= n.change_date
        LEFT JOIN line_type lt ON lt.security_id = va.security_id
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


def build(buckets: list[int] | None = None) -> dict[str, Any]:
    receipt: dict[str, Any] = {"rule": RULE}
    con = C.connect(memory="240MB", threads=2, db_file="mkt_shares.duckdb")
    M.project_prices(con, receipt)
    prepare(con, receipt)
    tmp = M.resumable_dir("shares", M.input_manifests(*INPUTS), MODULES) if buckets is None else M.tmp("shares")
    work = [(b, None) for b in buckets] if buckets is not None else M.units()
    for b, k in work:
        name = f"bucket={b:02d}" + ("" if k is None else f"_{k}")
        if buckets is None and (tmp / f"{name}.parquet").exists():
            continue  # resumed after a guard stop
        with C.timed(receipt, name):
            n = C.copy_to_parquet(con, f"""
                SELECT * EXCLUDE (last_split, obs_available_at), obs_available_at,
                       CASE WHEN last_split IS NOT NULL AND last_split > shrout_asof
                            THEN greatest(obs_available_at, CAST(last_split AS TIMESTAMP) + {M.MARK})
                            ELSE obs_available_at END AS available_at
                FROM ({bucket_sql(b, k)})""", tmp / f"{name}.parquet")
        print(f"{name}: {n} rows {receipt['timings_s'][name]} s", flush=True)
    if buckets is not None:
        return receipt
    out = C.stage_dir(OUT_DIR) / "shares_daily"
    glob = (tmp / "bucket=*.parquet").as_posix()
    years = {}
    lo, hi = con.execute(f"SELECT min(session_date), max(session_date) FROM read_parquet('{glob}')").fetchone()
    for y in range(lo.year, hi.year + 1):
        years[str(y)] = C.copy_to_parquet(con, f"""
            SELECT * FROM read_parquet('{glob}') WHERE year(session_date) = {y} ORDER BY session_date, security_id
        """, out / f"year={y}" / "shares_daily.parquet")
    receipt["years"] = years
    receipt["sources"] = [list(map(str, r)) for r in con.execute(f"""
        SELECT year(session_date), shrout_source, count(*) FROM read_parquet('{glob}') GROUP BY ALL ORDER BY ALL
    """).fetchall()]
    M.publish_part(OUT_DIR, STAGE, SCHEMA, MODULES, {"rule": RULE, "rule_text": __doc__, "receipt": receipt,
                                                    "staleness": f"sec {SEC_STALE_DAYS} d; vendor carried"},
                   M.input_manifests(*INPUTS), pattern="shares_daily/**/*.parquet")
    return receipt


def measure() -> dict[str, Any]:
    """Done criteria on panel member_equity cells: shrout coverage; SEC vs vendor agreement within 5%."""
    con = C.connect(memory="240MB", threads=2)
    glob = (C.build_root() / OUT_DIR / "shares_daily" / "year={y}" / "shares_daily.parquet").as_posix()
    both_cur = "t.shares_sec > 0 AND t.shares_vendor_current > 0 AND t.shares_sec_line_level"
    both_pit = "t.shares_sec > 0 AND t.shares_vendor_pit > 0 AND t.shares_sec_line_level"
    both_any = "t.shares_sec > 0 AND t.shares_vendor_current > 0 AND t.issuer_equity_lines = 1"
    res = M.member_coverage(con, glob, {
        "shrout": "t.shrout > 0", "shares_sec": "t.shares_sec > 0", "shares_vendor_pit": "t.shares_vendor_pit > 0",
        "shares_vendor_current": "t.shares_vendor_current > 0", "source_sec": "t.shrout_source LIKE 'sec_%'",
        "conflict": "coalesce(t.shrout_conflict, false)"}, extra={
        "both_current_cells": f"count(*) FILTER (WHERE {both_cur})",
        "agree5_current": f"avg(CASE WHEN abs(t.shares_sec / t.shares_vendor_current - 1) <= 0.05 THEN 1.0 ELSE 0.0 END)"
                          f" FILTER (WHERE {both_cur})",
        "both_pit_cells": f"count(*) FILTER (WHERE {both_pit})",
        "both_any_source_cells": f"count(*) FILTER (WHERE {both_any})",
        "agree5_any_source": f"avg(CASE WHEN abs(t.shares_sec / t.shares_vendor_current - 1) <= 0.05 THEN 1.0 ELSE 0.0 END)"
                             f" FILTER (WHERE {both_any})",
        "agree5_pit": f"avg(CASE WHEN abs(t.shares_sec / t.shares_vendor_pit - 1) <= 0.05 THEN 1.0 ELSE 0.0 END)"
                      f" FILTER (WHERE {both_pit})"})
    tot = sum(v["cells"] for v in res.values())
    summary = {"rule": RULE, "per_year": res,
               "shrout_2019_2026": sum(v["cells"] * v["shrout"] for v in res.values()) / tot,
               "agree5_current_2019_2026": sum(v["both_current_cells"] * (v["agree5_current"] or 0) for v in res.values())
               / max(sum(v["both_current_cells"] for v in res.values()), 1),
               "agree5_pit_2019_2026": sum(v["both_pit_cells"] * (v["agree5_pit"] or 0) for v in res.values())
               / max(sum(v["both_pit_cells"] for v in res.values()), 1),
               "agree5_any_source_2019_2026": sum(v["both_any_source_cells"] * (v["agree5_any_source"] or 0)
                                                  for v in res.values())
               / max(sum(v["both_any_source_cells"] for v in res.values()), 1),
               "gate": {"shrout_min": 0.99, "agree5_min": 0.95,
                        "agree_basis": "line-level cells (dei cover count, one equity line, not ADR) where both the "
                                       "SEC and the vendor count exist; any_source adds issuer totals for reference"},
               "input_manifests_sha256": M.input_manifests("panel", "market/market_shares_manifest.json")}
    C.write_json_atomic(C.stage_dir("validation") / "shares_coverage.json", summary)
    return summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build", "measure", "all"), default="build")
    ap.add_argument("--buckets", default=None, help="comma list: build only these buckets (smoke)")
    args = ap.parse_args(argv)
    if args.step in ("build", "all"):
        bl = [int(x) for x in args.buckets.split(",")] if args.buckets else None
        rec = build(bl)
        print(json.dumps({k: v for k, v in rec.items() if k != "timings_s"}, default=str)[:3000], flush=True)
    if args.step in ("measure", "all"):
        print(json.dumps(measure(), default=str)[:5000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
