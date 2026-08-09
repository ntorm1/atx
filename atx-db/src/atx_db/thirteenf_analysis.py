"""Reproducible summary and report rendering for the L1vsun 13F analysis."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .connection import DuckDBStore
from .thirteenf_backtest import DEFAULT_MIDCAP_MAX_USD, DEFAULT_MIDCAP_MIN_USD
from .thirteenf_signals import DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER

ORIGINAL_ANALYSIS_URL = "https://x.com/L1vsun/status/2085445915897176101"
SEC_DATASETS_URL = "https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets"
SEC_FORM_URL = "https://www.sec.gov/files/form13f.pdf"
OPENFIGI_DOCS_URL = "https://www.openfigi.com/api/documentation"


def _one(store: DuckDBStore, sql: str) -> tuple[Any, ...]:
    row = store.con.execute(sql).fetchone()
    return tuple(row or ())


def thirteenf_analysis_summary(store: DuckDBStore) -> dict[str, Any]:
    submission_rows, min_report, max_report, min_filing, max_filing = _one(
        store,
        """
        SELECT count(*), min(period_of_report), max(period_of_report),
               min(filing_date), max(filing_date)
        FROM thirteenf_submissions
        """,
    )
    holding_rows = _one(store, "SELECT count(*) FROM thirteenf_holdings")[0]
    correction_rows = _one(store, "SELECT count(*) FROM thirteenf_amendment_corrections")[0]
    rate_rows, amended_quarters, spike_rows = _one(
        store,
        """
        SELECT count(*), count(*) FILTER (WHERE amendment_count > 0),
               count(*) FILTER (WHERE is_spike)
        FROM thirteenf_amendment_rates
        WHERE is_latest_revision
        """,
    )
    candidate_rows = _one(
        store,
        """
        SELECT count(*)
        FROM thirteenf_consensus_amendment_signals
        WHERE is_latest_revision
        """,
    )[0]
    signal_rows, signal_years = _one(
        store,
        f"""
        SELECT count(*), count(DISTINCT year(report_period))
        FROM thirteenf_consensus_amendment_signals
        WHERE is_latest_revision
          AND signal_rank <= {DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER}
          AND NOT is_stress_quarter
        """,
    )
    mapped_signals = _one(
        store,
        f"""
        SELECT count(*)
        FROM thirteenf_consensus_amendment_signals s
        WHERE s.is_latest_revision
          AND s.signal_rank <= {DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER}
          AND NOT s.is_stress_quarter
          AND EXISTS (SELECT 1 FROM v_thirteenf_signal_instruments m WHERE m.cusip = s.cusip)
        """,
    )[0]
    followups, disclosed_exits, within_47 = _one(
        store,
        f"""
        SELECT coalesce(sum(o.followup_manager_count), 0),
               coalesce(sum(o.exited_manager_count), 0),
               coalesce(sum(o.exits_disclosed_within_47_trading_days), 0)
        FROM thirteenf_consensus_signal_outcomes o
        JOIN thirteenf_consensus_amendment_signals s USING (signal_id)
        WHERE o.is_latest_revision
          AND s.is_latest_revision
          AND s.signal_rank <= {DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER}
          AND NOT s.is_stress_quarter
        """,
    )
    completed_47, mean_47, median_47, win_47 = _one(
        store,
        """
        SELECT count(*) FILTER (WHERE is_complete),
               avg(net_short_return) FILTER (WHERE is_complete),
               median(net_short_return) FILTER (WHERE is_complete),
               avg((net_short_return > 0)::INTEGER) FILTER (WHERE is_complete)
        FROM thirteenf_amendment_backtest_trades
        WHERE is_latest_revision AND horizon_trading_days = 47
        """,
    )
    regime_rows = store.con.execute(
        """
        SELECT
            is_stress_quarter,
            count(*) FILTER (WHERE is_complete) AS completed_trades,
            avg(net_short_return) FILTER (WHERE is_complete) AS mean_net_short_return
        FROM thirteenf_amendment_backtest_trades
        WHERE is_latest_revision AND horizon_trading_days = 47
        GROUP BY is_stress_quarter
        ORDER BY is_stress_quarter
        """
    ).fetchall()
    horizon_rows = store.con.execute(
        """
        SELECT
            horizon_trading_days,
            count(*) FILTER (WHERE is_complete),
            avg(net_short_return) FILTER (WHERE is_complete),
            median(net_short_return) FILTER (WHERE is_complete),
            avg((net_short_return > 0)::INTEGER) FILTER (WHERE is_complete)
        FROM thirteenf_amendment_backtest_trades
        WHERE is_latest_revision
        GROUP BY horizon_trading_days
        ORDER BY horizon_trading_days
        """
    ).fetchall()
    archive_count = _one(
        store,
        "SELECT count(*) FROM raw_source_files WHERE dataset_id = 'sec_13f_archive' AND status = 'loaded'",
    )[0]
    return {
        "source": {
            "archive_count": int(archive_count),
            "submission_rows": int(submission_rows),
            "holding_rows": int(holding_rows),
            "minimum_report_period": min_report,
            "maximum_report_period": max_report,
            "minimum_filing_date": min_filing,
            "maximum_filing_date": max_filing,
        },
        "amendments": {
            "manager_quarter_rows": int(rate_rows),
            "amended_manager_quarters": int(amended_quarters),
            "position_corrections": int(correction_rows),
            "spike_manager_quarters": int(spike_rows),
        },
        "signals": {
            "candidate_count": int(candidate_rows),
            "signal_count": int(signal_rows),
            "years_with_signals": int(signal_years),
            "signals_per_year": float(signal_rows / signal_years) if signal_years else None,
            "mapped_signals": int(mapped_signals),
            "maximum_rank_per_quarter": DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER,
            "stress_quarters_excluded": True,
        },
        "disclosed_exits": {
            "followup_manager_positions": int(followups),
            "exited_manager_positions": int(disclosed_exits),
            "disclosed_exit_rate": float(disclosed_exits / followups) if followups else None,
            "exits_disclosed_within_47_trading_days": int(within_47),
        },
        "backtest_47d": {
            "completed_trades": int(completed_47),
            "mean_net_short_return": None if mean_47 is None else float(mean_47),
            "median_net_short_return": None if median_47 is None else float(median_47),
            "short_win_rate": None if win_47 is None else float(win_47),
            "one_way_slippage_bps": 5.0,
            "minimum_market_cap_usd": DEFAULT_MIDCAP_MIN_USD,
            "maximum_market_cap_usd": DEFAULT_MIDCAP_MAX_USD,
            "by_stress_regime": [
                {
                    "is_stress_quarter": bool(row[0]),
                    "completed_trades": int(row[1]),
                    "mean_net_short_return": None if row[2] is None else float(row[2]),
                }
                for row in regime_rows
            ],
            "horizons": [
                {
                    "trading_days": int(row[0]),
                    "completed_trades": int(row[1]),
                    "mean_net_short_return": None if row[2] is None else float(row[2]),
                    "median_net_short_return": None if row[3] is None else float(row[3]),
                    "short_win_rate": None if row[4] is None else float(row[4]),
                }
                for row in horizon_rows
            ],
        },
    }


def _percent(value: float | None) -> str:
    return "not available" if value is None else f"{100 * value:.2f}%"


def render_thirteenf_analysis_report(summary: dict[str, Any]) -> str:
    source = summary["source"]
    amendments = summary["amendments"]
    signals = summary["signals"]
    exits = summary["disclosed_exits"]
    backtest = summary["backtest_47d"]
    status = "complete" if signals["signal_count"] and backtest["completed_trades"] else "provisional"
    signals_per_year = (
        "not available"
        if signals["signals_per_year"] is None
        else f"{signals['signals_per_year']:.2f}"
    )
    regimes = backtest["by_stress_regime"]
    horizons = backtest["horizons"]
    regime_lines = (
        "\n".join(
            f"- {'Stress' if row['is_stress_quarter'] else 'Quiet'}: "
            f"{row['completed_trades']:,} completed trades, "
            f"{_percent(row['mean_net_short_return'])} mean net short return."
            for row in regimes
        )
        or "- No price-complete regime observations."
    )
    horizon_lines = "\n".join(
        f"| {row['trading_days']} | {row['completed_trades']:,} | "
        f"{_percent(row['mean_net_short_return'])} | "
        f"{_percent(row['median_net_short_return'])} | "
        f"{_percent(row['short_win_rate'])} |"
        for row in horizons
    )
    return f"""# Recreated 13F amendment-spike analysis

Status: **{status}**

This report independently implements the method described in the [original X
post]({ORIGINAL_ANALYSIS_URL}) using the [SEC Form 13F data
sets]({SEC_DATASETS_URL}). It does not assume the post's reported outcomes.

## Reproduction contract

- A RESTATEMENT replaces the prior information table; ADD NEW HOLDINGS
  supplements the latest full table, consistent with the [official Form 13F
  instructions]({SEC_FORM_URL}).
- Manager-quarter amendment rate is distinct corrected position keys divided by
  the final filed position count.
- The z-score uses only the manager's prior 24 reported quarters. A spike is at
  least 2.0 standard deviations above that trailing baseline. When all 24
  trailing rates are identical at zero, the first positive rate receives an
  explicit, rankable z-score cap of 10.0 rather than an undefined value.
- A security signal requires at least three distinct spike managers correcting
  the same CUSIP in the same report quarter. Availability is the last
  contributing amendment's filing date.
- The post says to rank signals but does not disclose a portfolio cutoff. This
  reproduction retains the top {signals['maximum_rank_per_quarter']} average-z
  candidates per quarter and excludes stress quarters as a transparent,
  capacity-constrained evaluation cohort. The raw candidate count remains
  reported so this assumption is auditable rather than fitted invisibly.
- Instrument mappings use audited [OpenFIGI v3]({OPENFIGI_DOCS_URL}) candidates.
- The price cohort is restricted at entry to US-listed common equity, ADR, or
  REIT instruments with market capitalization from
  ${backtest['minimum_market_cap_usd'] / 1_000_000_000:.0f}B to
  ${backtest['maximum_market_cap_usd'] / 1_000_000_000:.0f}B, using archived
  point-in-time shares and price.
- Backtests enter on the first price bar available strictly after the signal and
  deduct 5 bps each way. Short returns are reported at fixed trading-day
  horizons; no outcome-driven exit timing is used.

## Warehouse coverage

- SEC archives loaded: {source['archive_count']:,}
- Submissions: {source['submission_rows']:,}
- Holdings: {source['holding_rows']:,}
- Filing-date coverage: {source['minimum_filing_date']} through {source['maximum_filing_date']}
- Report-period coverage: {source['minimum_report_period']} through {source['maximum_report_period']}
- Manager-quarter observations: {amendments['manager_quarter_rows']:,}
- Amended manager-quarters: {amendments['amended_manager_quarters']:,}
- Reconstructed position corrections: {amendments['position_corrections']:,}
- 2-sigma manager-quarter spikes: {amendments['spike_manager_quarters']:,}
- Raw three-filer security-quarter candidates: {signals['candidate_count']:,}
- Selected top-{signals['maximum_rank_per_quarter']} quiet-quarter signals: {signals['signal_count']:,}

## Results versus the post

| Metric | Post claim | Independent result |
|---|---:|---:|
| Signals per year | 62 | {signals_per_year} |
| Signals / trades | ~1,700 over 11 years | {signals['signal_count']:,} signals; {backtest['completed_trades']:,} price-complete 47-day trades |
| Position leaves portfolio | 71% within 47 trading days | {_percent(exits['disclosed_exit_rate'])} at next disclosed filing |
| Net return per trade | 0.38% | {_percent(backtest['mean_net_short_return'])} at 47 trading days |
| Sharpe ratio | 3.1 | Not identifiable without the post's portfolio sizing and return-series construction |
| Average holding period | 9.4 days | Not reproducible from quarterly 13F disclosures; fixed horizons are used |

Mapped signals: {signals['mapped_signals']:,} of {signals['signal_count']:,}. The
next-filing disclosed-exit sample contains {exits['followup_manager_positions']:,}
manager/security observations and {exits['exited_manager_positions']:,} exits.
An absence in the next quarterly filing is not evidence of the exact trade date,
so the claimed 47-trading-day exit timing cannot be identified from Form 13F
alone.

## Fixed-horizon sensitivity

| Trading days | Complete trades | Mean net short return | Median | Short win rate |
|---:|---:|---:|---:|---:|
{horizon_lines}

## Stress-regime split

{regime_lines}

## Interpretation

The backtest is point-in-time with respect to public filing and price
availability. It is still subject to CUSIP-to-ticker coverage, quarterly
disclosure latency, confidential-treatment omissions, manager identity changes,
and the limits of the available daily-price archive. These coverage figures are
part of the result rather than silently dropped. The near-exact 62-signals/year
match is a consequence of the disclosed quiet-quarter rule plus the explicit
top-20 capacity assumption; it is not independent evidence for the post's
undisclosed implementation.
"""


def write_thirteenf_analysis_report(
    store: DuckDBStore,
    output_path: Path,
    *,
    json_path: Path | None = None,
) -> dict[str, Any]:
    summary = thirteenf_analysis_summary(store)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(render_thirteenf_analysis_report(summary), encoding="utf-8")
    if json_path is not None:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(summary, default=str, indent=2, sort_keys=True), encoding="utf-8")
    return summary
