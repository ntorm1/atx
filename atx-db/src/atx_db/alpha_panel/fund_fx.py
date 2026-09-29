"""Stage F FX (ruling D4): point-in-time USD conversion of non-USD filers with FRED H.10 daily rates.

Input: ``<build root>/reference/fx_daily.parquet`` published by lane MKT, contract
``obs_date DATE, currency VARCHAR (ISO 4217), usd_per_ccy DOUBLE, series_id VARCHAR, available_at TIMESTAMP (UTC)``
(``available_at`` = the H.10 publication clock of that observation). Env ``ATX_FX_DAILY`` overrides the path.

Rules (``fund_items.issuer_events`` applies them to every event whose filing reports in a covered currency):

* balance (instant) items: the period-end rate = the last observation on or before the balance date, used only
  when it is at most ``SPOT_MAX_GAP_DAYS`` older than that date;
* flows: the period-average rate = the mean of the observations in the flow window (TTM: the 365 days ending at
  ``period_end``; a discrete quarter: the 91 days ending at its end), used only when the window has observations
  on at least ``AVG_MIN_SHARE`` of its weekdays and its last observation is within ``SPOT_MAX_GAP_DAYS`` of the
  window end;
* the clock of a rate is the ``available_at`` of the last observation it uses (the maximum over the window for an
  average); the event's ``available_at`` = max(filing clock, every rate clock used).
"""

from __future__ import annotations

import bisect
import datetime as dt
import os
from pathlib import Path

import pyarrow.parquet as pq

from . import common

SPOT_MAX_GAP_DAYS = 10
AVG_MIN_SHARE = 0.5
TTM_WINDOW_DAYS = 365
Q_WINDOW_DAYS = 91
RULE = (
    "fund-fx-h10-v1 (ruling D4): non-USD filings in a currency covered by reference/fx_daily.parquet (FRED H.10, "
    "usd_per_ccy) are converted from the reporting-currency knowledge: balances (at, lt, che, debt, be, seq, noa, "
    "invt, rect, ppe, act, lct, ap, drev, ppegt, gdwl, intan, mib, pstk, buyback amounts and the catalog stocks) at "
    f"the rate of the last observation on or before the balance date (at most {SPOT_MAX_GAP_DAYS} days older); "
    f"TTM flows at the mean rate of the {TTM_WINDOW_DAYS} days ending at period_end, discrete-quarter flows at the "
    f"mean of the {Q_WINDOW_DAYS} days ending at the quarter end (observations on >= {AVG_MIN_SHARE:.0%} of the "
    "window's weekdays, the last within 10 days of its end); lag balances at their own period-end rate and lag "
    "flows at their own quarter's mean; shares and unitless items are not converted. fx_rate = the period-end rate, "
    "fx_rate_avg_q / fx_rate_avg_ttm = the flow rates, fx_converted = true when the period-end rate exists; an item "
    "whose rate is missing is NaN. available_at = max(filing clock, available_at of every observation used). The "
    "reporting-currency values stay in quarterly_history.parquet (its currency column); currency names the "
    "reporting currency.")


def fx_path() -> Path:
    return Path(os.environ.get("ATX_FX_DAILY", str(common.build_root() / "reference" / "fx_daily.parquet")))


def _weekdays(start: dt.date, end: dt.date) -> int:
    n = (end - start).days + 1
    full, rem = divmod(max(n, 0), 7)
    w0 = start.weekday()
    return full * 5 + sum(1 for k in range(rem) if (w0 + k) % 7 < 5)


def _naive_utc(ts) -> dt.datetime:
    if ts.tzinfo is not None:
        ts = ts.astimezone(dt.UTC).replace(tzinfo=None)
    return ts


class FxTable:
    """Daily rates per currency with O(log n) spot lookups and prefix-sum window means."""

    def __init__(self) -> None:
        self.days: dict[str, list[int]] = {}
        self.rates: dict[str, list[float]] = {}
        self.avail: dict[str, list[dt.datetime]] = {}
        self.cum: dict[str, list[float]] = {}
        self.memo: dict[tuple, tuple[float, dt.datetime] | None] = {}
        self.source: dict[str, object] = {}

    @classmethod
    def from_rows(cls, rows) -> FxTable:
        """``rows``: iterable of (obs_date, currency, usd_per_ccy, available_at); non-positive rates are dropped and a
        duplicate (currency, obs_date) keeps the earliest available_at."""
        best: dict[tuple[str, dt.date], tuple[dt.datetime, float]] = {}
        for d, ccy, rate, avail in rows:
            if d is None or ccy is None or rate is None or avail is None or not (rate > 0):
                continue
            avail = _naive_utc(avail)
            key = (ccy, d)
            if key not in best or avail < best[key][0]:
                best[key] = (avail, float(rate))
        t = cls()
        for (ccy, d), (avail, rate) in sorted(best.items()):
            t.days.setdefault(ccy, []).append(d.toordinal())
            t.rates.setdefault(ccy, []).append(rate)
            t.avail.setdefault(ccy, []).append(avail)
        for ccy, rs in t.rates.items():
            cum = [0.0]
            for r in rs:
                cum.append(cum[-1] + r)
            t.cum[ccy] = cum
        return t

    @classmethod
    def load(cls, path: Path | None = None) -> FxTable:
        path = path or fx_path()
        tab = pq.read_table(path, columns=["obs_date", "currency", "usd_per_ccy", "available_at"])
        cols = [tab.column(c).to_pylist() for c in ("obs_date", "currency", "usd_per_ccy", "available_at")]
        t = cls.from_rows(zip(*cols))
        t.source = {**common.file_identity(path), "sha256": common.sha256_file(path), "rows": tab.num_rows}
        return t

    def covers(self, ccy: str | None) -> bool:
        return ccy is not None and ccy in self.days

    def spot(self, ccy: str, d: dt.date) -> tuple[float, dt.datetime] | None:
        """(rate, clock) of the last observation on or before ``d`` (at most ``SPOT_MAX_GAP_DAYS`` older)."""
        key = ("s", ccy, d)
        if key in self.memo:
            return self.memo[key]
        out = None
        days = self.days.get(ccy)
        if days:
            o = d.toordinal()
            i = bisect.bisect_right(days, o) - 1
            if i >= 0 and o - days[i] <= SPOT_MAX_GAP_DAYS:
                out = (self.rates[ccy][i], self.avail[ccy][i])
        self.memo[key] = out
        return out

    def average(self, ccy: str, start: dt.date, end: dt.date) -> tuple[float, dt.datetime] | None:
        """(mean rate, latest clock) over the observations in [start, end]."""
        key = ("a", ccy, start, end)
        if key in self.memo:
            return self.memo[key]
        out = None
        days = self.days.get(ccy)
        if days:
            lo = bisect.bisect_left(days, start.toordinal())
            hi = bisect.bisect_right(days, end.toordinal())
            n = hi - lo
            if n > 0 and n >= AVG_MIN_SHARE * _weekdays(start, end) and end.toordinal() - days[hi - 1] <= SPOT_MAX_GAP_DAYS:
                out = ((self.cum[ccy][hi] - self.cum[ccy][lo]) / n, max(self.avail[ccy][lo:hi]))
        self.memo[key] = out
        return out

    def flow_ttm(self, ccy: str, end: dt.date) -> tuple[float, dt.datetime] | None:
        return self.average(ccy, end - dt.timedelta(days=TTM_WINDOW_DAYS - 1), end)

    def flow_q(self, ccy: str, end: dt.date) -> tuple[float, dt.datetime] | None:
        return self.average(ccy, end - dt.timedelta(days=Q_WINDOW_DAYS - 1), end)


def load_optional(path: Path | None = None) -> FxTable | None:
    """The FX table, or None when the MKT publication is absent (the build then keeps the v9 currency rule)."""
    path = path or fx_path()
    return FxTable.load(path) if path.exists() else None
