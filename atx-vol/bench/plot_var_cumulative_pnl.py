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
from matplotlib.ticker import FuncFormatter


@dataclass(frozen=True)
class PnlRow:
    base_date: date
    shifted_date: date
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
        default="Historical Replay of the 2026-07-31 SP100 Dispersion Portfolio",
    )
    return parser.parse_args()


def read_trace(path: Path) -> list[PnlRow]:
    with path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        rows = [
            PnlRow(
                base_date=date.fromisoformat(row["base_date"]),
                shifted_date=date.fromisoformat(row["shifted_date"]),
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
    dates: list[date] = [rows[0].base_date]
    values: list[float] = [0.0]
    previous_shifted: date | None = None
    previous_cumulative = 0.0
    gaps = 0

    for row in rows:
        if previous_shifted is not None and row.base_date != previous_shifted:
            dates.append(row.base_date)
            values.append(math.nan)
            dates.append(row.base_date)
            values.append(previous_cumulative)
            gaps += 1
        dates.append(row.shifted_date)
        values.append(row.cumulative_pnl)
        previous_shifted = row.shifted_date
        previous_cumulative = row.cumulative_pnl

    plt.style.use("seaborn-v0_8-whitegrid")
    figure, axis = plt.subplots(figsize=(13.5, 7.4), constrained_layout=True)
    axis.plot(dates, values, color="#1261A0", linewidth=2.0)
    axis.axhline(0.0, color="#333333", linewidth=0.9, alpha=0.75)
    axis.fill_between(
        dates,
        values,
        0.0,
        where=[not math.isnan(value) and value >= 0.0 for value in values],
        color="#2A9D8F",
        alpha=0.14,
        interpolate=True,
    )
    axis.fill_between(
        dates,
        values,
        0.0,
        where=[not math.isnan(value) and value < 0.0 for value in values],
        color="#D1495B",
        alpha=0.14,
        interpolate=True,
    )

    endpoint = rows[-1]
    axis.scatter(
        [endpoint.shifted_date],
        [endpoint.cumulative_pnl],
        color="#1261A0",
        edgecolor="white",
        linewidth=1.1,
        s=55,
        zorder=3,
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

    axis.set_title(title, loc="left", fontsize=16, fontweight="bold", pad=18)
    first = rows[0]
    excluded = (
        first.coverage_excluded_option_lots
        + first.delta_boundary_excluded_option_lots
        + first.replay_excluded_option_lots
    )
    portfolio_label = f"{first.n_positions:,} replayable positions"
    if first.source_option_lots != 0:
        portfolio_label += f" ({excluded:,} source options excluded)"
    subtitle = (
        f"{portfolio_label} | {len(rows)} adjacent-session scenarios | "
        f"{gaps} visible history breaks | max drawdown {dollar_axis(drawdown(rows), 0.0)}"
    )
    axis.text(
        0.0,
        1.015,
        subtitle,
        transform=axis.transAxes,
        fontsize=10,
        color="#555555",
        va="bottom",
    )
    axis.set_xlabel("Shifted market date")
    axis.set_ylabel("Cumulative replay P&L")
    axis.yaxis.set_major_formatter(FuncFormatter(dollar_axis))
    axis.xaxis.set_major_locator(mdates.MonthLocator())
    axis.xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
    axis.grid(axis="x", alpha=0.22)
    axis.grid(axis="y", alpha=0.34)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)

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
