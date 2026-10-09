#!/usr/bin/env python3
"""Plot the one-day P&L distribution exported by atx-vol-projection-bench.

The historical scenarios are alternative observations for one fixed reference
portfolio. Their sum is not a realizable equity curve, so this report never
plots cumulative scenario P&L.
"""

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
from matplotlib.ticker import FuncFormatter


@dataclass(frozen=True)
class PnlRow:
    base_date: date
    shifted_date: date
    base_value: float
    shifted_value: float
    pnl: float
    n_positions: int
    source_option_lots: int
    coverage_excluded_option_lots: int
    delta_boundary_excluded_option_lots: int
    replay_excluded_option_lots: int
    stock_hedges: int


@dataclass(frozen=True)
class LossStatistics:
    confidence: float
    value_at_risk: float
    expected_shortfall: float
    n_scenarios: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot an atx-vol fixed-portfolio historical VaR trace."
    )
    parser.add_argument("trace", type=Path, help="tab-separated benchmark trace")
    parser.add_argument("output", type=Path, help="PNG output path")
    parser.add_argument(
        "--title",
        default="One-Day Historical P&L — 2026-07-31 SP100 Dispersion Portfolio",
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


def historical_loss_statistics(rows: list[PnlRow], confidence: float) -> LossStatistics:
    """Match the engine's nearest-rank VaR and inclusive-tail ES convention."""
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be in (0, 1)")
    if not rows:
        raise ValueError("P&L trace is empty")
    losses = sorted(-row.pnl for row in rows)
    index = min(len(losses) - 1, max(0, math.ceil(confidence * len(losses)) - 1))
    tail = losses[index:]
    return LossStatistics(confidence, losses[index], sum(tail) / len(tail), len(losses))


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


def count_history_breaks(rows: list[PnlRow]) -> int:
    return sum(
        rows[index - 1].shifted_date != rows[index].base_date
        for index in range(1, len(rows))
    )


def plot(rows: list[PnlRow], output: Path, title: str) -> None:
    pnl = np.asarray([row.pnl for row in rows], dtype=float)
    dates = [row.shifted_date for row in rows]
    var95 = historical_loss_statistics(rows, 0.95)
    var99 = historical_loss_statistics(rows, 0.99)

    plt.style.use("seaborn-v0_8-whitegrid")
    figure, (timeline, distribution) = plt.subplots(
        2,
        1,
        figsize=(13.5, 9.0),
        gridspec_kw={"height_ratios": [2.1, 1.0], "hspace": 0.30},
    )

    colors = np.where(pnl < 0.0, "#B33A3A", "#1261A0")
    timeline.bar(dates, pnl, width=1.8, color=colors, alpha=0.88)
    timeline.axhline(0.0, color="#333333", linewidth=0.9)
    timeline.set_ylabel("One-day scenario P&L")
    timeline.yaxis.set_major_formatter(FuncFormatter(dollar_axis))
    timeline.xaxis.set_major_locator(mdates.MonthLocator())
    timeline.xaxis.set_major_formatter(mdates.DateFormatter("%b %Y"))
    timeline.grid(axis="x", alpha=0.18)
    timeline.grid(axis="y", alpha=0.34)

    bins = min(30, max(8, int(math.sqrt(len(rows)) * 2)))
    distribution.hist(pnl, bins=bins, color="#6B7C93", alpha=0.82)
    distribution.axvline(
        -var95.value_at_risk,
        color="#E08A1E",
        linewidth=1.8,
        linestyle="--",
        label=f"95% loss cutoff: {dollar_axis(var95.value_at_risk, 0.0)}",
    )
    distribution.axvline(
        -var99.value_at_risk,
        color="#B33A3A",
        linewidth=1.8,
        linestyle="--",
        label=f"99% loss cutoff: {dollar_axis(var99.value_at_risk, 0.0)}",
    )
    distribution.set_xlabel("One-day scenario P&L")
    distribution.set_ylabel("Observations")
    distribution.xaxis.set_major_formatter(FuncFormatter(dollar_axis))
    distribution.legend(loc="upper left", frameon=False, fontsize=9.5)
    distribution.grid(axis="x", alpha=0.25)
    distribution.grid(axis="y", alpha=0.15)

    for axis in (timeline, distribution):
        axis.spines["top"].set_visible(False)
        axis.spines["right"].set_visible(False)

    first = rows[0]
    excluded = (
        first.coverage_excluded_option_lots
        + first.delta_boundary_excluded_option_lots
        + first.replay_excluded_option_lots
    )
    portfolio_label = f"{first.n_positions:,} replayed positions"
    if first.source_option_lots != 0:
        portfolio_label += f" ({excluded:,} source options excluded)"
    stats_line = (
        f"{portfolio_label} | {len(rows)} adjacent-session scenarios | "
        f"{count_history_breaks(rows)} history breaks | "
        f"worst {dollar_axis(float(np.min(pnl)), 0.0)} | "
        f"best {dollar_axis(float(np.max(pnl)), 0.0)}"
    )
    disclosure_line = (
        "Each scenario projects the reference option profile to its historical base "
        "market, keeps terminal contract quantities and hedge shares fixed, then reprices "
        "one session forward. Scenarios are alternative VaR observations; summing them is "
        "not a held-portfolio equity curve. Model mids, no transaction costs."
    )

    figure.subplots_adjust(top=0.82, left=0.08, right=0.975, bottom=0.08)
    figure.text(
        0.045,
        0.970,
        title,
        fontsize=16,
        fontweight="bold",
        color="#111111",
        ha="left",
        va="top",
    )
    figure.text(
        0.045, 0.925, stats_line, fontsize=10, color="#555555", ha="left", va="top"
    )
    figure.text(
        0.045,
        0.895,
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
    var99 = historical_loss_statistics(rows, 0.99)
    print(
        f"wrote {args.output} | rows={len(rows)} | "
        f"var99={var99.value_at_risk:.2f} | es99={var99.expected_shortfall:.2f}"
    )


if __name__ == "__main__":
    main()
