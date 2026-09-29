"""TRAIN-window information-coefficient screen of the characteristics (writes docs/ALPHA_PANEL_IC.md).

Only sessions 2020-09-28..2022-12-31 are read (the mega-alpha TRAIN years): the scorecard's validation
(2023-2024) and holdout (2025+) years are never touched, so this screen spends no out-of-sample data.

For each characteristic and session: Spearman rank IC among ``member_equity`` lines with a value, against
(a) the next-but-one session return r[d+2] (the scorecard's forward convention: decide at d, trade d+1,
earn d+2) and (b) the 21-session forward return from d+2 to d+22. Market-neutral by construction of rank IC.
Reported: mean IC, Newey-West t (lag 5 for 1-day, lag 21 for 21-day), share of positive-IC sessions,
coverage, and the mean absolute cross-sectional rank correlation with the scorecard's admitted members.
"""

from __future__ import annotations

import datetime as dt
import math
import sys
from typing import Any

import numpy as np

from . import common as C
from .characteristics import EXTRA, SCORECARD

TRAIN_START = dt.date(2020, 9, 28)
TRAIN_END = dt.date(2022, 12, 31)
ADMITTED = ("chtax", "droe", "ear", "sue", "asset_growth", "noa_at", "low_beta", "low_ivol", "low_max", "lowvol_ind",
            "iv_rv_spread", "high_52w", "ind_mom_12_1", "mom_12_1", "within_ind_mom", "accruals", "cfoa", "fscore", "gpa",
            "opbe", "opex_at", "roa", "roe_q", "ind_adj_rev_5", "seasonality_same_month", "dtc", "si_change", "si_ratio",
            "cfp", "fcfp", "net_payout", "rd_me")


def nw_t(x: np.ndarray, lag: int) -> float:
    x = x[np.isfinite(x)]
    n = len(x)
    if n < 30:
        return float("nan")
    m = x.mean()
    e = x - m
    s = (e @ e) / n
    for k in range(1, lag + 1):
        w = 1 - k / (lag + 1)
        s += 2 * w * (e[k:] @ e[:-k]) / n
    return float(m / math.sqrt(s / n)) if s > 0 else float("nan")


def run() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="650MB", threads=2)
    chars = (root / "characteristics" / "*" / "*.parquet").as_posix()
    panel = (root / "panel" / "*" / "*.parquet").as_posix()
    names = [c for c in list(SCORECARD) + list(EXTRA)]
    # forward returns per line from the panel: r[d+2] and the 21-session compound from d+2..d+22
    con.execute(f"""
        CREATE TEMP TABLE fwd AS
        WITH p AS (
            SELECT session_date, security_id, member_equity,
                   CASE WHEN ret_guarded THEN NULL ELSE ln(1 + ret) END AS lr
            FROM read_parquet('{panel}', hive_partitioning = false)
            WHERE session_date BETWEEN DATE '{TRAIN_START}' AND DATE '{TRAIN_END}' + INTERVAL 45 DAY
        )
        SELECT session_date, security_id, member_equity,
               lead(lr, 2) OVER w AS f1,
               sum(lr) OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN 2 FOLLOWING AND 22 FOLLOWING) AS f21,
               count(lr) OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN 2 FOLLOWING AND 22 FOLLOWING) AS n21
        FROM p WINDOW w AS (PARTITION BY security_id ORDER BY session_date)
    """)
    cols = ", ".join(f"c.{n}" for n in names)
    con.execute(f"""
        CREATE TEMP TABLE x AS
        SELECT c.session_date, c.security_id, f.f1, CASE WHEN f.n21 >= 18 THEN f.f21 END AS f21, {cols}
        FROM read_parquet('{chars}', hive_partitioning = false) c
        JOIN fwd f USING (session_date, security_id)
        WHERE f.member_equity AND c.session_date BETWEEN DATE '{TRAIN_START}' AND DATE '{TRAIN_END}'
    """)
    # daily rank ICs via DuckDB: rank within session, then Pearson of ranks
    res: dict[str, Any] = {"window": [str(TRAIN_START), str(TRAIN_END)], "ic": {}}
    for n in names:
        rows = con.execute(f"""
            WITH r AS (
                SELECT session_date,
                       rank() OVER (PARTITION BY session_date ORDER BY {n}) AS rx,
                       rank() OVER (PARTITION BY session_date ORDER BY f1) AS r1,
                       CASE WHEN f21 IS NOT NULL THEN rank() OVER (PARTITION BY session_date, f21 IS NULL ORDER BY f21) END AS r21,
                       f1, f21
                FROM x WHERE {n} IS NOT NULL AND isfinite({n}) AND f1 IS NOT NULL
            )
            SELECT session_date, corr(rx, r1), corr(rx, r21), count(*) FROM r GROUP BY 1 ORDER BY 1
        """).fetchall()
        if not rows:
            continue
        ic1 = np.array([r[1] if r[1] is not None else np.nan for r in rows], dtype=float)
        ic21 = np.array([r[2] if r[2] is not None else np.nan for r in rows], dtype=float)
        cnt = np.array([r[3] for r in rows], dtype=float)
        res["ic"][n] = {
            "sessions": len(rows), "names_mean": round(float(cnt.mean()), 1),
            "ic1_mean": round(float(np.nanmean(ic1)), 5), "ic1_t_nw5": round(nw_t(ic1, 5), 2),
            "ic1_pos": round(float(np.nanmean(ic1 > 0)), 3),
            "ic21_mean": round(float(np.nanmean(ic21)), 5), "ic21_t_nw21": round(nw_t(ic21, 21), 2),
        }
        print(n, res["ic"][n], flush=True)
    # redundancy of each characteristic vs the admitted scorecard members (mean |rank corr| on 12 month-ends)
    ends = [r[0] for r in con.execute("""SELECT max(session_date) FROM x GROUP BY year(session_date), month(session_date)
                                         ORDER BY 1""").fetchall()]
    sample = ends[:: max(1, len(ends) // 12)]
    dates = ", ".join(f"DATE '{d}'" for d in sample)
    frame = con.execute(f"SELECT session_date, {', '.join(names)} FROM x WHERE session_date IN ({dates})").fetchnumpy()
    for n in names:
        if n not in res["ic"]:
            continue
        vals = []
        for d in sample:
            mask = frame["session_date"].astype("datetime64[D]") == np.datetime64(d)
            a = np.asarray(frame[n], dtype=float)[mask]
            best = 0.0
            for m in ADMITTED:
                if m == n or m not in frame:
                    continue
                b = np.asarray(frame[m], dtype=float)[mask]
                ok = np.isfinite(a) & np.isfinite(b)
                if ok.sum() < 100:
                    continue
                ra = np.argsort(np.argsort(a[ok]))
                rb = np.argsort(np.argsort(b[ok]))
                best = max(best, abs(float(np.corrcoef(ra, rb)[0, 1])))
            vals.append(best)
        res["ic"][n]["max_abs_rank_corr_admitted"] = round(float(np.mean(vals)), 3) if vals else None
    C.write_json_atomic(root / "ic_train.json", res)
    lines = [
        "# Characteristic IC screen (TRAIN 2020-09-28 .. 2022-12-31 only)", "",
        __doc__.split("\n\n", 1)[1].strip(), "",
        "| characteristic | set | names/day | IC r[d+2] | t (NW5) | IC>0 share | IC 21d | t (NW21) | max abs corr vs admitted |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for n, s in sorted(res["ic"].items(), key=lambda kv: -abs(kv[1]["ic21_t_nw21"] if kv[1]["ic21_t_nw21"] == kv[1]["ic21_t_nw21"] else 0)):
        kind = "scorecard" if n in SCORECARD else "new"
        lines.append(f"| `{n}` | {kind} | {s['names_mean']} | {s['ic1_mean']:+.4f} | {s['ic1_t_nw5']} | {s['ic1_pos']} | "
                     f"{s['ic21_mean']:+.4f} | {s['ic21_t_nw21']} | {s.get('max_abs_rank_corr_admitted')} |")
    (C.PACKAGE_ROOT / "docs" / "ALPHA_PANEL_IC.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return res


def main() -> int:
    run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
