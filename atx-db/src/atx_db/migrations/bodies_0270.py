"""Commercial API pricing, idempotency, quota, and worker-state contracts."""

from __future__ import annotations

import datetime as dt
import hashlib

import duckdb

from ..api.catalog import DATASETS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

TABLES = (
    "api_unit_price_catalog",
    "saas_idempotency_records",
    "saas_usage_events",
    "saas_batch_jobs",
)


def _commercial_api_controls(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS api_unit_price_catalog (
            price_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            mode VARCHAR NOT NULL,
            currency VARCHAR NOT NULL,
            billing_unit VARCHAR NOT NULL,
            unit_price_per_gb DECIMAL(18,6),
            status VARCHAR NOT NULL,
            valid_from TIMESTAMP NOT NULL,
            valid_to TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS saas_idempotency_records (
            account_id VARCHAR NOT NULL,
            endpoint VARCHAR NOT NULL,
            idempotency_key VARCHAR NOT NULL,
            request_sha256 VARCHAR NOT NULL,
            state VARCHAR NOT NULL,
            resource_type VARCHAR,
            resource_id VARCHAR,
            response_status INTEGER,
            response_json VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            completed_at TIMESTAMP,
            expires_at TIMESTAMP NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (account_id, endpoint, idempotency_key)
        );
        ALTER TABLE saas_usage_events
            ADD COLUMN IF NOT EXISTS billable_bytes BIGINT;
        ALTER TABLE saas_usage_events
            ADD COLUMN IF NOT EXISTS cost_usd DECIMAL(24,9);
        ALTER TABLE saas_usage_events
            ADD COLUMN IF NOT EXISTS billing_mode VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS request_sha256 VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS idempotency_key VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS attempt_count INTEGER;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS worker_id VARCHAR;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS lease_expires_at TIMESTAMP;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS next_attempt_at TIMESTAMP;
        ALTER TABLE saas_batch_jobs
            ADD COLUMN IF NOT EXISTS monthly_byte_limit BIGINT;
        UPDATE saas_batch_jobs
        SET request_sha256 = coalesce(request_sha256, sha256(request_json)),
            attempt_count = coalesce(attempt_count, 0);
        UPDATE saas_usage_events
        SET billable_bytes = coalesce(billable_bytes, 0);
        CREATE INDEX IF NOT EXISTS idx_api_unit_prices_lookup
            ON api_unit_price_catalog(dataset_id,schema_code,mode,status,valid_from,valid_to);
        CREATE INDEX IF NOT EXISTS idx_saas_idempotency_expiry
            ON saas_idempotency_records(expires_at,state);
        CREATE INDEX IF NOT EXISTS idx_saas_batch_worker_claim
            ON saas_batch_jobs(state,next_attempt_at,lease_expires_at,received_at);
        CREATE INDEX IF NOT EXISTS idx_saas_usage_billable_month
            ON saas_usage_events(account_id,dataset_id,finished_at,billable_bytes)
        """
    )
    valid_from = dt.datetime(1900, 1, 1)
    price_rows = []
    for dataset in DATASETS:
        for schema in dataset.schemas:
            natural = f"{dataset.code}|{schema.code}|historical|USD|{valid_from.isoformat()}"
            price_rows.append(
                (
                    hashlib.sha256(natural.encode()).hexdigest(),
                    dataset.code,
                    schema.code,
                    "historical",
                    "USD",
                    "uncompressed_arrow_bytes",
                    None,
                    "contract_required",
                    valid_from,
                )
            )
    conn.executemany(
        """
        INSERT OR IGNORE INTO api_unit_price_catalog (
            price_id,dataset_id,schema_code,mode,currency,billing_unit,
            unit_price_per_gb,status,valid_from
        ) VALUES (?,?,?,?,?,?,?,?,?)
        """,
        price_rows,
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "api_unit_price_catalog",
                "api",
                "api_unit_price",
                "dataset_id,schema_code,mode,currency,valid_from",
                "Versioned billable-byte unit prices for public schemas.",
                '["price_id"]',
                "valid_from/valid_to version commercial terms without changing historical invoices.",
            ),
            (
                "saas_idempotency_records",
                "control",
                "idempotency_record",
                "account_id,endpoint,idempotency_key",
                "Account-scoped POST replay protection and response/resource binding.",
                '["account_id","endpoint","idempotency_key"]',
                "Control state expires independently of economic data and is not a PIT fact.",
            ),
        ],
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (
            'atx_api_pricing','atx_warehouse','ATX API unit pricing',
            'Versioned schema prices expressed per decimal gigabyte of uncompressed Arrow bytes.',
            'dataset_id,schema_code,mode,currency,valid_from','api_unit_price_catalog',
            'valid_from','updated_at',now()
        )
        """
    )
    _catalog_fields_for_tables(conn, TABLES)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=270, name="commercial_api_controls", up=_commercial_api_controls)]
