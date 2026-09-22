"""Refresh retained lineage inputs needed by legacy-compatible projections."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import AssetGrowthOptions, refresh_asset_growth_values
from .cash_flow_profitability import CashFlowProfitabilityOptions, refresh_cash_flow_profitability_values
from .connection import DuckDBStore
from .derived_factor_projection import FactorProjectionOptions, default_projections
from .fundamental_signals import FundamentalSignalOptions, refresh_fundamental_signal_values
from .quarterly_cash_profitability import QuarterlyCashProfitabilityOptions, refresh_quarterly_cash_profitability_values
from .quarterly_operating_profitability import (
    QuarterlyOperatingProfitabilityOptions,
    refresh_quarterly_operating_profitability_values,
)
from .quarterly_revenue_growth import QuarterlyRevenueGrowthOptions, refresh_quarterly_revenue_growth_values


@dataclass(frozen=True)
class CompatibilityParentResult:
    """Counts are rows refreshed in the recorded parent date scopes.

    Zero counts are explicit diagnostics, not success evidence for publication.
    An empty governed cohort yields no parent/projection rows. The caller owns
    coverage policy; this helper never fabricates membership or minimum breadth.
    """

    row_counts: dict[str, int]
    selected_factor_count: int
    history_start_date: dt.date | None

    @property
    def empty_parents(self) -> tuple[str, ...]:
        return tuple(name for name, count in self.row_counts.items() if count == 0)


def refresh_compatibility_parents(
    store: DuckDBStore,
    options: FactorProjectionOptions | None = None,
) -> CompatibilityParentResult:
    """Refresh only retained prerequisites for the requested retirement factors.

    Input prerequisites are statement points, TTM points, share history, bars and
    an already-built legacy liquid cohort. No deprecated valuation table is read
    or refreshed. The QOP history starts 600 calendar days before a scoped start:
    same-quarter revenue pairs need a prior published observation whose fiscal
    period is up to 400 days earlier, plus reporting delay/monthly grid slack.
    All other parents and final projection retain the requested date scope.
    """

    options = options or FactorProjectionOptions()
    if options.start_date and options.end_date and options.start_date > options.end_date:
        raise ValueError("start_date must not be after end_date")
    wanted = set(options.factor_ids) if options.factor_ids is not None else None
    selected = [p for p in default_projections() if p.source_window == "legacy"
                and (wanted is None or p.factor_id in wanted)]
    codes = {p.metric_code for p in selected}
    counts: dict[str, int] = {}
    history_start = options.start_date-dt.timedelta(days=600) if options.start_date else None
    if codes.intersection({
        "legacy_beneish_m", "legacy_external_financing", "legacy_net_debt_financing",
        "legacy_large_rd_increase", "legacy_tax_expense_momentum", "legacy_tax_to_book_income",
    }):
        counts["asset_growth"] = refresh_asset_growth_values(store, AssetGrowthOptions(
            start_date=options.start_date, end_date=options.end_date,
            universe_id=options.universe_id, run_id=options.run_id,
        ))
    if codes.intersection({"legacy_altman_z", "legacy_gross_profit_to_ev"}):
        counts["fundamental_signals"] = refresh_fundamental_signal_values(store, FundamentalSignalOptions(
            start_date=options.start_date, end_date=options.end_date, run_id=options.run_id,
        ))
    if codes.intersection({"legacy_altman_z", "legacy_cfo_to_ev"}):
        counts["cash_flow_profitability"] = refresh_cash_flow_profitability_values(store, CashFlowProfitabilityOptions(
            start_date=options.start_date, end_date=options.end_date,
            universe_id=options.universe_id, run_id=options.run_id,
        ))
    qop_codes = {"legacy_quarterly_working_capital_accruals", "legacy_quarterly_gross_margin_change",
                 "legacy_quarterly_profitability_change"}
    if codes.intersection(qop_codes):
        counts["quarterly_operating_profitability"] = refresh_quarterly_operating_profitability_values(
            store, QuarterlyOperatingProfitabilityOptions(
                start_date=history_start, end_date=options.end_date,
                universe_id=options.universe_id, run_id=options.run_id,
            ),
        )
    if "legacy_quarterly_working_capital_accruals" in codes:
        counts["quarterly_cash_profitability"] = refresh_quarterly_cash_profitability_values(
            store, QuarterlyCashProfitabilityOptions(
                start_date=options.start_date, end_date=options.end_date, run_id=options.run_id,
            ),
        )
    if codes.intersection(qop_codes-{"legacy_quarterly_working_capital_accruals"}):
        counts["quarterly_revenue_growth"] = refresh_quarterly_revenue_growth_values(store, QuarterlyRevenueGrowthOptions(
            start_date=options.start_date, end_date=options.end_date, run_id=options.run_id,
        ))
    return CompatibilityParentResult(counts, len(selected), history_start)
