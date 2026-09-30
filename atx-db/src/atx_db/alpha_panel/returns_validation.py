"""S3.2 (internal part): total-return identity of the prices stage on a stratified line-month sample ->
``validation/returns.json``.

Sample (rule ``returns-identity-v1``): line-months 2018-02 .. 2022-12 (ruling D6: no 2023+ returns) of lines that
are ``member_equity`` on the first session of the month; strata = year x ME tercile of that session (panel
``me_line``) x event month (the line has a ``corporate_actions`` split or distribution in the month); a
deterministic hash of (security_id, month, ``SEED``) orders each stratum; ``N_SAMPLE`` line-months are allocated
evenly over the strata, event months at ``EVENT_SHARE`` of the sample. Every daily return of a sampled line-month
is checked (``TOL`` = 1 bp):

* ``identity``: CRSP convention ret = (P_t x split + D_t) / P_{t-1} - 1 with the split ratio and the cash amount
  from ``corporate_actions/vendor_events.parquet`` (``split_ratio``, ``implied_cash``); on days without an event,
  ret = P_t / P_{t-1} - 1. The vendor's multiplicative convention (P_t / (f P_{t-1}) - 1) differs from it by about
  yield x return on an ex-date, which is reported, not repaired.
* ``chain``: ret = adj_close_t / adj_close_{t-1} - 1 (the stage's backward chain).
* ``monthly``: the product of 1 + ret over the month = adj_close ratio of the month's last session to the prior
  month's last session.

Every exception is attributed: ``fb_action`` repaired / kept (factor-break-v1), ``ret_source`` observed (gap
repair), ``ret_guarded``, ``sid0_repaired``, ``large_distribution`` (spin-off or special), ``cash_distribution``
(convention difference), ``unexplained``.

Independent second source: none is used. Free daily price APIs (Yahoo, Stooq, Nasdaq, Alpha Vantage) forbid bulk
automated retrieval or redistribution in their terms, or need a key under terms not approved here; SEC holds no
prices. The gap is documented in ``docs/ALPHA_PANEL_MARKET.md``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from typing import Any

from . import common as C
from . import market_common as M

RULE = "returns-identity-v1"
SEED = 20260929
N_SAMPLE = 500
EVENT_SHARE = 0.2
TOL = 1e-4
END = dt.date(2022, 12, 31)


def allocate(n: int, sizes: dict[tuple, int], event_share: float = EVENT_SHARE) -> dict[tuple, int]:
    """Even allocation over strata (keys ``(year, tercile, event_month)``), event strata at ``event_share`` of ``n``,
    each capped at its population; a shortfall goes round-robin to strata with room left."""
    alloc = {k: 0 for k in sizes}
    for group, total in (([k for k in sizes if k[2]], round(n * event_share)),
                         ([k for k in sizes if not k[2]], n - round(n * event_share))):
        for i in range(total):
            if group:
                alloc[group[i % len(group)]] += 1
    over = 0
    for k in alloc:
        if alloc[k] > sizes[k]:
            over += alloc[k] - sizes[k]
            alloc[k] = sizes[k]
    keys = sorted(sizes)
    while over > 0 and any(alloc[k] < sizes[k] for k in keys):
        for k in keys:
            if over and alloc[k] < sizes[k]:
                alloc[k] += 1
                over -= 1
    return alloc


def build(n: int = N_SAMPLE) -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="300MB", threads=2, db_file="returns_validation.duckdb")
    prices = (root / "prices" / "year=*" / "prices.parquet").as_posix()
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    ev = (root / "corporate_actions" / "vendor_events.parquet").as_posix()
    con.execute(f"""
        CREATE TABLE lm AS
        WITH first AS (
            SELECT security_id, CAST(date_trunc('month', session_date) AS DATE) AS month, me_line
            FROM read_parquet('{panel}', union_by_name = true, hive_partitioning = false)
            WHERE member_equity AND session_date <= DATE '{END}' AND session_date >= DATE '2018-02-01'
            QUALIFY session_date = min(session_date) OVER (PARTITION BY CAST(date_trunc('month', session_date) AS DATE))
        ),
        evm AS (SELECT DISTINCT security_id, CAST(date_trunc('month', ex_date) AS DATE) AS month
                FROM read_parquet('{ev}') WHERE kind IN ('split', 'reverse_split', 'cash_distribution', 'large_distribution'))
        SELECT f.*, year(f.month) AS y, ntile(3) OVER (PARTITION BY f.month ORDER BY f.me_line) AS tercile,
               e.security_id IS NOT NULL AS event_month,
               hash(f.security_id, f.month, {SEED}) AS h
        FROM first f LEFT JOIN evm e USING (security_id, month)
        WHERE f.me_line > 0
    """)
    sizes = {(y, t, e): k for y, t, e, k in con.execute(
        "SELECT y, tercile, event_month, count(*) FROM lm GROUP BY ALL ORDER BY ALL").fetchall()}
    alloc = allocate(n, sizes)
    con.execute("CREATE TABLE sample (security_id BIGINT, month DATE, y INTEGER, tercile INTEGER, event_month BOOLEAN)")
    for (y, t, e), k in alloc.items():
        con.execute(f"""INSERT INTO sample SELECT security_id, month, y, tercile, event_month FROM lm
                        WHERE y = {y} AND tercile = {t} AND event_month = {e} ORDER BY h LIMIT {k}""")
    con.execute(f"""
        CREATE TABLE days AS
        WITH p AS (
            SELECT p.*, lag(adj_close) OVER (PARTITION BY security_id ORDER BY session_date) AS adj_prev
            FROM read_parquet('{prices}', hive_partitioning = false) p
            WHERE security_id IN (SELECT security_id FROM sample)
              AND session_date >= DATE '2018-01-01' AND session_date <= DATE '{END}'
        )
        SELECT s.y, s.tercile, s.event_month, p.security_id, p.session_date, s.month, p.close, p.prev_raw_close,
               p.ret, p.ret_guarded, p.ret_source, p.fb_action, p.sid0_repaired, p.return_factor, p.adj_close,
               p.adj_prev, e.kind, e.split_ratio, e.implied_cash
        FROM p JOIN sample s ON s.security_id = p.security_id AND CAST(date_trunc('month', p.session_date) AS DATE) = s.month
        LEFT JOIN read_parquet('{ev}') e ON e.security_id = p.security_id AND e.ex_date = p.session_date
        WHERE p.ret IS NOT NULL
    """)
    con.execute(f"""
        CREATE TABLE chk AS
        SELECT *,
            CASE WHEN kind IN ('split', 'reverse_split') THEN close * split_ratio / prev_raw_close - 1
                 WHEN kind IN ('cash_distribution', 'large_distribution') THEN (close + implied_cash) / prev_raw_close - 1
                 ELSE close / prev_raw_close - 1 END AS ret_identity,
            adj_close / adj_prev - 1 AS ret_chain
        FROM days
    """)
    con.execute(f"""
        CREATE TABLE res AS
        SELECT *, abs(ret - ret_identity) <= {TOL} AS ok_identity, abs(ret - ret_chain) <= {TOL} AS ok_chain,
            CASE WHEN abs(ret - ret_identity) <= {TOL} THEN NULL
                 WHEN fb_action = 'repaired' THEN 'factor_break_repaired'
                 WHEN fb_action LIKE 'kept%' THEN 'factor_break_kept_' || fb_action
                 WHEN ret_source = 'observed' THEN 'observed_gap_repair'
                 WHEN sid0_repaired THEN 'sid0_repaired'
                 WHEN ret_guarded THEN 'guarded'
                 WHEN kind = 'large_distribution' THEN 'large_distribution'
                 WHEN kind = 'cash_distribution' THEN 'cash_distribution_convention'
                 WHEN kind IN ('split', 'reverse_split') THEN 'split_rounding'
                 WHEN kind IS NULL AND abs(return_factor - 1) > 1e-6 THEN 'factor_below_event_threshold'
                 ELSE 'unexplained' END AS reason
        FROM chk
    """)
    tot = con.execute("SELECT count(*), sum(ok_identity::INT), sum(ok_chain::INT), count(DISTINCT (security_id, month)) FROM res").fetchone()
    reasons = dict(con.execute("SELECT reason, count(*) FROM res WHERE reason IS NOT NULL GROUP BY 1 ORDER BY 2 DESC").fetchall())
    monthly = con.execute(f"""
        WITH m AS (SELECT security_id, month, exp(sum(ln(1 + ret))) - 1 AS r FROM res GROUP BY 1, 2),
        edge AS (
            SELECT p.security_id, CAST(date_trunc('month', p.session_date) AS DATE) AS month,
                   arg_max(p.adj_close, p.session_date) AS adj_end
            FROM read_parquet('{prices}', hive_partitioning = false) p
            WHERE p.security_id IN (SELECT security_id FROM sample) AND p.session_date <= DATE '{END}'
            GROUP BY 1, 2)
        SELECT count(*), sum((abs(m.r - (e1.adj_end / e0.adj_end - 1)) <= {TOL})::INT)
        FROM m JOIN edge e1 ON e1.security_id = m.security_id AND e1.month = m.month
        JOIN edge e0 ON e0.security_id = m.security_id AND e0.month = CAST(m.month - INTERVAL 1 MONTH AS DATE)
    """).fetchone()
    by_event = [list(map(str, r)) for r in con.execute("""
        SELECT coalesce(kind, 'none'), count(*), avg(ok_identity::INT), max(abs(ret - ret_identity)) FROM res
        GROUP BY 1 ORDER BY 2 DESC""").fetchall()]
    worst = [list(map(str, r)) for r in con.execute("""
        SELECT security_id, session_date, round(ret, 6), round(ret_identity, 6), kind, fb_action, ret_source, reason
        FROM res WHERE reason IS NOT NULL ORDER BY abs(ret - ret_identity) DESC LIMIT 25""").fetchall()]
    out = {"rule": RULE, "rule_text": __doc__, "seed": SEED, "tolerance": TOL, "window_end": END.isoformat(),
           "sample": {"line_months": int(tot[3]), "strata": len(alloc), "daily_returns": int(tot[0])},
           "identity_within_1bp": tot[1] / tot[0], "chain_within_1bp": tot[2] / tot[0],
           "monthly_within_1bp": (monthly[1] / monthly[0]) if monthly[0] else None, "monthly_checked": monthly[0],
           "exceptions_by_reason": reasons, "by_event_kind": by_event, "largest_exceptions": worst,
           "second_source": "none within terms (see rule_text)",
           "gate": {"identity_within_1bp_min": 0.99, "pass": tot[1] / tot[0] >= 0.99,
                    "every_exception_explained": "unexplained" not in reasons},
           "input_manifests_sha256": M.input_manifests("prices", "panel", "corporate_actions")}
    C.write_json_atomic(C.stage_dir("validation") / "returns.json", out)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=N_SAMPLE)
    args = ap.parse_args(argv)
    out = build(args.n)
    print(json.dumps({k: v for k, v in out.items() if k != "rule_text"}, default=str)[:5000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
