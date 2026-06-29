from __future__ import annotations

from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "SEC XBRL share counts"
SHARE_COUNT_METRICS = ("shares_outstanding", "shares_basic_avg", "shares_diluted_avg")


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
            """
            INSERT INTO shares_outstanding_history (
                share_history_id,
                source,
                security_id,
                symbol,
                cik,
                share_count_type,
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
                p.canonical_metric AS share_count_type,
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
            WHERE p.canonical_metric IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
              AND p.value IS NOT NULL
              AND p.value >= 0
              AND p.security_id IS NOT NULL
              AND p.security_id <> ''
              AND p.cik IS NOT NULL
              AND p.cik <> ''
              AND p.period_end IS NOT NULL
              AND p.as_of_date IS NOT NULL
              AND p.accession_number IS NOT NULL
              AND p.accession_number <> ''
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
