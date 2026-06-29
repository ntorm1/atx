from __future__ import annotations

from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


TYPE_DIM_SOURCE = "ATX corporate action type seed"
SOURCE_NAME = "public corporate action adjustment factors"


@dataclass(frozen=True)
class CorporateActionTypeRow:
    type_code: int
    event_type: str
    category: str
    sub_category: str
    description: str
    crsp_distcd: int | None
    dtcc_caev: str | None
    bloomberg_type: str | None
    factset_type: str | None
    affects_price: bool
    affects_shares: bool
    taxable: bool | None
    mandatory: bool


CORP_ACTION_TYPE_ROWS: tuple[CorporateActionTypeRow, ...] = (
    CorporateActionTypeRow(
        120000,
        "CASH_DIV",
        "D",
        "cash_dividend",
        "Cash dividend or distribution; public/local inference maps to CRSP cash-distribution buckets.",
        1232,
        "DVCA",
        "Cash Dividend",
        "cash_dividend",
        True,
        False,
        True,
        True,
    ),
    CorporateActionTypeRow(
        500000,
        "SPLIT",
        "S",
        "forward_or_reverse_split",
        "Stock split or reverse split; price and share factors move inversely.",
        5523,
        "SPLF",
        "Stock Split",
        "split",
        True,
        True,
        None,
        True,
    ),
    CorporateActionTypeRow(
        700000,
        "SPINOFF",
        "P",
        "spinoff",
        "Spinoff or distribution of another security; detailed basis allocation requires Form 8937 evidence.",
        3763,
        "SOFF",
        "Spin Off",
        "spinoff",
        True,
        True,
        None,
        True,
    ),
    CorporateActionTypeRow(
        800000,
        "MERGER",
        "M",
        "merger_or_exchange",
        "Merger, acquisition, exchange, or reorganization event.",
        5737,
        "MRGR",
        "Merger",
        "merger",
        True,
        True,
        None,
        True,
    ),
    CorporateActionTypeRow(
        900000,
        "OTHER",
        "X",
        "other",
        "Other corporate-action event not yet mapped to a richer public type.",
        None,
        None,
        None,
        "other",
        True,
        False,
        None,
        True,
    ),
)


@dataclass(frozen=True)
class AdjustmentFactorHistoryOptions:
    source: str = SOURCE_NAME
    run_id: str | None = None


def seed_corp_action_type_dim(store: DuckDBStore) -> int:
    """Seed the compact CRSP/DTCC-style corporate-action type dimension."""

    store.con.execute("DELETE FROM corp_action_type_dim WHERE source = ?", [TYPE_DIM_SOURCE])
    for row in CORP_ACTION_TYPE_ROWS:
        store.con.execute(
            """
            INSERT OR REPLACE INTO corp_action_type_dim (
                type_code,
                event_type,
                category,
                sub_category,
                description,
                crsp_distcd,
                dtcc_caev,
                bloomberg_type,
                factset_type,
                affects_price,
                affects_shares,
                taxable,
                mandatory,
                source,
                source_loaded_at,
                updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now(), now())
            """,
            [
                row.type_code,
                row.event_type,
                row.category,
                row.sub_category,
                row.description,
                row.crsp_distcd,
                row.dtcc_caev,
                row.bloomberg_type,
                row.factset_type,
                row.affects_price,
                row.affects_shares,
                row.taxable,
                row.mandatory,
                TYPE_DIM_SOURCE,
            ],
        )
    return len(CORP_ACTION_TYPE_ROWS)


def refresh_adjustment_factor_history(
    store: DuckDBStore,
    options: AdjustmentFactorHistoryOptions | None = None,
) -> int:
    """Build event-level price/share/volume factors from normalized corporate actions."""

    options = options or AdjustmentFactorHistoryOptions()
    with store.transaction():
        seed_corp_action_type_dim(store)
        store.con.execute("DELETE FROM adjustment_factor_history WHERE source = ?", [options.source])
        store.con.execute(
            """
            INSERT INTO adjustment_factor_history (
                adjustment_factor_id,
                source,
                source_action_source,
                security_id,
                symbol,
                ex_date,
                event_type,
                type_code,
                event_ref_id,
                factor_price,
                factor_shares,
                factor_volume,
                ratio_numerator,
                ratio_denominator,
                cash_div_amount,
                cash_div_currency,
                cumulative_price_factor,
                cumulative_share_factor,
                available_at,
                run_id,
                source_loaded_at
            )
            WITH classified AS (
                SELECT
                    c.*,
                    sha256(
                        concat_ws(
                            '|',
                            c.source,
                            c.security_id,
                            coalesce(c.symbol, ''),
                            c.action_type,
                            CAST(c.ex_date AS VARCHAR),
                            coalesce(CAST(c.cash_amount AS VARCHAR), ''),
                            coalesce(CAST(c.split_from AS VARCHAR), ''),
                            coalesce(CAST(c.split_to AS VARCHAR), ''),
                            coalesce(CAST(c.adjustment_factor AS VARCHAR), '')
                        )
                    ) AS event_ref_id,
                    CASE
                        WHEN c.action_type ILIKE '%spin%' THEN 'SPINOFF'
                        WHEN c.action_type ILIKE '%merger%' OR c.action_type ILIKE '%exchange%' THEN 'MERGER'
                        WHEN (c.split_from IS NOT NULL AND c.split_to IS NOT NULL AND c.split_from > 0 AND c.split_to > 0)
                          OR c.action_type ILIKE '%split%' THEN 'SPLIT'
                        WHEN (c.cash_amount IS NOT NULL AND c.cash_amount <> 0)
                          OR c.action_type ILIKE '%dividend%' THEN 'CASH_DIV'
                        ELSE 'OTHER'
                    END AS event_type
                FROM corporate_actions c
                WHERE c.security_id IS NOT NULL
                  AND c.security_id <> ''
                  AND c.ex_date IS NOT NULL
            ),
            factors AS (
                SELECT
                    c.*,
                    CASE
                        WHEN c.event_type = 'SPLIT'
                         AND c.split_from IS NOT NULL
                         AND c.split_to IS NOT NULL
                         AND c.split_from > 0
                         AND c.split_to > 0
                            THEN c.split_from / c.split_to
                        WHEN c.adjustment_factor IS NOT NULL AND c.adjustment_factor > 0
                            THEN c.adjustment_factor
                        ELSE 1.0
                    END AS factor_price,
                    CASE
                        WHEN c.event_type = 'SPLIT'
                         AND c.split_from IS NOT NULL
                         AND c.split_to IS NOT NULL
                         AND c.split_from > 0
                         AND c.split_to > 0
                            THEN c.split_to / c.split_from
                        ELSE 1.0
                    END AS factor_shares
                FROM classified c
            ),
            typed AS (
                SELECT
                    f.*,
                    d.type_code
                FROM factors f
                JOIN corp_action_type_dim d
                  ON d.event_type = f.event_type
                WHERE f.factor_price > 0
                  AND f.factor_shares > 0
            ),
            sequenced AS (
                SELECT
                    t.*,
                    exp(
                        sum(ln(t.factor_price)) OVER (
                            PARTITION BY t.security_id
                            ORDER BY t.ex_date ASC, t.event_ref_id ASC
                            ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                        )
                    ) AS cumulative_price_factor,
                    exp(
                        sum(ln(t.factor_shares)) OVER (
                            PARTITION BY t.security_id
                            ORDER BY t.ex_date ASC, t.event_ref_id ASC
                            ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                        )
                    ) AS cumulative_share_factor
                FROM typed t
            )
            SELECT
                sha256(concat_ws('|', ?, s.event_ref_id, s.event_type)) AS adjustment_factor_id,
                ? AS source,
                s.source AS source_action_source,
                s.security_id,
                s.symbol,
                s.ex_date,
                s.event_type,
                s.type_code,
                s.event_ref_id,
                s.factor_price,
                s.factor_shares,
                s.factor_shares AS factor_volume,
                s.split_to AS ratio_numerator,
                s.split_from AS ratio_denominator,
                CASE WHEN s.event_type = 'CASH_DIV' THEN s.cash_amount ELSE NULL END AS cash_div_amount,
                CASE WHEN s.event_type = 'CASH_DIV' THEN 'USD' ELSE NULL END AS cash_div_currency,
                s.cumulative_price_factor,
                s.cumulative_share_factor,
                coalesce(s.available_at, CAST(s.ex_date AS TIMESTAMP) + INTERVAL 22 HOURS) AS available_at,
                coalesce(?, s.run_id) AS run_id,
                s.source_loaded_at
            FROM sequenced s
            """,
            [options.source, options.source, options.run_id],
        )

    return int(
        store.con.execute(
            "SELECT count(*) FROM adjustment_factor_history WHERE source = ?",
            [options.source],
        ).fetchone()[0]
    )


class AdjustmentFactorHistoryDataset(Dataset):
    dataset_id = "adjustment_factor_history"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: AdjustmentFactorHistoryOptions,
    ) -> DatasetLoadResult:
        rows = refresh_adjustment_factor_history(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="adjustment_factor_history",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "type_rows": len(CORP_ACTION_TYPE_ROWS)},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"type_rows": len(CORP_ACTION_TYPE_ROWS)},
        )
