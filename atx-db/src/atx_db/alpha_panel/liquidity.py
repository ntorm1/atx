"""Stage ``market_liquidity`` (S3.7): cost-model inputs per line-session -> ``market/liquidity/year=YYYY/``.

One row per line and calendar session between the line's first and last vendor bar (a listed line with no bar
that session is a no-trade row), 2018-01-02 on. Every window counts calendar sessions (``ROWS``), so a halted or
untraded session is inside the window. Rule ``liquidity-v1``; inputs are raw OHLC, volume, the vendor total return
and ``market/shares_daily``. A value at session d uses bars through d (its close): ``available_at`` = d 22:00 UTC.

* ``spread_cs_21``: Corwin and Schultz (2012, JF) high-low spread, mean of the two-day estimates over the pairs
  (t-1, t) of consecutive sessions with t in the last 21 sessions, negative two-day estimates set to 0. Day t-1's high, low and close are put on
  day t's basis with the vendor price factor of t (splits, distributions; a ``factor-break-v1`` repaired step is
  not a price change and uses factor 1); the overnight adjustment of CS section II.D shifts day t's range by the gap
  when t's low is above (high below) the adjusted prior close. Needs >= ``MIN_PAIRS`` pairs.
* ``spread_ar_21``: Abdi and Ranaldo (2017, RFS) close-high-low estimator, sqrt(max(mean over pairs (t, t+1) with
  t+1 <= d of 4 (c_t - eta_t)(c_t - eta_{t+1}), 0)), c = log close, eta = log mid-range; day t on t+1's basis.
* ``amihud_{21,63,252}``: Amihud (2002) mean of |ret| / dollar volume x 1e6 over sessions with a bar, a positive
  dollar volume and an unguarded return; needs >= 10 / 30 / 60 observations.
* ``turnover``: volume / ``shrout`` of the session (same share basis); ``turnover_{21,63}`` means over the window
  (untraded sessions count as 0) with ``shrout`` present on >= half of them.
* ``zero_vol_{21,63}``: share of window sessions without a trade (no bar or volume 0).
* ``adv_{21,63}``: mean dollar volume (raw close x volume) over the window, untraded sessions as 0.
* ``halt_proxy``: the line is listed (between its first and last bar) and did not trade this session;
  ``halt_days_21`` counts them in the window. No exchange halt feed exists in atx-db: this is the proxy.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Any

from . import common as C
from . import market_common as M

STAGE = "market_liquidity"
SCHEMA = "atx.alpha-panel.market-liquidity/v1"
RULE = "liquidity-v1"
MODULES = ("liquidity", "market_common", "common")
OUT_DIR = "market"
MIN_PAIRS = 10
INPUTS = ("prices", "market/market_shares_manifest.json")
K = 3 - 2 * math.sqrt(2)

LAKE_STAGES = [
    {"name": "market_liquidity", "lane": "MKT", "schema": "atx.alpha-panel.market-liquidity/v1",
     "module": "atx_db.alpha_panel.liquidity", "args": ["build"], "inputs": ["prices", "market_shares"],
     "manifest": "market/market_liquidity_manifest.json",
     "outputs": [{"glob": "market/liquidity/year=*/liquidity.parquet", "view": "market_liquidity"}],
     "staleness": "daily; rebuilt with prices", "vintage": "daily", "guard_gb": 0.4},
]


def cs_two_day(h0: float, l0: float, c0: float, h1: float, l1: float) -> float | None:
    """Corwin-Schultz two-day spread from (t-1: high, low, close) and (t: high, low) on one basis, floored at 0."""
    if not (h0 >= l0 > 0 and h1 >= l1 > 0 and c0 > 0):
        return None
    if l1 > c0:
        h1, l1 = h1 - (l1 - c0), c0
    elif h1 < c0:
        h1, l1 = c0, l1 + (c0 - h1)
    beta = math.log(h0 / l0) ** 2 + math.log(h1 / l1) ** 2
    gamma = math.log(max(h0, h1) / min(l0, l1)) ** 2
    alpha = (math.sqrt(2 * beta) - math.sqrt(beta)) / K - math.sqrt(gamma / K)
    return max(2 * (math.exp(alpha) - 1) / (1 + math.exp(alpha)), 0.0)


def ar_term(c0: float, h0: float, l0: float, h1: float, l1: float) -> float | None:
    """Abdi-Ranaldo pair term 4 (c_t - eta_t)(c_t - eta_{t+1}) with t on t+1's basis."""
    if not (c0 > 0 and h0 >= l0 > 0 and h1 >= l1 > 0):
        return None
    c = math.log(c0)
    return 4 * (c - (math.log(h0) + math.log(l0)) / 2) * (c - (math.log(h1) + math.log(l1)) / 2)


def bucket_sql(b: int, k: int | None = None) -> str:
    shares = (C.build_root() / OUT_DIR / "shares_daily" / "year=*" / "shares_daily.parquet").as_posix()
    kc = f"{K!r}"
    return f"""
    WITH bars AS (
        SELECT security_id, session_date, sidx, high, low, close, volume, dollar_volume, ret, ret_guarded,
               CASE WHEN fb_action = 'repaired' OR NOT (return_factor > 0 AND isfinite(return_factor)) THEN 1.0
                    ELSE return_factor END AS f
        FROM read_parquet('{M.bucket_path(b)}') WHERE {M.sub_filter(k)}
    ),
    span AS (SELECT security_id, min(sidx) AS s0, max(sidx) AS s1 FROM bars GROUP BY 1),
    grid AS (
        SELECT s.security_id, cal.session_date, cal.sidx
        FROM span s JOIN {M.calendar_sql()} cal ON cal.sidx BETWEEN s.s0 AND s.s1
    ),
    g AS (
        SELECT grid.security_id, grid.session_date, grid.sidx, b.high, b.low, b.close, b.volume, b.dollar_volume,
               b.ret, b.ret_guarded, b.f, b.security_id IS NOT NULL AS has_bar, sh.shrout
        FROM grid LEFT JOIN bars b USING (security_id, sidx)
        LEFT JOIN (SELECT security_id, session_date, shrout FROM read_parquet('{shares}')
                   WHERE security_id % {M.BUCKETS} = {b}) sh
          ON sh.security_id = grid.security_id AND sh.session_date = grid.session_date
    ),
    lg AS (
        SELECT *,
               lag(high) OVER w * f AS h0, lag(low) OVER w * f AS l0, lag(close) OVER w * f AS c0
        FROM g WINDOW w AS (PARTITION BY security_id ORDER BY sidx)
    ),
    adj AS (
        SELECT *,
               CASE WHEN low > c0 THEN high - (low - c0) WHEN high < c0 THEN c0 ELSE high END AS h1,
               CASE WHEN low > c0 THEN c0 WHEN high < c0 THEN low + (c0 - high) ELSE low END AS l1
        FROM lg
    ),
    pair AS (
        SELECT *,
               CASE WHEN h0 >= l0 AND l0 > 0 AND high >= low AND low > 0 AND c0 > 0 THEN
                   ln(h0 / l0) ^ 2 + ln(h1 / l1) ^ 2 END AS beta,
               CASE WHEN h0 >= l0 AND l0 > 0 AND high >= low AND low > 0 AND c0 > 0 THEN
                   ln(greatest(h0, h1) / least(l0, l1)) ^ 2 END AS gamma,
               CASE WHEN c0 > 0 AND h0 >= l0 AND l0 > 0 AND high >= low AND low > 0 THEN
                   4 * (ln(c0) - (ln(h0) + ln(l0)) / 2) * (ln(c0) - (ln(high) + ln(low)) / 2) END AS ar,
               CASE WHEN has_bar AND dollar_volume > 0 AND ret IS NOT NULL AND NOT ret_guarded
                    THEN abs(ret) / dollar_volume * 1e6 END AS illiq,
               NOT has_bar OR coalesce(volume, 0) <= 0 AS no_trade
        FROM adj
    ),
    cs AS (
        SELECT *,
               CASE WHEN beta IS NOT NULL THEN
                   greatest(2 * (exp((sqrt(2 * beta) - sqrt(beta)) / {kc} - sqrt(gamma / {kc})) - 1)
                            / (1 + exp((sqrt(2 * beta) - sqrt(beta)) / {kc} - sqrt(gamma / {kc}))), 0) END AS cs2
        FROM pair
    ),
    win AS (
        SELECT security_id, session_date, has_bar, no_trade,
               avg(cs2) OVER w21 AS cs_mean, count(cs2) OVER w21 AS n_cs,
               avg(ar) OVER w21 AS ar_mean, count(ar) OVER w21 AS n_ar,
               avg(illiq) OVER w21 AS a21, count(illiq) OVER w21 AS n21,
               avg(illiq) OVER w63 AS a63, count(illiq) OVER w63 AS n63,
               avg(illiq) OVER w252 AS a252, count(illiq) OVER w252 AS n252,
               CASE WHEN shrout > 0 THEN coalesce(volume, 0) / shrout END AS turnover,
               sum(CASE WHEN shrout > 0 THEN coalesce(volume, 0) / shrout END) OVER w21 / count(*) OVER w21 AS t21,
               count(shrout) OVER w21 AS ns21, count(*) OVER w21 AS len21,
               sum(CASE WHEN shrout > 0 THEN coalesce(volume, 0) / shrout END) OVER w63 / count(*) OVER w63 AS t63,
               count(shrout) OVER w63 AS ns63, count(*) OVER w63 AS len63,
               avg(CAST(no_trade AS DOUBLE)) OVER w21 AS zero_vol_21, avg(CAST(no_trade AS DOUBLE)) OVER w63 AS zero_vol_63,
               avg(coalesce(dollar_volume, 0)) OVER w21 AS adv_21, avg(coalesce(dollar_volume, 0)) OVER w63 AS adv_63,
               CAST(sum(CAST(no_trade AS INTEGER)) OVER w21 AS INTEGER) AS halt_days_21, shrout
        FROM cs
        WINDOW w21 AS (PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 20 PRECEDING AND CURRENT ROW),
               w63 AS (PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 62 PRECEDING AND CURRENT ROW),
               w252 AS (PARTITION BY security_id ORDER BY sidx ROWS BETWEEN 251 PRECEDING AND CURRENT ROW)
    )
    SELECT security_id, session_date, has_bar, no_trade AS halt_proxy, halt_days_21,
           CASE WHEN n_cs >= {MIN_PAIRS} THEN cs_mean END AS spread_cs_21, CAST(n_cs AS INTEGER) AS n_pairs_cs_21,
           CASE WHEN n_ar >= {MIN_PAIRS} THEN sqrt(greatest(ar_mean, 0)) END AS spread_ar_21,
           CASE WHEN n21 >= 10 THEN a21 END AS amihud_21, CASE WHEN n63 >= 30 THEN a63 END AS amihud_63,
           CASE WHEN n252 >= 60 THEN a252 END AS amihud_252, CAST(n252 AS INTEGER) AS n_amihud_252,
           turnover, CASE WHEN ns21 * 2 >= len21 THEN t21 END AS turnover_21,
           CASE WHEN ns63 * 2 >= len63 THEN t63 END AS turnover_63,
           zero_vol_21, zero_vol_63, adv_21, adv_63, shrout AS shrout_used,
           CAST(session_date AS TIMESTAMP) + {M.MARK} AS available_at
    FROM win
    WHERE session_date >= DATE '{C.WARMUP_START}'
    """


def build(buckets: list[int] | None = None) -> dict[str, Any]:
    receipt: dict[str, Any] = {"rule": RULE}
    con = C.connect(memory="240MB", threads=2)
    M.project_prices(con, receipt)
    tmp = M.resumable_dir("liquidity", M.input_manifests(*INPUTS), MODULES) if buckets is None else M.tmp("liquidity")
    work = [(b, None) for b in buckets] if buckets is not None else M.units()
    for b, k in work:
        name = f"bucket={b:02d}" + ("" if k is None else f"_{k}")
        if buckets is None and (tmp / f"{name}.parquet").exists():
            continue  # resumed after a guard stop
        with C.timed(receipt, name):
            n = C.copy_to_parquet(con, bucket_sql(b, k), tmp / f"{name}.parquet")
        print(f"{name}: {n} rows {receipt['timings_s'][name]} s", flush=True)
    if buckets is not None:
        return receipt
    out = C.stage_dir(OUT_DIR) / "liquidity"
    glob = (tmp / "bucket=*.parquet").as_posix()
    lo, hi = con.execute(f"SELECT min(session_date), max(session_date) FROM read_parquet('{glob}')").fetchone()
    years = {}
    for y in range(lo.year, hi.year + 1):
        years[str(y)] = C.copy_to_parquet(con, f"""
            SELECT * FROM read_parquet('{glob}') WHERE year(session_date) = {y} ORDER BY session_date, security_id
        """, out / f"year={y}" / "liquidity.parquet")
    receipt["years"] = years
    M.publish_part(OUT_DIR, STAGE, SCHEMA, MODULES, {"rule": RULE, "rule_text": __doc__, "receipt": receipt,
                                                    "staleness": "daily"},
                   M.input_manifests(*INPUTS), pattern="liquidity/**/*.parquet")
    return receipt


MEASURED = ("spread_cs_21", "spread_ar_21", "amihud_21", "amihud_63", "amihud_252", "turnover", "turnover_21",
            "turnover_63", "zero_vol_21", "zero_vol_63", "adv_21", "adv_63", "halt_days_21")


def measure() -> dict[str, Any]:
    """Share of panel member_equity cells with a finite value, per column and year (done criterion >= 99%)."""
    con = C.connect(memory="240MB", threads=2)
    glob = (C.build_root() / OUT_DIR / "liquidity" / "year={y}" / "liquidity.parquet").as_posix()
    res = M.member_coverage(con, glob, {c: f"isfinite(t.{c})" for c in MEASURED})
    tot = sum(v["cells"] for v in res.values())
    overall = {c: sum(v["cells"] * v[c] for v in res.values()) / tot for c in MEASURED}
    summary = {"rule": RULE, "per_year": res, "overall_2019_2026": overall, "gate": {"finite_min": 0.99},
               "pass": {c: overall[c] >= 0.99 for c in MEASURED},
               "input_manifests_sha256": M.input_manifests("panel", "market/market_liquidity_manifest.json")}
    C.write_json_atomic(C.stage_dir("validation") / "liquidity_coverage.json", summary)
    return summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build", "measure", "all"), default="build")
    ap.add_argument("--buckets", default=None)
    args = ap.parse_args(argv)
    if args.step in ("build", "all"):
        rec = build([int(x) for x in args.buckets.split(",")] if args.buckets else None)
        print(json.dumps({k: v for k, v in rec.items() if k != "timings_s"}, default=str)[:3000], flush=True)
    if args.step in ("measure", "all"):
        print(json.dumps(measure(), default=str)[:5000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
