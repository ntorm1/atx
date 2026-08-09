#!/usr/bin/env python3
"""Plot the cumulative P&L trace exported by atx-vol-projection-bench."""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from datetime import date
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.ticker import FuncFormatter


@dataclass(frozen=True)
class PnlRow:
    base_date: date
    shifted_date: date
    base_value: float
    shifted_value: float
    pnl: float
    cumulative_pnl: float
    n_positions: int
    source_option_lots: int
    coverage_excluded_option_lots: int
    delta_boundary_excluded_option_lots: int
    replay_excluded_option_lots: int
    stock_hedges: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot an atx-vol historical VaR cumulative P&L trace."
    )
    parser.add_argument("trace", type=Path, help="tab-separated benchmark trace")
    parser.add_argument("output", type=Path, help="PNG output path")
    parser.add_argument(
        "--title",
        default=(
            "Characteristic-Restruck Historical Replay — "
            "2026-07-31 SP100 Dispersion Profile"
        ),
    )
    return parser.parse_args()


def read_trace(path: Path) -> list[PnlRow]:
    with path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        rows = [
            PnlRow(
                base_date=date.fromisoformat(row["base_date"]),
                shifted_date=date.fromisoformat(row["shifted_date"]),
                base_value=float(row["base_value"]),
                shifted_value=float(row["shifted_value"]),
                pnl=float(row["pnl"]),
                cumulative_pnl=float(row["cumulative_pnl"]),
                n_positions=int(row["n_positions"]),
                source_option_lots=int(row.get("source_option_lots", 0)),
                coverage_excluded_option_lots=int(
                    row.get("coverage_excluded_option_lots", 0)
                ),
                delta_boundary_excluded_option_lots=int(
                    row.get("delta_boundary_excluded_option_lots", 0)
                ),
                replay_excluded_option_lots=int(
                    row.get("replay_excluded_option_lots", 0)
                ),
                stock_hedges=int(row.get("stock_hedges", 0)),
            )
            for row in reader
        ]
    if not rows:
        raise ValueError(f"P&L trace is empty: {path}")
    return rows


def compute_decomposition(df: pd.DataFrame) -> pd.DataFrame:
    """Decompose cumulative restruck-scenario P&L into resets vs. held drift.

    The engine restrikes every option to its reference |delta| and relative
    time-to-expiry, and re-sizes to the reference dollar delta, on every
    scenario's base date (see docs/historical-var-engine-status.md). Summing
    the resulting per-scenario P&L therefore compounds two effects: the
    re-basing reset between the one-session-aged book and the freshly
    restruck book on the same date, and the genuine revaluation drift of
    holding a restruck-once profile across every adjacent pair of scenarios.
    This function separates them.

    ``rebasing_reset[i] = base_value[i+1] - shifted_value[i]`` for every
    adjacent pair (i, i+1) — both chained pairs (``shifted_date[i] ==
    base_date[i+1]``, i.e. no history break between them) and break pairs
    alike. It is NaN only on the trailing row (no i+1 to pair against). The
    boolean ``chained`` column records which is which, for annotation: at a
    break, ``rebasing_reset`` conflates the break's own market move with the
    re-basing jump (accepted convention; see docs/historical-var-engine-status.md).

    ``cumulative_held_drift[k] = cumulative_pnl[k] + sum(rebasing_reset[i]
    for i < k)`` (plain sum, no NaN skipping — the only NaN is at the
    trailing row, never included as a "prior" term). Since
    ``base_value[i+1] - base_value[i] == pnl[i] + rebasing_reset[i]`` holds
    for every pair, this telescopes to ``cumulative_held_drift[k] ==
    shifted_value[k] - base_value[0]`` — the value change of holding a
    restruck-once profile from the first base date through scenario k.

    Does not mutate ``df``.

    Args:
        df: columns ``base_date``, ``shifted_date``, ``base_value``,
            ``shifted_value``, ``pnl``, ``cumulative_pnl``.

    Returns:
        A copy of ``df`` with ``rebasing_reset``, ``chained``, and
        ``cumulative_held_drift`` columns added.
    """
    result = df.copy()
    n = len(result)

    rebasing_reset = np.full(n, np.nan)
    chained = pd.array([pd.NA] * n, dtype="boolean")
    if n > 1:
        base_date = result["base_date"].to_numpy()
        shifted_date = result["shifted_date"].to_numpy()
        base_value = result["base_value"].to_numpy(dtype=float)
        shifted_value = result["shifted_value"].to_numpy(dtype=float)
        rebasing_reset[:-1] = base_value[1:] - shifted_value[:-1]
        chained[:-1] = shifted_date[:-1] == base_date[1:]
    result["rebasing_reset"] = rebasing_reset
    result["chained"] = chained

    # Running total of resets strictly before row k. A plain cumsum: the
    # only NaN in rebasing_reset is at the trailing row, and it is never
    # included as a "prior" term (dropped by the [:-1] slice below), so it
    # never needs NaN-skipping.
    reset_running_total = np.cumsum(rebasing_reset)
    prior_reset_sum = np.concatenate(([0.0], reset_running_total[:-1]))
    result["cumulative_held_drift"] = (
        result["cumulative_pnl"].to_numpy(dtype=float) + prior_reset_sum
    )
    return result


def rows_to_frame(rows: list[PnlRow]) -> pd.DataFrame:
    """Convert parsed trace rows into the frame compute_decomposition expects."""
    return pd.DataFrame(
        {
            "base_date": [row.base_date for row in rows],
            "shifted_date": [row.shifted_date for row in rows],
            "base_value": [row.base_value for row in rows],
            "shifted_value": [row.shifted_value for row in rows],
            "pnl": [row.pnl for row in rows],
            "cumulative_pnl": [row.cumulative_pnl for row in rows],
        }
    )


def dollar_axis(value: float, _position: float) -> str:
    absolute = abs(value)
    sign = "-" if value < 0.0 else ""
    if absolute >= 1_000_000_000.0:
        return f"{sign}${absolute / 1_000_000_000.0:.1f}B"
    if absolute >= 1_000_000.0:
        return f"{sign}${absolute / 1_000_000.0:.1f}M"
    if absolute >= 1_000.0:
        return f"{sign}${absolute / 1_000.0:.0f}K"
    return f"{sign}${absolute:.0f}"


def drawdown(rows: list[PnlRow]) -> float:
    peak = 0.0
    maximum = 0.0
    for row in rows:
        peak = max(peak, row.cumulative_pnl)
        maximum = min(maximum, row.cumulative_pnl - peak)
    return maximum


def plot(rows: list[PnlRow], output: Path, title: str) -> None:
    held_drift = compute_decomposition(rows_to_frame(rows))["cumulative_held_drift"].tolist()

    dates: list[date] = [rows[0].base_date]
    values: list[float] = [0.0]
    held_values: list[float] = [0.0]
    previous_shifted: date | None = None
    previous_cumulative = 0.0
    previous_held = 0.0
    gaps = 0

    for row, held in zip(rows, held_drift):
        if previous_shifted is not None and row.base_date != previous_shifted:
            dates.append(row.base_date)
            values.append(math.nan)
            held_values.append(math.nan)
            dates.append(row.base_date)
            values.append(previous_cumulative)
            held_values.append(previous_held)
            gaps += 1
        dates.append(row.shifted_date)
        values.append(row.cumulative_pnl)
        held_values.append(held)
        previous_shifted = row.shifted_date
        previous_cumulative = row.cumulative_pnl
        previous_held = held

    plt.style.use("seaborn-v0_8-whitegrid")
    figure, axis = plt.subplots(figsize=(13.5, 8.2))
    axis.axhline(0.0, color="#333333", linewidth=0.9, alpha=0.75)

    # The gap between the total and held-drift lines is the cumulative
    # re-basing reset: the value jump from restriking/re-sizing the book
    # every session, which is not held-book drift. Shade it so the two
    # honest readings (total restruck-scenario compounding vs. genuine
    # revaluation drift) are visually separated, not just line-labeled.
    reset_band = [
        not (math.isnan(total) or math.isnan(held))
        for total, held in zip(values, held_values)
    ]
    axis.fill_between(
        dates,
        values,
        held_values,
        where=reset_band,
        color="#EB6834",
        alpha=0.20,
        interpolate=True,
        label="Cumulative re-basing resets (remainder)",
        zorder=1,
    )

    axis.plot(
        dates,
        values,
        color="#1261A0",
        linewidth=2.0,
        label="Cumulative restruck-scenario P&L (total)",
        zorder=3,
    )
    axis.plot(
        dates,
        held_values,
        color="#4A3AA7",
        linewidth=1.8,
        linestyle="--",
        label="Held-profile revaluation drift",
        zorder=2,
    )

    endpoint = rows[-1]
    axis.scatter(
        [endpoint.shifted_date],
        [endpoint.cumulative_pnl],
        color="#1261A0",
        edgecolor="white",
        linewidth=1.1,
        s=55,
        zorder=4,
    )
    axis.annotate(
        dollar_axis(endpoint.cumulative_pnl, 0.0),
        (endpoint.shifted_date, endpoint.cumulative_pnl),
        xytext=(-10, 12),
        textcoords="offset points",
        ha="right",
        fontsize=10,
        fontweight="bold",
        color="#1261A0",
    )
    axis.scatter(
        [endpoint.shifted_date],
        [held_drift[-1]],
        color="#4A3AA7",
        edgecolor="white",
        linewidth=1.1,
        s=45,
        zorder=4,
    )
    axis.annotate(
        dollar_axis(held_drift[-1], 0.0),
        (endpoint.shifted_date, held_drift[-1]),
        xytext=(-10, -14),
        textcoords="offset points",
        ha="right",
        fontsize=10,
        fontweight="bold",
        color="#4A3AA7",
    )

    first = rows[0]
    excluded = (
        first.coverage_excluded_option_lots
        + first.delta_boundary_excluded_option_lots
        + first.replay_excluded_option_lots
    )
    portfolio_label = f"{first.n_positions:,} replayable positions"
    if first.source_option_lots != 0:
        portfolio_label += f" ({excluded:,} source options excluded)"
    stats_line = (
        f"{portfolio_label} | {len(rows)} adjacent-session scenarios | "
        f"{gaps} visible history breaks | max drawdown {dollar_axis(drawdown(rows), 0.0)}"
    )
    disclosure_line = (
        "Each scenario restrikes every option to its reference |delta| and relative expiry, "
        "re-sizes to the reference dollar delta, and reprices on the shifted surface: "
        "cumulative P&L compounds per-session restruck scenarios, not a held book. Model "
        "mids, no transaction costs."
    )

    axis.set_xlabel("Shifted market date")
    axis.set_ylabel("Cumulative P&L")
    axis.yaxis.set_major_formatter(FuncFormatter(dollar_axis))
    axis.xaxis.set_major_locator(mdates.MonthLocator())
    axis.xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
    axis.grid(axis="x", alpha=0.22)
    axis.grid(axis="y", alpha=0.34)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)
    axis.legend(loc="upper left", frameon=False, fontsize=9.5)

    # Header block (title, stats, disclosure) is placed in figure-fraction
    # coordinates with a manually reserved top margin, rather than fighting
    # constrained_layout for space above the axes: constrained_layout only
    # tracks the Axes title artist, not arbitrary text, so multi-line
    # disclosure text above the axes would otherwise overlap the title.
    figure.subplots_adjust(top=0.80, left=0.07, right=0.975, bottom=0.09)
    figure.text(
        0.045,
        0.965,
        title,
        fontsize=16,
        fontweight="bold",
        color="#111111",
        ha="left",
        va="top",
    )
    figure.text(
        0.045,
        0.915,
        stats_line,
        fontsize=10,
        color="#555555",
        ha="left",
        va="top",
    )
    figure.text(
        0.045,
        0.888,
        disclosure_line,
        fontsize=9,
        color="#555555",
        ha="left",
        va="top",
        style="italic",
        wrap=True,
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, dpi=200, facecolor="white")
    plt.close(figure)


def main() -> None:
    args = parse_args()
    rows = read_trace(args.trace)
    plot(rows, args.output, args.title)
    print(
        f"wrote {args.output} | rows={len(rows)} | "
        f"cumulative_pnl={rows[-1].cumulative_pnl:.2f} | "
        f"max_drawdown={drawdown(rows):.2f}"
    )


if __name__ == "__main__":
    main()
