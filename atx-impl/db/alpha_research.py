from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, now_utc_naive, quality_check, symbol_key


SOURCE_NAME = "atx-impl alpha research engine"


@dataclass(frozen=True)
class AlphaExpressionSpec:
    alpha_id: str
    alpha_name: str
    description: str
    expression_sql: str
    input_features: tuple[str, ...]
    signal_sql: str
    direction: int = 1


DEFAULT_ALPHA_SPECS: tuple[AlphaExpressionSpec, ...] = (
    AlphaExpressionSpec(
        alpha_id="alpha_momentum_liquidity_v1",
        alpha_name="Momentum liquidity",
        description="Cross-sectional signal combining 21-day momentum and average dollar-volume ranks.",
        expression_sql=(
            "zscore_cs(mom_21d) + 0.25 * zscore_cs(log1p(adv_21d))"
        ),
        input_features=("mom_21d", "adv_21d"),
        signal_sql=(
            "coalesce(mom_21d_z, 0.0) + 0.25 * coalesce(adv_21d_log_z, 0.0)"
        ),
    ),
    AlphaExpressionSpec(
        alpha_id="alpha_reversal_low_vol_v1",
        alpha_name="Reversal low volatility",
        description="Cross-sectional signal favoring short-term reversal with lower trailing volatility.",
        expression_sql="-zscore_cs(mom_5d) - zscore_cs(vol_21d)",
        input_features=("mom_5d", "vol_21d"),
        signal_sql="-coalesce(mom_5d_z, 0.0) - coalesce(vol_21d_z, 0.0)",
    ),
    AlphaExpressionSpec(
        alpha_id="alpha_trend_quality_v1",
        alpha_name="Trend quality",
        description="Cross-sectional signal rewarding medium-term trend while penalizing noisy one-day moves.",
        expression_sql="zscore_cs(mom_63d) - 0.5 * abs(zscore_cs(ret_1d))",
        input_features=("mom_63d", "ret_1d"),
        signal_sql="coalesce(mom_63d_z, 0.0) - 0.5 * abs(coalesce(ret_1d_z, 0.0))",
    ),
)


@dataclass(frozen=True)
class AlphaResearchOptions:
    symbols: tuple[str, ...] | None = None
    feature_set: str = "equity_daily_v1"
    universe_id: str = "us_liquid_equity_v1"
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    horizon_days: int = 1
    top_quantile: float = 0.2
    bottom_quantile: float = 0.2
    min_cross_section: int = 5
    run_id: str | None = None


def _hash_id(prefix: str, *parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(f"{prefix}|{payload}".encode("utf-8")).hexdigest()


def _target_symbols_sql(symbols: tuple[str, ...] | None) -> tuple[str, pd.DataFrame | None]:
    if not symbols:
        return "", None
    normalized = sorted({symbol_key(symbol) for symbol in symbols if symbol_key(symbol)})
    return "JOIN alpha_symbol_filter sf ON sf.symbol = fv.symbol", pd.DataFrame({"symbol": normalized})


def _alpha_catalog_frame(options: AlphaResearchOptions) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for spec in DEFAULT_ALPHA_SPECS:
        rows.append(
            {
                "alpha_id": spec.alpha_id,
                "alpha_name": spec.alpha_name,
                "description": spec.description,
                "expression_sql": spec.expression_sql,
                "feature_set": options.feature_set,
                "input_features_json": json_dumps(list(spec.input_features)),
                "universe_id": options.universe_id,
                "rebalance_frequency": "daily",
                "horizon_days": options.horizon_days,
                "direction": spec.direction,
                "neutralization": "none",
                "rank_method": "cross_section_percent_rank",
                "weighting_method": "demeaned_unit_gross",
                "is_point_in_time_safe": True,
                "available_at_policy": "Signal available when all same-date feature inputs are available.",
                "params_json": json_dumps(
                    {
                        "top_quantile": options.top_quantile,
                        "bottom_quantile": options.bottom_quantile,
                        "min_cross_section": options.min_cross_section,
                    }
                ),
                "owner": "atx-impl",
                "source": SOURCE_NAME,
            }
        )
    return pd.DataFrame(rows)


class AlphaResearchDataset(Dataset):
    dataset_id = "alpha_research"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: AlphaResearchOptions) -> DatasetLoadResult:
        if options.horizon_days < 1:
            raise ValueError("horizon_days must be positive")
        if not 0 < options.top_quantile < 1:
            raise ValueError("top_quantile must be between 0 and 1")
        if not 0 < options.bottom_quantile < 1:
            raise ValueError("bottom_quantile must be between 0 and 1")
        if options.top_quantile + options.bottom_quantile > 1:
            raise ValueError("top_quantile + bottom_quantile must be <= 1")
        if options.min_cross_section < 2:
            raise ValueError("min_cross_section must be at least 2")

        symbols_join, symbols_frame = _target_symbols_sql(options.symbols)
        if symbols_frame is not None:
            store.con.register("alpha_symbol_filter", symbols_frame)
        try:
            signals = self._build_signals(store, options, symbols_join)
        finally:
            if symbols_frame is not None:
                store.con.unregister("alpha_symbol_filter")

        catalog = _alpha_catalog_frame(options)
        manifests = self._build_manifests(store, options, signals)
        loaded_signals = self._replace_rows(store, catalog, signals, manifests)

        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="alpha_signal_values",
            check_name="rows_loaded",
            status="passed" if loaded_signals > 0 else "warning",
            observed_value=float(loaded_signals),
            threshold_value=1.0,
            details={
                "feature_set": options.feature_set,
                "universe_id": options.universe_id,
                "alpha_count": len(catalog),
                "manifest_count": len(manifests),
                "symbols": options.symbols,
                "horizon_days": options.horizon_days,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=loaded_signals + len(catalog) + len(manifests),
            source=SOURCE_NAME,
            details={
                "feature_set": options.feature_set,
                "universe_id": options.universe_id,
                "alpha_count": len(catalog),
                "signals": loaded_signals,
                "manifests": len(manifests),
                "start_date": None if signals.empty else signals["as_of_date"].min(),
                "end_date": None if signals.empty else signals["as_of_date"].max(),
            },
        )

    def _build_signals(
        self,
        store: DuckDBStore,
        options: AlphaResearchOptions,
        symbols_join: str,
    ) -> pd.DataFrame:
        date_filters: list[str] = []
        params: list[Any] = [options.feature_set]
        if options.start_date is not None:
            date_filters.append("fv.as_of_date >= ?")
            params.append(options.start_date)
        if options.end_date is not None:
            date_filters.append("fv.as_of_date <= ?")
            params.append(options.end_date)
        date_sql = "" if not date_filters else "AND " + " AND ".join(date_filters)
        alpha_selects = "\nUNION ALL\n".join(
            f"""
            SELECT
                '{spec.alpha_id}' AS alpha_id,
                security_id,
                symbol,
                as_of_date,
                available_at,
                {spec.signal_sql} AS raw_signal
            FROM scored
            WHERE { " AND ".join(f"{feature} IS NOT NULL" for feature in spec.input_features) }
            """
            for spec in DEFAULT_ALPHA_SPECS
        )
        signals = store.con.execute(
            f"""
            WITH feature_panel AS (
                SELECT
                    fv.security_id,
                    fv.symbol,
                    fv.as_of_date,
                    max(fv.available_at) AS available_at,
                    max(CASE WHEN fv.feature_name = 'ret_1d' THEN fv.value END) AS ret_1d,
                    max(CASE WHEN fv.feature_name = 'mom_5d' THEN fv.value END) AS mom_5d,
                    max(CASE WHEN fv.feature_name = 'mom_21d' THEN fv.value END) AS mom_21d,
                    max(CASE WHEN fv.feature_name = 'mom_63d' THEN fv.value END) AS mom_63d,
                    max(CASE WHEN fv.feature_name = 'vol_21d' THEN fv.value END) AS vol_21d,
                    max(CASE WHEN fv.feature_name = 'adv_21d' THEN fv.value END) AS adv_21d
                FROM feature_values fv
                {symbols_join}
                WHERE fv.feature_set = ?
                  AND fv.feature_name IN ('ret_1d', 'mom_5d', 'mom_21d', 'mom_63d', 'vol_21d', 'adv_21d')
                  AND fv.value IS NOT NULL
                  {date_sql}
                GROUP BY fv.security_id, fv.symbol, fv.as_of_date
            ),
            scored AS (
                SELECT
                    *,
                    (ret_1d - avg(ret_1d) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(ret_1d) OVER (PARTITION BY as_of_date), 0) AS ret_1d_z,
                    (mom_5d - avg(mom_5d) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(mom_5d) OVER (PARTITION BY as_of_date), 0) AS mom_5d_z,
                    (mom_21d - avg(mom_21d) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(mom_21d) OVER (PARTITION BY as_of_date), 0) AS mom_21d_z,
                    (mom_63d - avg(mom_63d) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(mom_63d) OVER (PARTITION BY as_of_date), 0) AS mom_63d_z,
                    (vol_21d - avg(vol_21d) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(vol_21d) OVER (PARTITION BY as_of_date), 0) AS vol_21d_z,
                    (ln(1 + adv_21d) - avg(ln(1 + adv_21d)) OVER (PARTITION BY as_of_date)) /
                        nullif(stddev_samp(ln(1 + adv_21d)) OVER (PARTITION BY as_of_date), 0) AS adv_21d_log_z
                FROM feature_panel
            ),
            alpha_raw AS (
                {alpha_selects}
            ),
            ranked AS (
                SELECT
                    *,
                    count(*) OVER (PARTITION BY alpha_id, as_of_date) AS cross_section_count,
                    percent_rank() OVER (PARTITION BY alpha_id, as_of_date ORDER BY raw_signal) AS rank_value
                FROM alpha_raw
                WHERE raw_signal IS NOT NULL
            ),
            weighted AS (
                SELECT
                    *,
                    CASE
                        WHEN cross_section_count < ?
                            THEN NULL
                        WHEN rank_value >= 1 - ?
                            THEN 1.0
                        WHEN rank_value <= ?
                            THEN -1.0
                        ELSE 0.0
                    END AS side
                FROM ranked
            ),
            normalized AS (
                SELECT
                    *,
                    CASE
                        WHEN side IS NULL THEN NULL
                        ELSE side / nullif(sum(abs(side)) OVER (PARTITION BY alpha_id, as_of_date), 0)
                    END AS weight
                FROM weighted
            )
            SELECT
                alpha_id,
                security_id,
                symbol,
                as_of_date,
                raw_signal AS signal_value,
                rank_value,
                weight,
                cross_section_count,
                available_at,
                '{SOURCE_NAME}' AS source,
                ? AS run_id
            FROM normalized
            WHERE weight IS NOT NULL
            ORDER BY alpha_id, as_of_date, security_id
            """,
            params + [options.min_cross_section, options.top_quantile, options.bottom_quantile, options.run_id],
        ).df()
        if signals.empty:
            return signals
        signals["alpha_signal_id"] = [
            _hash_id("alpha_signal", row.alpha_id, row.security_id, row.as_of_date)
            for row in signals.itertuples(index=False)
        ]
        signals["input_hash"] = [
            _hash_id("alpha_signal_input", row.alpha_id, row.security_id, row.as_of_date, row.signal_value, row.weight)
            for row in signals.itertuples(index=False)
        ]
        return signals[
            [
                "alpha_signal_id",
                "alpha_id",
                "security_id",
                "symbol",
                "as_of_date",
                "signal_value",
                "rank_value",
                "weight",
                "cross_section_count",
                "available_at",
                "input_hash",
                "source",
                "run_id",
            ]
        ]

    def _build_manifests(
        self,
        store: DuckDBStore,
        options: AlphaResearchOptions,
        signals: pd.DataFrame,
    ) -> pd.DataFrame:
        if signals.empty:
            return pd.DataFrame()
        store.con.register("alpha_signals_eval", signals)
        try:
            frame = store.con.execute(
                """
                WITH returns AS (
                    SELECT
                        security_id,
                        trade_date AS as_of_date,
                        lead(close, ?) OVER (PARTITION BY security_id ORDER BY trade_date) / close - 1.0 AS forward_return
                    FROM equity_daily_bars
                ),
                eval AS (
                    SELECT
                        s.alpha_id,
                        s.as_of_date,
                        s.security_id,
                        s.weight,
                        s.signal_value,
                        s.cross_section_count,
                        r.forward_return,
                        s.weight * r.forward_return AS contribution
                    FROM alpha_signals_eval s
                    JOIN returns r
                      ON r.security_id = s.security_id
                     AND r.as_of_date = s.as_of_date
                    WHERE r.forward_return IS NOT NULL
                ),
                daily AS (
                    SELECT
                        alpha_id,
                        as_of_date,
                        sum(contribution) AS long_short_return,
                        corr(signal_value, forward_return) AS rank_ic,
                        count(*) AS securities
                    FROM eval
                    GROUP BY 1, 2
                )
                SELECT
                    alpha_id,
                    min(as_of_date) AS start_date,
                    max(as_of_date) AS end_date,
                    count(*) AS evaluation_days,
                    sum(securities) AS evaluated_signal_count,
                    avg(securities) AS average_cross_section,
                    avg(long_short_return) AS mean_daily_long_short_return,
                    stddev_samp(long_short_return) AS volatility_daily_long_short_return,
                    avg(rank_ic) AS mean_rank_ic,
                    avg(CASE WHEN long_short_return > 0 THEN 1.0 ELSE 0.0 END) AS hit_rate,
                    sum(long_short_return) AS cumulative_long_short_return
                FROM daily
                GROUP BY 1
                ORDER BY 1
                """,
                [options.horizon_days],
            ).df()
        finally:
            store.con.unregister("alpha_signals_eval")
        if frame.empty:
            return frame
        signal_counts = signals.groupby("alpha_id").agg(
            signal_count=("alpha_signal_id", "count"),
            security_count=("security_id", "nunique"),
            min_available_at=("available_at", "min"),
            max_available_at=("available_at", "max"),
        )
        frame = frame.merge(signal_counts, left_on="alpha_id", right_index=True, how="left")
        frame["backtest_id"] = [
            _hash_id(
                "alpha_backtest",
                row.alpha_id,
                options.feature_set,
                options.universe_id,
                row.start_date,
                row.end_date,
                options.horizon_days,
            )
            for row in frame.itertuples(index=False)
        ]
        frame["feature_set"] = options.feature_set
        frame["universe_id"] = options.universe_id
        frame["horizon_days"] = options.horizon_days
        frame["rebalance_frequency"] = "daily"
        frame["params_json"] = json_dumps(
            {
                "symbols": options.symbols,
                "start_date": options.start_date,
                "end_date": options.end_date,
                "top_quantile": options.top_quantile,
                "bottom_quantile": options.bottom_quantile,
                "min_cross_section": options.min_cross_section,
            }
        )
        frame["source"] = SOURCE_NAME
        frame["run_id"] = options.run_id
        return frame[
            [
                "backtest_id",
                "alpha_id",
                "feature_set",
                "universe_id",
                "start_date",
                "end_date",
                "horizon_days",
                "rebalance_frequency",
                "signal_count",
                "security_count",
                "evaluation_days",
                "evaluated_signal_count",
                "average_cross_section",
                "mean_daily_long_short_return",
                "volatility_daily_long_short_return",
                "mean_rank_ic",
                "hit_rate",
                "cumulative_long_short_return",
                "min_available_at",
                "max_available_at",
                "params_json",
                "source",
                "run_id",
            ]
        ]

    def _replace_rows(
        self,
        store: DuckDBStore,
        catalog: pd.DataFrame,
        signals: pd.DataFrame,
        manifests: pd.DataFrame,
    ) -> int:
        with store.transaction():
            if not catalog.empty:
                store.con.register("alpha_expression_catalog_load", catalog)
                try:
                    store.con.execute(
                        """
                        DELETE FROM alpha_expression_catalog AS dst
                        USING alpha_expression_catalog_load AS src
                        WHERE dst.alpha_id = src.alpha_id
                        """
                    )
                    insert_frame(store, catalog, "alpha_expression_catalog", "alpha_expression_catalog_insert")
                finally:
                    store.con.unregister("alpha_expression_catalog_load")
            if not signals.empty:
                store.con.register("alpha_signal_values_load", signals)
                try:
                    store.con.execute(
                        """
                        DELETE FROM alpha_signal_values AS dst
                        USING alpha_signal_values_load AS src
                        WHERE dst.alpha_id = src.alpha_id
                          AND dst.security_id = src.security_id
                          AND dst.as_of_date = src.as_of_date
                        """
                    )
                    insert_frame(store, signals, "alpha_signal_values", "alpha_signal_values_insert")
                finally:
                    store.con.unregister("alpha_signal_values_load")
            if not manifests.empty:
                store.con.register("alpha_backtest_manifests_load", manifests)
                try:
                    store.con.execute(
                        """
                        DELETE FROM alpha_backtest_manifests AS dst
                        USING alpha_backtest_manifests_load AS src
                        WHERE dst.backtest_id = src.backtest_id
                        """
                    )
                    insert_frame(store, manifests, "alpha_backtest_manifests", "alpha_backtest_manifests_insert")
                finally:
                    store.con.unregister("alpha_backtest_manifests_load")
        return int(len(signals))
