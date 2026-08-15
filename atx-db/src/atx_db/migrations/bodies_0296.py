"""Reproducible batch-query and immutable manifest identity."""

from __future__ import annotations

import duckdb

from ..api.catalog import DATASETS, _record_schema_sha256
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _reproducible_batch_manifests(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE api_schema_catalog
            ADD COLUMN IF NOT EXISTS schema_sha256 VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS schema_version VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS schema_sha256 VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS query_sha256 VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS logical_content_sha256 VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS manifest_uri VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS manifest_sha256 VARCHAR;
        """
    )
    rows = [
        (schema.version, _record_schema_sha256(schema), dataset.code, schema.code)
        for dataset in DATASETS
        for schema in dataset.schemas
    ]
    conn.executemany(
        """
        UPDATE api_schema_catalog
        SET schema_sha256=?,updated_at=now()
        WHERE dataset_id=? AND schema_code=? AND schema_version=?
        """,
        [(digest, dataset_id, schema_code, version) for version, digest, dataset_id, schema_code in rows],
    )
    conn.execute(
        """
        UPDATE saas_batch_jobs AS job
        SET schema_version=catalog.schema_version,
            schema_sha256=catalog.schema_sha256,
            query_sha256=coalesce(job.query_sha256,sha256(job.request_json))
        FROM (
            SELECT * EXCLUDE (schema_rank)
            FROM (
                SELECT *,row_number() OVER (
                    PARTITION BY dataset_id,schema_code
                    ORDER BY updated_at DESC,schema_version DESC
                ) AS schema_rank
                FROM api_schema_catalog
                WHERE is_active
            )
            WHERE schema_rank=1
        ) AS catalog
        WHERE job.dataset_id=catalog.dataset_id
          AND job.schema_code=catalog.schema_code
          AND job.schema_version IS NULL
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET description='Asynchronous export jobs with pinned query/schema contracts, encoding-independent content identity, and immutable checksummed artifact manifests.',
            pit_notes='The accepted query pins as_of, query_sha256, schema version/hash, and the completed artifact plus logical-content and manifest hashes.',
            updated_at=now()
        WHERE table_name='saas_batch_jobs'
        """
    )
    _catalog_fields_for_tables(conn, ("api_schema_catalog", "saas_batch_jobs"))
    _refresh_schema_contract_v2_pin(conn)


def _reproducible_batch_manifest_indexes_and_gates(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    # DuckDB can fail when a populated table is altered and indexed in the same
    # transaction, so the index intentionally follows the column migration.
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_saas_batch_manifest
            ON saas_batch_jobs(account_id,state,manifest_sha256,completed_at)
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "completed_batch_jobs_missing_reproducibility_identity",
                "atx_saas_control",
                "saas_batch_jobs",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "malformed_batch_manifest_hashes",
                "atx_saas_control",
                "saas_batch_jobs",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
        ],
    )
MIGRATIONS = [
    Migration(
        version=296,
        name="reproducible_batch_manifests",
        up=_reproducible_batch_manifests,
    ),
    Migration(
        version=297,
        name="reproducible_batch_manifest_indexes_and_gates",
        up=_reproducible_batch_manifest_indexes_and_gates,
    ),
]
