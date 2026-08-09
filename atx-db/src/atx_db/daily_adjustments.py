from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .adjustment_factors import SOURCE_NAME as ADJUSTMENT_FACTOR_SOURCE
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "public daily adjustment factors"


@dataclass(frozen=True)
class DailyAdjustmentFactorOptions:
    source: str = SOURCE_NAME
    factor_source: str = ADJUSTMENT_FACTOR_SOURCE
    bar_source: str | None = None
    as_of_date: dt.date | None = None
    as_of_ts: dt.datetime | None = None
    run_id: str | None = None


def _max_bar_trade_date(store: DuckDBStore) -> dt.date | None:
    row = store.con.execute("SELECT max(trade_date) FROM equity_daily_bars").fetchone()
    if not row:
        return None
    return row[0]


def _end_of_day(as_of_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(as_of_date, dt.time(22, 0))


def refresh_daily_adjustment_factors(
    store: DuckDBStore,
    options: DailyAdjustmentFactorOptions | None = None,
) -> int:
    """Materialize PIT daily split and total-return adjustment factors."""

    options = options or DailyAdjustmentFactorOptions()
    as_of_date = options.as_of_date or _max_bar_trade_date(store)
    if as_of_date is None:
        return 0
    as_of_ts = options.as_of_ts or _end_of_day(as_of_date)

    with store.transaction():
        store.con.execute(
            """
            DELETE FROM daily_adjustment_factors
            WHERE source = ?
              AND factor_source = ?
              AND as_of_date = ?
              AND (? IS NULL OR bar_source = ?)
            """,
            [options.source, options.factor_source, as_of_date, options.bar_source, options.bar_source],
        )
        store.con.execute(
            """
            INSERT INTO daily_adjustment_factors (
                daily_adjustment_id,
                source,
                bar_source,
                factor_source,
                security_id,
                symbol,
                trade_date,
                as_of_date,
                split_price_factor,
                split_share_factor,
                dividend_total_return_factor,
                total_return_price_factor,
                raw_close,
                split_adjusted_close,
                total_return_adjusted_close,
                raw_volume,
                split_adjusted_volume,
                visible_event_count,
                split_event_count,
                cash_div_event_count,
                last_factor_ex_date,
                available_at,
                run_id
            )
            WITH params AS (
                SELECT
                    ? AS source,
                    ? AS factor_source,
                    ? AS bar_source_filter,
                    CAST(? AS DATE) AS as_of_date,
                    CAST(? AS TIMESTAMP) AS as_of_ts,
                    ? AS run_id
            ),
            bars AS (
                SELECT
                    b.source AS bar_source,
                    b.security_id,
                    b.symbol,
                    b.trade_date,
                    b.close AS raw_close,
                    b.volume AS raw_volume
                FROM equity_daily_bars b
                CROSS JOIN params p
                WHERE b.trade_date <= p.as_of_date
                  AND b.close IS NOT NULL
                  AND b.close > 0
                  AND (b.available_at IS NULL OR b.available_at <= p.as_of_ts)
                  AND (p.bar_source_filter IS NULL OR b.source = p.bar_source_filter)
            ),
            events AS (
                SELECT a.*
                FROM adjustment_factor_history a
                CROSS JOIN params p
                WHERE a.source = p.factor_source
                  AND a.ex_date <= p.as_of_date
                  AND (a.available_at IS NULL OR a.available_at <= p.as_of_ts)
                  AND a.factor_price > 0
                  AND a.factor_shares > 0
            ),
            aggregated AS (
                SELECT
                    p.source,
                    b.bar_source,
                    p.factor_source,
                    b.security_id,
                    b.symbol,
                    b.trade_date,
                    p.as_of_date,
                    exp(
                        coalesce(
                            sum(CASE WHEN e.event_type = 'SPLIT' THEN ln(e.factor_price) ELSE 0.0 END),
                            0.0
                        )
                    ) AS split_price_factor,
                    exp(
                        coalesce(
                            sum(CASE WHEN e.event_type = 'SPLIT' THEN ln(e.factor_shares) ELSE 0.0 END),
                            0.0
                        )
                    ) AS split_share_factor,
                    exp(
                        coalesce(
                            sum(CASE WHEN e.event_type = 'CASH_DIV' THEN ln(e.factor_price) ELSE 0.0 END),
                            0.0
                        )
                    ) AS dividend_total_return_factor,
                    b.raw_close,
                    b.raw_volume,
                    count(e.event_ref_id)::INTEGER AS visible_event_count,
                    sum(CASE WHEN e.event_type = 'SPLIT' THEN 1 ELSE 0 END)::INTEGER AS split_event_count,
                    sum(CASE WHEN e.event_type = 'CASH_DIV' THEN 1 ELSE 0 END)::INTEGER AS cash_div_event_count,
                    max(CASE WHEN e.event_type IN ('SPLIT', 'CASH_DIV') THEN e.ex_date ELSE NULL END) AS last_factor_ex_date,
                    p.as_of_ts AS available_at,
                    p.run_id
                FROM bars b
                CROSS JOIN params p
                LEFT JOIN events e
                  ON e.security_id = b.security_id
                 AND e.ex_date > b.trade_date
                GROUP BY
                    p.source,
                    b.bar_source,
                    p.factor_source,
                    b.security_id,
                    b.symbol,
                    b.trade_date,
                    p.as_of_date,
                    b.raw_close,
                    b.raw_volume,
                    p.as_of_ts,
                    p.run_id
            )
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        source,
                        bar_source,
                        factor_source,
                        security_id,
                        CAST(trade_date AS VARCHAR),
                        CAST(as_of_date AS VARCHAR)
                    )
                ) AS daily_adjustment_id,
                source,
                bar_source,
                factor_source,
                security_id,
                symbol,
                trade_date,
                as_of_date,
                split_price_factor,
                split_share_factor,
                dividend_total_return_factor,
                split_price_factor * dividend_total_return_factor AS total_return_price_factor,
                raw_close,
                raw_close * split_price_factor AS split_adjusted_close,
                raw_close * split_price_factor * dividend_total_return_factor AS total_return_adjusted_close,
                raw_volume,
                CASE
                    WHEN raw_volume IS NULL THEN NULL
                    ELSE CAST(raw_volume AS DOUBLE) * split_share_factor
                END AS split_adjusted_volume,
                visible_event_count,
                split_event_count,
                cash_div_event_count,
                last_factor_ex_date,
                available_at,
                coalesce(run_id, ?)
            FROM aggregated
            """,
            [
                options.source,
                options.factor_source,
                options.bar_source,
                as_of_date,
                as_of_ts,
                options.run_id,
                options.run_id,
            ],
        )

    return int(
        store.con.execute(
            """
            SELECT count(*)
            FROM daily_adjustment_factors
            WHERE source = ?
              AND factor_source = ?
              AND as_of_date = ?
              AND (? IS NULL OR bar_source = ?)
            """,
            [options.source, options.factor_source, as_of_date, options.bar_source, options.bar_source],
        ).fetchone()[0]
    )


class DailyAdjustmentFactorDataset(Dataset):
    dataset_id = "daily_adjustment_factors"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: DailyAdjustmentFactorOptions,
    ) -> DatasetLoadResult:
        rows = refresh_daily_adjustment_factors(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="daily_adjustment_factors",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "factor_source": options.factor_source,
                "bar_source": options.bar_source,
                "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "factor_source": options.factor_source,
                "bar_source": options.bar_source,
                "as_of_date": options.as_of_date.isoformat() if options.as_of_date else None,
            },
        )
