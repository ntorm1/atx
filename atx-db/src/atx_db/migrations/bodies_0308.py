"""Publication release ledger: one row per release, one per release dataset."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _publication_releases(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS publication_releases (
            release_id VARCHAR PRIMARY KEY,
            contract_version VARCHAR NOT NULL,
            out_dir VARCHAR NOT NULL,
            manifest_sha256 VARCHAR NOT NULL,
            dataset_count INTEGER NOT NULL,
            total_rows BIGINT NOT NULL,
            previous_release_id VARCHAR,
            created_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE TABLE IF NOT EXISTS publication_release_datasets (
            release_id VARCHAR NOT NULL,
            dataset_name VARCHAR NOT NULL,
            object_name VARCHAR NOT NULL,
            schema_code VARCHAR,
            parquet_path VARCHAR NOT NULL,
            row_count BIGINT NOT NULL,
            byte_count BIGINT NOT NULL,
            schema_sha256 VARCHAR NOT NULL,
            query_sha256 VARCHAR NOT NULL,
            parquet_sha256 VARCHAR NOT NULL,
            rows_added BIGINT,
            rows_removed BIGINT,
            rows_changed BIGINT,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (release_id, dataset_name)
        );
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "publication_releases",
                "control",
                "publication_release",
                "release_id",
                "One row per scheduled full-universe publication, with the manifest hash "
                "and the release it diffs against.",
                '["release_id"]',
                "created_at is a warehouse load stamp and is never a point-in-time key.",
            ),
            (
                "publication_release_datasets",
                "control",
                "publication_release_dataset",
                "release_id,dataset_name",
                "One row per dataset in a release: Parquet path, row count, schema/query/"
                "file hashes, and the added/removed/changed counts against the previous "
                "release.",
                '["release_id","dataset_name"]',
                "Lineage only; the published Parquet files carry the PIT columns.",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, ("publication_releases", "publication_release_datasets"))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=308, name="publication_releases", up=_publication_releases)]
