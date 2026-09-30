"""Stage ``borrow_proxy``: short-constraint PROXY inputs per (session, security) -> ``borrow_proxy/``.

THIS IS A PROXY. atx-db holds no securities-lending data: no borrow fee, no utilization, no lendable
quantity. The stage lines up public short-side inputs that a consumer can combine into its own
hard-to-borrow screen, each with its own clock:

| column group | source stage | value | clock |
| --- | --- | --- | --- |
| ``si_*`` | ``short_interest/si.parquet`` | latest visible FINRA short interest | ``si_available_at`` = dissemination date + 1 day 00:00 UTC |
| ``inst_*`` | ``thirteenf/agg_asof45.parquet`` | latest visible 13F institutional shares (as-of-45-days version) | the aggregate's ``available_at`` |
| ``si_to_io`` | both | ``si_shares / inst_shares`` when both are within their staleness rules | ``greatest`` of both clocks |
| ``ftd_*`` | ``ftd/`` | fails on the latest published settlement date (0 when the security is absent from that file) and the latest non-zero fail | the file's ``available_at`` |
| ``threshold_*``, ``on_threshold_list`` | ``regsho_threshold/`` | on any listing market's latest visible Reg SHO threshold list | the list's ``available_at`` |

Grid: every ``(session_date, security_id)`` row of the prices stage (2018-01-02 onward); ``member_equity`` is
joined from the panel stage where the panel has the cell (NULL otherwise) and is used only for coverage. A value enters the row
of decision session ``d`` only if its ``available_at`` is strictly before 22:00 UTC of session ``d-1``
(``cutoff_utc``), the ground rule of the data request. "Latest visible" is taken over settlement / period /
list order among visible values (a late-posted older file never hides a newer visible one).

``si_available_at``: FINRA publishes on the dissemination date after the close; no time-of-day evidence is
held, so the stage uses the dissemination date + 1 day at 00:00 UTC, which is one session more conservative
than the stage-S contract ("visible on sessions strictly after the dissemination date"). ``si_vintage_risk``
repeats stage S: settlements up to 2021-05-28 are FINRA's later republication.

Staleness (the consumer's NaN rules, applied here only inside ``si_to_io``): SI 45 days after settlement,
13F 150 days after period end, FTD 60 days after the latest published settlement date, threshold lists 10
days after the latest visible list date.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any

from . import common as C
from . import shortflow_common as S

STAGE = "borrow_proxy"
SCHEMA = "atx.alpha-panel.borrow-proxy/v1"
LABEL = "borrow-proxy-v1: public short-side inputs only; NOT a borrow fee, utilization or lendable quantity"
SI_STALE_DAYS = 45
INST_STALE_DAYS = 150
FTD_STALE_DAYS = 60
THRESHOLD_STALE_DAYS = 10
SI_REPUBLICATION_LAST = "2021-05-28"
MODULES = ("borrow_proxy", "shortflow_common", "common", "finra_fetch")


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"label": LABEL}
    root = C.build_root()
    # file-backed, one thread: the per-year ASOF chain over the full SI / 13F / FTD runs overflowed 381 MiB in memory
    con = S.connect("borrow", memory="560MB", threads=1, db_file="borrow.duckdb")
    tmp = S.tmp_dir("borrow")
    si = (root / "short_interest" / "si.parquet").as_posix()
    agg = (root / "thirteenf" / "agg_asof45.parquet").as_posix()
    ftd_glob = (root / "ftd" / "year=*" / "ftd.parquet").as_posix()
    th_glob = (root / "regsho_threshold" / "year=*" / "threshold.parquet").as_posix()
    lists = (root / "regsho_threshold" / "lists.parquet").as_posix()
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    prices = (root / "prices" / "year=*" / "prices.parquet").as_posix()
    cal = C.calendar_path().as_posix()
    inputs = {k: C.read_json(root / k / "manifest.json") for k in ("short_interest", "thirteenf", "ftd", "regsho_threshold",
                                                                    "prices", "panel")
              if (root / k / "manifest.json").exists()}
    receipt["input_manifests"] = {k: {"sha256": C.sha256_file(root / k / "manifest.json"), "status": v.get("status")}
                                  for k, v in inputs.items()}
    # sessions with cutoff = 22:00 UTC of the previous session
    con.execute(f"""CREATE OR REPLACE TEMP TABLE sess AS
        SELECT session_date, (CAST(prev AS TIMESTAMP) + INTERVAL 22 HOUR) AT TIME ZONE 'UTC' AS cutoff_utc
        FROM (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev FROM read_parquet('{cal}'))
        WHERE prev IS NOT NULL AND session_date >= DATE '2018-01-02'""")
    # SI with running max over settlement order, keyed by availability
    C.copy_to_parquet(con, f"""
        SELECT security_id, settlement_date, si_shares,
               (CAST(dissemination_date AS TIMESTAMP) + INTERVAL 24 HOUR) AT TIME ZONE 'UTC' AS si_available_at,
               settlement_date <= DATE '{SI_REPUBLICATION_LAST}' AS si_vintage_risk
        FROM read_parquet('{si}') WHERE security_id IS NOT NULL""", tmp / "si.parquet")
    C.copy_to_parquet(con, f"""
        SELECT security_id, period_of_report AS inst_period_of_report, inst_shares, n_holders AS inst_n_holders,
               available_at AS inst_available_at
        FROM read_parquet('{agg}') WHERE available_at IS NOT NULL""", tmp / "inst.parquet")
    # FTD: per (id, file) the latest settlement row; the global latest published settlement per file
    C.copy_to_parquet(con, f"""
        SELECT security_id, settlement_date, sum(quantity) AS quantity, max(available_at) AS available_at
        FROM read_parquet('{ftd_glob}', hive_partitioning = false) WHERE security_id IS NOT NULL
        GROUP BY 1, 2""", tmp / "ftd_id_day.parquet")
    C.copy_to_parquet(con, f"""
        SELECT source_file, max(settlement_date) AS last_settlement, min(available_at) AS available_at
        FROM read_parquet('{ftd_glob}', hive_partitioning = false) GROUP BY 1""", tmp / "ftd_files.parquet")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE ftd_pub AS
        SELECT s.session_date, max(f.last_settlement) AS ftd_settlement_date
        FROM sess s JOIN read_parquet('{(tmp / 'ftd_files.parquet').as_posix()}') f ON f.available_at < s.cutoff_utc
        GROUP BY 1""")
    # threshold: per session and market the latest visible list; ids on those lists
    con.execute(f"""CREATE OR REPLACE TEMP TABLE vis_lists AS
        SELECT s.session_date, l.market, max(l.list_date) AS list_date
        FROM sess s JOIN read_parquet('{lists}') l ON l.available_at < s.cutoff_utc AND l.status IN ('list', 'empty_list')
        GROUP BY 1, 2""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE vis_lists2 AS
        SELECT v.*, l.available_at FROM vis_lists v JOIN read_parquet('{lists}') l USING (market, list_date)""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE th_on AS
        SELECT v.session_date, t.security_id, string_agg(DISTINCT t.market, ',' ORDER BY t.market) AS threshold_markets,
               max(t.run_days) AS threshold_run_days
        FROM vis_lists2 v JOIN read_parquet('{th_glob}', hive_partitioning = false) t
          ON t.market = v.market AND t.list_date = v.list_date
        WHERE t.on_list AND t.security_id IS NOT NULL
        GROUP BY 1, 2""")
    con.execute("""CREATE OR REPLACE TEMP TABLE th_sess AS
        SELECT session_date, max(list_date) AS threshold_list_date, max(available_at) AS threshold_available_at,
               count(DISTINCT market) AS threshold_markets_visible
        FROM vis_lists2 GROUP BY 1""")
    out = C.stage_dir(STAGE)
    # resumable per year: a year file written after the current panel manifest is reused (the guard can stop this
    # build for host headroom; each input here is at least as old as the panel manifest)
    panel_mtime = (root / "panel" / "manifest.json").stat().st_mtime
    for f in out.glob("year=*/borrow_proxy.parquet"):
        if f.stat().st_mtime <= panel_mtime:
            f.unlink()
    years = [r[0] for r in con.execute("SELECT DISTINCT year(session_date) FROM sess ORDER BY 1").fetchall()]
    per_year: dict[str, Any] = {}
    for y in years:
        ty = time.perf_counter()
        sql = f"""
            WITH g AS (
                SELECT p.session_date, p.security_id, pn.member_equity, s.cutoff_utc
                FROM read_parquet('{prices}', hive_partitioning = false) p JOIN sess s USING (session_date)
                LEFT JOIN (SELECT session_date, security_id, member_equity
                           FROM read_parquet('{panel}', hive_partitioning = false) WHERE year(session_date) = {y}) pn
                  USING (session_date, security_id)
                WHERE year(p.session_date) = {y}
            ),
            si_run AS (
                SELECT *, max(settlement_date) OVER (PARTITION BY security_id ORDER BY si_available_at, settlement_date
                          ROWS UNBOUNDED PRECEDING) AS run_settle
                FROM read_parquet('{(tmp / 'si.parquet').as_posix()}')
            ),
            g1 AS (SELECT g.*, si_run.run_settle FROM g ASOF LEFT JOIN si_run
                   ON si_run.security_id = g.security_id AND g.cutoff_utc > si_run.si_available_at),
            g2 AS (SELECT g1.*, si.si_shares, si.si_available_at, si.si_vintage_risk
                   FROM g1 LEFT JOIN read_parquet('{(tmp / 'si.parquet').as_posix()}') si
                     ON si.security_id = g1.security_id AND si.settlement_date = g1.run_settle),
            inst_run AS (
                SELECT *, max(inst_period_of_report) OVER (PARTITION BY security_id ORDER BY inst_available_at, inst_period_of_report
                          ROWS UNBOUNDED PRECEDING) AS run_period
                FROM read_parquet('{(tmp / 'inst.parquet').as_posix()}')
            ),
            g3 AS (SELECT g2.*, inst_run.run_period FROM g2 ASOF LEFT JOIN inst_run
                   ON inst_run.security_id = g2.security_id AND g2.cutoff_utc > inst_run.inst_available_at),
            g4 AS (SELECT g3.*, i.inst_shares, i.inst_n_holders, i.inst_available_at
                   FROM g3 LEFT JOIN read_parquet('{(tmp / 'inst.parquet').as_posix()}') i
                     ON i.security_id = g3.security_id AND i.inst_period_of_report = g3.run_period),
            ftd_run AS (   -- one key per (id, available_at): the rows of one file share its clock
                SELECT security_id, available_at, max(max_settle) OVER (PARTITION BY security_id ORDER BY available_at
                          ROWS UNBOUNDED PRECEDING) AS run_settle_f
                FROM (SELECT security_id, available_at, max(settlement_date) AS max_settle
                      FROM read_parquet('{(tmp / 'ftd_id_day.parquet').as_posix()}') GROUP BY 1, 2)
            ),
            g5 AS (SELECT g4.*, ftd_run.run_settle_f FROM g4 ASOF LEFT JOIN ftd_run
                   ON ftd_run.security_id = g4.security_id AND g4.cutoff_utc > ftd_run.available_at),
            fd AS (SELECT * FROM read_parquet('{(tmp / 'ftd_id_day.parquet').as_posix()}'))
            SELECT g5.session_date, g5.security_id, g5.cutoff_utc, g5.member_equity,
                   g5.si_shares, g5.run_settle AS si_settlement_date, g5.si_available_at, g5.si_vintage_risk,
                   g5.inst_shares, g5.inst_n_holders, g5.run_period AS inst_period_of_report, g5.inst_available_at,
                   CASE WHEN g5.si_shares IS NOT NULL AND g5.inst_shares > 0
                             AND date_diff('day', g5.run_settle, g5.session_date) <= {SI_STALE_DAYS}
                             AND date_diff('day', g5.run_period, g5.session_date) <= {INST_STALE_DAYS}
                        THEN g5.si_shares / g5.inst_shares END AS si_to_io,
                   CASE WHEN g5.si_shares IS NOT NULL AND g5.inst_shares > 0
                        THEN greatest(g5.si_available_at, g5.inst_available_at) END AS si_to_io_available_at,
                   fp.ftd_settlement_date,
                   CASE WHEN fp.ftd_settlement_date IS NOT NULL THEN coalesce(fq.quantity, 0) END AS ftd_quantity,
                   ff.available_at AS ftd_available_at,
                   g5.run_settle_f AS ftd_last_nonzero_date, fl.quantity AS ftd_last_nonzero_quantity,
                   fl.available_at AS ftd_last_nonzero_available_at,
                   CASE WHEN ts.threshold_list_date IS NOT NULL THEN t.security_id IS NOT NULL END AS on_threshold_list,
                   ts.threshold_list_date, ts.threshold_available_at, ts.threshold_markets_visible,
                   t.threshold_markets, t.threshold_run_days
            FROM g5
            LEFT JOIN ftd_pub fp ON fp.session_date = g5.session_date
            LEFT JOIN fd fq ON fq.security_id = g5.security_id AND fq.settlement_date = fp.ftd_settlement_date
            LEFT JOIN (SELECT last_settlement, min(available_at) AS available_at
                       FROM read_parquet('{(tmp / 'ftd_files.parquet').as_posix()}') GROUP BY 1) ff
              ON ff.last_settlement = fp.ftd_settlement_date
            LEFT JOIN fd fl ON fl.security_id = g5.security_id AND fl.settlement_date = g5.run_settle_f
            LEFT JOIN th_sess ts ON ts.session_date = g5.session_date
            LEFT JOIN th_on t ON t.session_date = g5.session_date AND t.security_id = g5.security_id
            ORDER BY g5.session_date, g5.security_id
        """
        dest = out / f"year={y}" / "borrow_proxy.parquet"
        if dest.exists():
            n = con.execute(f"SELECT count(*) FROM read_parquet('{dest.as_posix()}')").fetchone()[0]
        else:
            n = C.copy_to_parquet(con, sql, dest)
        d = dest.as_posix()
        r = con.execute(f"""
            SELECT count(*) FILTER (WHERE member_equity),
                   count(*) FILTER (WHERE member_equity AND si_shares IS NOT NULL AND date_diff('day', si_settlement_date, session_date) <= {SI_STALE_DAYS}),
                   count(*) FILTER (WHERE member_equity AND inst_shares IS NOT NULL AND date_diff('day', inst_period_of_report, session_date) <= {INST_STALE_DAYS}),
                   count(*) FILTER (WHERE member_equity AND si_to_io IS NOT NULL),
                   count(*) FILTER (WHERE member_equity AND ftd_quantity IS NOT NULL AND date_diff('day', ftd_settlement_date, session_date) <= {FTD_STALE_DAYS}),
                   count(*) FILTER (WHERE member_equity AND ftd_quantity > 0),
                   count(*) FILTER (WHERE member_equity AND on_threshold_list IS NOT NULL AND date_diff('day', threshold_list_date, session_date) <= {THRESHOLD_STALE_DAYS}),
                   count(*) FILTER (WHERE member_equity AND on_threshold_list),
                   quantile_cont(si_to_io, [0.01, 0.5, 0.99]) FILTER (WHERE member_equity),
                   count(*) FILTER (WHERE member_equity AND si_to_io > 1)
            FROM read_parquet('{d}')""").fetchone()
        me = r[0] or 1
        per_year[str(y)] = {"rows": n, "member_equity_cells": r[0],
                            "share_si_fresh": round(r[1] / me, 4), "share_inst_fresh": round(r[2] / me, 4),
                            "share_si_to_io": round(r[3] / me, 4), "share_ftd_fresh": round(r[4] / me, 4),
                            "share_ftd_positive": round(r[5] / me, 4), "share_threshold_fresh": round(r[6] / me, 4),
                            "share_on_threshold_list": round(r[7] / me, 5),
                            "si_to_io_p01_p50_p99": [round(x, 4) for x in r[8]] if r[8] else None,
                            "si_to_io_gt_1_cells": r[9], "elapsed_s": round(time.perf_counter() - ty, 1)}
        print(f"borrow_proxy {y}: {per_year[str(y)]}", flush=True)
    receipt["per_year"] = per_year
    receipt["timings_s"] = {"total": round(time.perf_counter() - t0, 1)}
    payload = {
        "label": LABEL,
        "rule_text": ("grid = prices (session_date, security_id) rows from 2018-01-02; each input is the latest (by settlement / "
                      "period / list date) value with available_at < 22:00 UTC of the previous session (cutoff_utc)"),
        "clocks": {"si": "dissemination_date + 1 day 00:00 UTC (conservative; stage-S contract is one session earlier)",
                   "inst": "thirteenf agg_asof45.available_at (filed + 46 h, latest needed filing)",
                   "ftd": "ftd.available_at of the file (nominal + 7 d or a later plausible Last-Modified)",
                   "threshold": "regsho_threshold lists.available_at (per market rule)"},
        "staleness_rule": {"si_days": SI_STALE_DAYS, "inst_days": INST_STALE_DAYS, "ftd_days": FTD_STALE_DAYS,
                           "threshold_days": THRESHOLD_STALE_DAYS,
                           "text": "consumer treats an input as NaN beyond these ages; si_to_io is NULL unless SI and 13F are both fresh"},
        "not_included": "borrow fee, utilization, lendable supply, rebate rate: no source in atx-db (D4 licensed data needed)",
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args(argv)
    rec = build()
    print(json.dumps(rec.get("per_year"), default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
