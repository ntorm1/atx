from __future__ import annotations

from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "ATX public delisting proxy builder"
DEFAULT_SOURCE = "atx_delisting_proxy_v1"
DEFAULT_CODE_SOURCE = "atx_delist_code_dim_v1"


@dataclass(frozen=True)
class DelistingEventOptions:
    source: str = DEFAULT_SOURCE
    listing_status_source: str | None = None
    include_snapshot_absence: bool = False
    apply_shumway_warther_imputation: bool = False
    run_id: str | None = None


DELIST_CODE_ROWS = (
    (
        "NASDAQ_DELETE",
        "ATX_PUBLIC_PROXY",
        None,
        None,
        "UNKNOWN_PUBLIC_DELETE",
        "exchange_delete",
        (
            "Nasdaq Trader add/delete file delete action. This is public listing-status evidence, "
            "not an official CRSP DLSTCD reason code."
        ),
        "DELISTED_OR_TRANSFERRED_UNKNOWN",
        True,
        -0.30,
        "optional_shumway_warther_unresolved_delete_minus_30pct",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "SNAPSHOT_ABSENCE",
        "ATX_PUBLIC_PROXY",
        None,
        None,
        "UNKNOWN_SNAPSHOT_GAP",
        "snapshot_absence",
        (
            "Symbol disappeared from consecutive public symbol-directory snapshots. This is lower "
            "confidence absence evidence and should not be treated as an official delisting reason."
        ),
        "ABSENT_FROM_PUBLIC_DIRECTORY",
        False,
        None,
        "none",
        DEFAULT_CODE_SOURCE,
    ),
)


def seed_delist_code_dim(store: DuckDBStore, *, source: str = DEFAULT_CODE_SOURCE) -> int:
    store.initialize()
    rows = [row for row in DELIST_CODE_ROWS if row[-1] == source]
    if not rows:
        return 0
    with store.transaction():
        store.con.execute("DELETE FROM delist_code_dim WHERE source = ?", [source])
        store.con.executemany(
            """
            INSERT INTO delist_code_dim (
                delist_code,
                code_system,
                vendor_code,
                crsp_dlstcd,
                crsp_dlstcd_family,
                reason_category,
                description,
                terminal_trading_status,
                imputation_allowed,
                default_imputed_return,
                imputation_policy,
                source
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            rows,
        )
    return len(rows)


def refresh_delisting_events(
    store: DuckDBStore,
    options: DelistingEventOptions | None = None,
) -> int:
    """Materialize conservative public delisting evidence from listing-status intervals."""

    options = options or DelistingEventOptions()
    store.initialize()
    seed_delist_code_dim(store)

    with store.transaction():
        store.con.execute(
            """
            DELETE FROM delisting_events
            WHERE source = ?
              AND (? IS NULL OR listing_status_source = ?)
            """,
            [options.source, options.listing_status_source, options.listing_status_source],
        )
        store.con.execute(
            """
            INSERT INTO delisting_events (
                delisting_event_id,
                source,
                listing_status_source,
                source_listing_status_id,
                security_id,
                symbol,
                listing_venue_code,
                listing_venue_name,
                listing_exchange_code,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                delist_reason,
                delisting_return,
                delisting_return_type,
                is_return_imputed,
                return_policy,
                return_confidence,
                evidence_source,
                evidence_source_table,
                source_event_id,
                source_url,
                method,
                evidence_confidence,
                inferred_from_absence,
                details_json,
                run_id
            )
            WITH params AS (
                SELECT
                    ? AS source,
                    ? AS listing_status_source,
                    CAST(? AS BOOLEAN) AS include_snapshot_absence,
                    CAST(? AS BOOLEAN) AS apply_imputation,
                    ? AS run_id
            ),
            candidates AS (
                SELECT
                    l.*,
                    'NASDAQ_DELETE' AS delist_code,
                    l.valid_from AS delist_date,
                    coalesce(l.as_of_date, l.valid_from) AS event_as_of_date,
                    coalesce(l.available_at, l.last_evidence_at) AS event_available_at,
                    'trading_system_delete_action' AS delisting_method,
                    'high' AS evidence_confidence,
                    false AS inferred_from_absence
                FROM listing_status_intervals l
                CROSS JOIN params p
                WHERE lower(l.status) = 'inactive'
                  AND l.valid_from IS NOT NULL
                  AND (p.listing_status_source IS NULL OR l.source = p.listing_status_source)

                UNION ALL

                SELECT
                    l.*,
                    'SNAPSHOT_ABSENCE' AS delist_code,
                    l.valid_to AS delist_date,
                    coalesce(l.last_evidence_as_of_date, l.as_of_date, l.valid_to) AS event_as_of_date,
                    coalesce(l.last_evidence_at, l.available_at) AS event_available_at,
                    'snapshot_presence_gap_absence' AS delisting_method,
                    'low' AS evidence_confidence,
                    true AS inferred_from_absence
                FROM listing_status_intervals l
                CROSS JOIN params p
                WHERE p.include_snapshot_absence
                  AND lower(l.status) = 'active'
                  AND l.valid_to IS NOT NULL
                  AND (p.listing_status_source IS NULL OR l.source = p.listing_status_source)
            ),
            enriched AS (
                SELECT
                    p.source,
                    c.source AS listing_status_source,
                    c.listing_status_id,
                    c.security_id,
                    c.symbol,
                    c.listing_venue_code,
                    c.listing_venue_name,
                    c.listing_exchange_code,
                    c.delist_date,
                    c.event_as_of_date AS as_of_date,
                    coalesce(
                        c.event_available_at,
                        CAST(c.event_as_of_date AS TIMESTAMP) + INTERVAL '22 hours'
                    ) AS available_at,
                    c.delist_code,
                    d.description AS delist_reason,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.default_imputed_return
                        ELSE NULL
                    END AS delisting_return,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.imputation_policy
                        ELSE 'UNOBSERVED_PUBLIC_PROXY'
                    END AS delisting_return_type,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN true
                        ELSE false
                    END AS is_return_imputed,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN d.imputation_policy
                        ELSE 'none'
                    END AS return_policy,
                    CASE
                        WHEN p.apply_imputation
                         AND d.imputation_allowed
                         AND d.default_imputed_return IS NOT NULL
                        THEN 'low'
                        ELSE 'none'
                    END AS return_confidence,
                    c.evidence_source,
                    c.evidence_source_table,
                    c.source_event_id,
                    c.source_url,
                    c.delisting_method AS method,
                    c.evidence_confidence,
                    c.inferred_from_absence,
                    c.details_json,
                    p.run_id
                FROM candidates c
                CROSS JOIN params p
                JOIN delist_code_dim d
                  ON d.delist_code = c.delist_code
            )
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        source,
                        listing_status_source,
                        listing_status_id,
                        delist_code,
                        CAST(delist_date AS VARCHAR)
                    )
                ) AS delisting_event_id,
                source,
                listing_status_source,
                listing_status_id AS source_listing_status_id,
                security_id,
                symbol,
                listing_venue_code,
                listing_venue_name,
                listing_exchange_code,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                delist_reason,
                delisting_return,
                delisting_return_type,
                is_return_imputed,
                return_policy,
                return_confidence,
                evidence_source,
                evidence_source_table,
                source_event_id,
                source_url,
                method,
                evidence_confidence,
                inferred_from_absence,
                details_json,
                coalesce(run_id, ?)
            FROM enriched
            """,
            [
                options.source,
                options.listing_status_source,
                options.include_snapshot_absence,
                options.apply_shumway_warther_imputation,
                options.run_id,
                options.run_id,
            ],
        )

    return int(
        store.con.execute(
            """
            SELECT count(*)
            FROM delisting_events
            WHERE source = ?
              AND (? IS NULL OR listing_status_source = ?)
            """,
            [options.source, options.listing_status_source, options.listing_status_source],
        ).fetchone()[0]
    )


class DelistingEventDataset(Dataset):
    dataset_id = "delisting_events"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: DelistingEventOptions) -> DatasetLoadResult:
        rows = refresh_delisting_events(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="delisting_events",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "listing_status_source": options.listing_status_source,
                "include_snapshot_absence": options.include_snapshot_absence,
                "apply_shumway_warther_imputation": options.apply_shumway_warther_imputation,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "listing_status_source": options.listing_status_source,
                "include_snapshot_absence": options.include_snapshot_absence,
                "apply_shumway_warther_imputation": options.apply_shumway_warther_imputation,
            },
        )
