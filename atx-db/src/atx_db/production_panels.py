"""Shared production dispatch for activation and scheduled factor/return panels."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, replace

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .delisting import (
    DelistingCodeReconciliationOptions,
    DelistingTerminalReturnOptions,
    ShumwayPerformancePolicy,
    SurvivorshipSafeForwardReturnOptions,
    reconcile_delisting_codes,
    refresh_delisting_terminal_returns,
    refresh_survivorship_safe_forward_returns,
    survivorship_forward_return_diagnostics,
)
from .derived_compatibility_parents import refresh_compatibility_parents
from .derived_factor_projection import (
    _GRID_SQL,
    FactorProjectionOptions,
    default_projections,
    refresh_projected_factor_values,
)
from .item_coverage import ItemCoverageOptions, measure_item_coverage, refresh_item_coverage
from .item_coverage_cohort import AnnualCoverageCohortOptions, refresh_item_coverage_cohort

# Job names use the existing operator aliases, while DATASET_DEPENDENCIES uses
# dataset IDs. These seeds add missing production paths without replacing user jobs.
PRODUCTION_JOB_SEEDS = (
    ("derived_metrics", "derived_metrics", ("fundamental_standardized",)),
    ("market_daily", "market_daily", ("derived_metrics", "daily_bars")),
    ("legacy_liquid_universe", "universe_membership", ("daily_bars", "listing_status_intervals")),
    ("factor_projections", "derived_factor_projection", ("legacy_liquid_universe", "derived_metrics", "market_daily")),
    ("delisting_evidence", "delisting_evidence", ("daily_bars", "nasdaq_listing_events", "sec_submissions")),
    ("universe_us_listed", "universe_us_listed", ("market_daily", "daily_bars")),
    ("delisting_terminal_returns", "delisting_terminal_returns", ("delisting_evidence", "universe_us_listed")),
    ("survivorship_forward_returns", "forward_returns_survivorship_safe", ("trading_calendar", "delisting_terminal_returns")),
    ("item_coverage", "fundamental_item_coverage", ("universe_us_listed", "market_daily", "fundamental_standardized")),
)


@dataclass(frozen=True)
class ProductionPanelOptions:
    as_of_date: dt.date
    run_id: str | None = None


class _ProductionDataset(Dataset):
    source_name = "atx-db production panels"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()


class DerivedFactorProjectionDataset(_ProductionDataset):
    dataset_id = "derived_factor_projection"

    def load(self, store: DuckDBStore, options: FactorProjectionOptions) -> DatasetLoadResult:
        predicates: list[str] = []
        params: list[object] = []
        for operator, value in ((">=", options.start_date), ("<=", options.end_date)):
            if value is not None:
                predicates.append(f"AND trade_date {operator} ?")
                params.append(value)
        params.append(options.universe_id)
        grid_sql = _GRID_SQL.format(date_predicate=" ".join(predicates), end_of_day=22)
        cohorts = store.con.execute(grid_sql + "SELECT trade_date,count(*) FROM grid GROUP BY 1 ORDER BY 1",
                                   params).fetchall()
        selected = [p for p in default_projections()
                    if (options.factor_ids is None and p.retired_module != "KEPT")
                    or (options.factor_ids is not None and p.factor_id in options.factor_ids)]
        detail: dict[str, object] = {
            "universe_id": options.universe_id, "selected_factor_count": len(selected),
            "eligible_rebalance_dates": len(cohorts),
            "eligible_security_dates": sum(int(n) for _, n in cohorts),
            "cohort_counts": [{"date": str(day), "names": n} for day, n in cohorts],
            "insufficient_peer_dates": {
                p.factor_id: sum(n < p.minimum_names_per_date for _, n in cohorts) for p in selected
            },
        }
        if not cohorts or not selected:
            # Empty scope must never dispatch parents whose empty filters mean all data,
            # nor erase the prior projection publication as if continuity were proved.
            detail.update(outcome="degraded", reason="empty governed rebalance cohort or factor selection",
                          parents_skipped=True, previous_projection_rows_preserved=True)
            return DatasetLoadResult(self.dataset_id, 0, options.source, detail)
        rows = 0
        parent_counts: dict[str, int] = {}
        # Date partitions retain complete cross-sectional peer sets. This bounds
        # the existing pandas parent/projection writers without changing a factor's
        # monthly population or its filing lookbacks (QOP adds its own 600-day buffer).
        years = sorted({day.year for day, _ in cohorts})
        for year in years:
            scoped = replace(options,
                start_date=max(dt.date(year, 1, 1), options.start_date or dt.date(year, 1, 1)),
                end_date=min(dt.date(year, 12, 31), options.end_date or dt.date(year, 12, 31)))
            parents = refresh_compatibility_parents(store, scoped)
            for name, count in parents.row_counts.items():
                parent_counts[name] = parent_counts.get(name, 0) + count
            rows += refresh_projected_factor_values(store, scoped)
        factor_ids = [p.factor_id for p in selected]
        counts = store.con.execute(
            "SELECT factor_id,as_of_date,count(*) FROM fundamental_factor_values "
            "WHERE source=? AND factor_id IN (SELECT unnest(?)) AND is_latest_revision "
            "AND (? IS NULL OR as_of_date>=?) AND (? IS NULL OR as_of_date<=?) GROUP BY 1,2 ORDER BY 1,2",
            [options.source, factor_ids, options.start_date, options.start_date, options.end_date, options.end_date],
        ).fetchall()
        detail.update(parent_rows_written=parent_counts,
                      empty_parents=tuple(name for name, n in parent_counts.items() if not n),
                      sequential_year_partitions=years,
                      factor_date_counts=[{"factor_id": factor, "date": str(day), "rows": n}
                                          for factor, day, n in counts],
                      missing_factors=sorted(set(factor_ids)-{factor for factor, _, _ in counts}),
                      outcome="measured" if rows else "degraded")
        return DatasetLoadResult(self.dataset_id, rows, options.source, detail)


class DelistingTerminalDataset(_ProductionDataset):
    dataset_id = "delisting_terminal_returns"

    def load(self, store: DuckDBStore, options: ProductionPanelOptions) -> DatasetLoadResult:
        rows = refresh_delisting_terminal_returns(store, DelistingTerminalReturnOptions(
            run_id=options.run_id, performance_delisting_return=ShumwayPerformancePolicy(),
        ))
        reconciled = reconcile_delisting_codes(store, DelistingCodeReconciliationOptions(run_id=options.run_id))
        missing = store.con.execute(
            "SELECT coalesce(c.reason_category,'unmapped'),count(*) FROM delisting_events e "
            "LEFT JOIN delist_code_dim c USING(delist_code) WHERE e.available_at<=? "
            "AND NOT EXISTS (SELECT 1 FROM delisting_terminal_returns t WHERE t.security_id=e.security_id "
            "AND t.delist_date=e.delist_date AND t.available_at<=? AND isfinite(t.terminal_return)) GROUP BY 1",
            [dt.datetime.combine(options.as_of_date, dt.time(22))]*2,
        ).fetchall()
        return DatasetLoadResult(self.dataset_id, rows, self.source_name,
                                 {"reconciliation_rows": reconciled, "uncovered_by_reason": dict(missing),
                                  "policy": "Shumway default: NASDAQ -0.55, other performance -0.30; observed wins"})


class SurvivorshipForwardDataset(_ProductionDataset):
    dataset_id = "forward_returns_survivorship_safe"

    def load(self, store: DuckDBStore, options: ProductionPanelOptions) -> DatasetLoadResult:
        forward_options = SurvivorshipSafeForwardReturnOptions(
            run_id=options.run_id, price_basis="adjusted_close",
            observation_cutoff=dt.datetime.combine(options.as_of_date, dt.time(22)),
        )
        rows = refresh_survivorship_safe_forward_returns(store, forward_options)
        detail = survivorship_forward_return_diagnostics(store, forward_options)
        return DatasetLoadResult(self.dataset_id, rows, self.source_name, detail)


class ItemCoverageDataset(_ProductionDataset):
    dataset_id = "fundamental_item_coverage"

    def load(self, store: DuckDBStore, options: ProductionPanelOptions) -> DatasetLoadResult:
        cohort = refresh_item_coverage_cohort(store, AnnualCoverageCohortOptions(
            as_of_date=options.as_of_date, run_id=options.run_id,
        ))
        coverage_options = ItemCoverageOptions(as_of_date=options.as_of_date, run_id=options.run_id)
        frame = measure_item_coverage(store, coverage_options)
        rows = refresh_item_coverage(store, coverage_options, frame=frame)
        return DatasetLoadResult(self.dataset_id, rows, self.source_name,
                                 {"cohort": cohort, "as_of_date": options.as_of_date.isoformat(),
                                  "schema_conditions_changed": False})
