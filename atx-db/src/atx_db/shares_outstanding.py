from __future__ import annotations

from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "SEC XBRL share counts"
SHARE_COUNT_METRICS = (
    "shares_outstanding",
    "shares_basic_avg",
    "shares_diluted_avg",
    "float",
    "treasury",
    "class_a",
    "class_b",
    "class_c",
    "class_d",
)
_METRIC_SQL_LIST = ", ".join(f"'{metric}'" for metric in SHARE_COUNT_METRICS)


@dataclass(frozen=True)
class SharesOutstandingHistoryOptions:
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_shares_outstanding_history(
    store: DuckDBStore,
    options: SharesOutstandingHistoryOptions | None = None,
) -> int:
    """Materialize PIT share-count history from normalized statement points."""

    options = options or SharesOutstandingHistoryOptions()
    with store.transaction():
        store.con.execute("DELETE FROM shares_outstanding_history WHERE source = ?", [options.source])
        store.con.execute(
            f"""
            INSERT INTO shares_outstanding_history (
                share_history_id,
                source,
                security_id,
                symbol,
                cik,
                share_count_type,
                share_class,
                share_count_category,
                taxonomy,
                concept,
                unit,
                period_type,
                period_start,
                period_end,
                effective_date,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                revision_sequence,
                revision_count,
                is_latest_revision,
                share_count,
                source_url,
                run_id,
                source_loaded_at
            )
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        ?,
                        p.security_id,
                        p.canonical_metric,
                        p.unit,
                        coalesce(CAST(p.period_start AS VARCHAR), ''),
                        CAST(p.period_end AS VARCHAR),
                        p.accession_number,
                        p.statement_point_id
                    )
                ) AS share_history_id,
                ? AS source,
                p.security_id,
                p.symbol,
                p.cik,
                m.share_count_type,
                m.share_class,
                CASE
                    WHEN m.share_count_type IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
                        THEN 'consolidated'
                    WHEN m.share_count_type IN ('float', 'treasury')
                        THEN 'float_treasury'
                    ELSE 'share_class'
                END AS share_count_category,
                p.taxonomy,
                p.concept,
                p.unit,
                p.period_type,
                p.period_start,
                p.period_end,
                p.period_end AS effective_date,
                p.as_of_date,
                p.available_at,
                p.fiscal_year,
                p.fiscal_period,
                p.form,
                p.accession_number,
                p.revision_sequence,
                p.revision_count,
                p.is_latest_revision,
                p.value AS share_count,
                p.source_url,
                coalesce(?, p.run_id) AS run_id,
                p.source_loaded_at
            FROM fundamental_statement_points p
            JOIN (
                SELECT
                    statement_point_id,
                    CASE
                        WHEN lower(canonical_metric) IN ('shares_float', 'public_float') THEN 'float'
                        WHEN lower(canonical_metric) IN ('shares_treasury', 'treasury_shares') THEN 'treasury'
                        ELSE lower(canonical_metric)
                    END AS share_count_type,
                    CASE
                        WHEN lower(canonical_metric) LIKE 'class_%'
                            THEN upper(substr(lower(canonical_metric), 7))
                        ELSE NULL
                    END AS share_class
                FROM fundamental_statement_points
            ) m
              ON m.statement_point_id = p.statement_point_id
            LEFT JOIN (
                SELECT
                    security_id,
                    accession_number,
                    period_end,
                    as_of_date,
                    max(value) AS shares_outstanding
                FROM fundamental_statement_points
                WHERE lower(canonical_metric) = 'shares_outstanding'
                  AND value IS NOT NULL
                  AND value >= 0
                GROUP BY security_id, accession_number, period_end, as_of_date
            ) outstanding
              ON outstanding.security_id = p.security_id
             AND outstanding.accession_number = p.accession_number
             AND outstanding.period_end = p.period_end
             AND outstanding.as_of_date = p.as_of_date
            WHERE m.share_count_type IN ({_METRIC_SQL_LIST})
              AND p.value IS NOT NULL
              AND p.value >= 0
              AND (lower(p.unit) IN ('share', 'shares') OR lower(p.unit_type) IN ('share', 'shares'))
              AND p.security_id IS NOT NULL
              AND p.security_id <> ''
              AND p.cik IS NOT NULL
              AND p.cik <> ''
              AND p.period_end IS NOT NULL
              AND p.as_of_date IS NOT NULL
              AND p.accession_number IS NOT NULL
              AND p.accession_number <> ''
              AND (
                  m.share_count_type <> 'float'
                  OR outstanding.shares_outstanding IS NULL
                  OR p.value <= outstanding.shares_outstanding
              )
            """,
            [options.source, options.source, options.run_id],
        )

    return int(
        store.con.execute(
            "SELECT count(*) FROM shares_outstanding_history WHERE source = ?",
            [options.source],
        ).fetchone()[0]
    )


class SharesOutstandingHistoryDataset(Dataset):
    dataset_id = "shares_outstanding_history"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: SharesOutstandingHistoryOptions,
    ) -> DatasetLoadResult:
        rows = refresh_shares_outstanding_history(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="shares_outstanding_history",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"metrics": SHARE_COUNT_METRICS},
        )
