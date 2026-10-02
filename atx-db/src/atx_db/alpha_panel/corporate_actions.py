"""D10 corporate actions from the vendor's daily return factors (splits, distributions), per line and ex-date.

TickerHistory3 ``returnFactor`` on an ex-date is the price adjustment of that day: a 4:1 split gives 0.25, a cash
dividend gives 1 - dividend / prior close (AAPL 2020-08-07: 0.99820 = 1 - 0.82 / 455.61), a 1:10 reverse split
gives 10. Rule ``vendor-factor-events-v1``, for every line-session of ``prices/`` with ``|return_factor - 1| > 1e-6``:

* ``kind``: ``vendor_factor_break`` when the session was repaired by ``factor-break-v1`` (a vendor artefact, not an
  action); ``split`` when 1/f is within 1% of a ratio p/q with q <= 4 and 1/f >= 1.2; ``reverse_split`` for f >= 1.2
  within 1% of such a ratio; ``cash_distribution`` for 0.8 <= f < 1; ``large_distribution`` (special dividend or
  spin-off) for f < 0.8 that is not split-like; ``other`` otherwise.
* ``split_ratio`` = 1 / f for splits and reverse splits; ``distribution_yield`` = 1 - f and ``implied_cash`` =
  ``prev_raw_close * (1 - f)`` for distributions.
* ``available_at`` = ex-date 22:00 UTC (the vendor records the action on the ex-date; the announcement date is
  unknown here, so this is conservative). ``vintage_risk`` = true: the factors come from one 2026-09-20 vendor
  snapshot, so history is as restated by the vendor, not as first published.

Mergers, material agreements, changes in control and bankruptcies are SEC 8-K events (``sec_filings/events``);
index additions and deletions have no source in atx-db.
"""

from __future__ import annotations

import sys
from fractions import Fraction
from typing import Any

from . import common as C

RULE = "vendor-factor-events-v1"


def split_like(x: float | None) -> bool:
    """True when x (>= 1) is within 1% of p/q with q <= 4 (2, 3, 1.5, 4/3, 10, 5/4, ...)."""
    if x is None or not x > 0:
        return False
    frac = Fraction(x).limit_denominator(4)
    return float(frac) > 0 and abs(x / float(frac) - 1) <= 0.01


def classify(f: float | None, fb_action: str | None) -> str | None:
    if f is None:
        return None
    if fb_action == "repaired":
        return "vendor_factor_break"
    if f < 1 and 1 / f >= 1.2 and split_like(1 / f):
        return "split"
    if f >= 1.2 and split_like(f):
        return "reverse_split"
    if 0.8 <= f < 1:
        return "cash_distribution"
    if f < 0.8:
        return "large_distribution"
    return "other"


def build() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="500MB", threads=1)
    con.create_function("classify_factor", classify, ["DOUBLE", "VARCHAR"], "VARCHAR", null_handling="special")
    prices = (root / "prices" / "*" / "*.parquet").as_posix()
    out = C.stage_dir("corporate_actions")
    n = C.copy_to_parquet(con, f"""
        SELECT security_id, session_date AS ex_date, ticker, return_factor, prev_raw_close, fb_action,
               classify_factor(return_factor, fb_action) AS kind,
               CASE WHEN classify_factor(return_factor, fb_action) IN ('split', 'reverse_split') THEN 1 / return_factor END
                   AS split_ratio,
               CASE WHEN return_factor < 1 AND classify_factor(return_factor, fb_action) IN ('cash_distribution', 'large_distribution')
                    THEN 1 - return_factor END AS distribution_yield,
               CASE WHEN return_factor < 1 AND classify_factor(return_factor, fb_action) IN ('cash_distribution', 'large_distribution')
                    THEN prev_raw_close * (1 - return_factor) END AS implied_cash,
               CAST(session_date AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR AS available_at,
               true AS vintage_risk
        FROM read_parquet('{prices}')
        WHERE abs(return_factor - 1) > 1e-6
        ORDER BY security_id, session_date
    """, out / "vendor_events.parquet")
    per_year = con.execute(f"""
        SELECT year(ex_date), kind, count(*), count(DISTINCT security_id)
        FROM read_parquet('{(out / "vendor_events.parquet").as_posix()}') GROUP BY ALL ORDER BY ALL
    """).fetchall()
    receipt = {"rule": RULE, "rows": n, "per_year_kind": [list(map(str, r)) for r in per_year]}
    C.write_stage_manifest("corporate_actions", "atx.alpha-panel.corporate-actions/v1", ("corporate_actions", "common"), {
        "rule": RULE, "rule_text": __doc__, "staleness": "event data (no staleness)",
        "sources": {"prices_manifest": C.output_hashes(root / "prices", "*.json")}, "receipt": receipt})
    return receipt


def main() -> int:
    print(build())
    return 0


if __name__ == "__main__":
    sys.exit(main())
