"""Create the versioned SaaS API contract and control-plane schema."""

from __future__ import annotations

import json

import duckdb

from ..api.catalog import DATASETS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

TABLES = (
    "api_dataset_catalog",
    "api_schema_catalog",
    "api_field_catalog",
    "saas_accounts",
    "saas_api_keys",
    "saas_entitlements",
    "saas_usage_events",
    "saas_batch_jobs",
)


def _seed_public_contract(conn: duckdb.DuckDBPyConnection) -> None:
    for dataset in DATASETS:
        conn.execute(
            """
            INSERT OR REPLACE INTO api_dataset_catalog (
                dataset_id,contract_version,title,description,asset_class,region,
                entitlement_code,default_schema,is_active,source_loaded_at,updated_at
            ) VALUES (?,?,?,?,?,?,?,?,true,now(),now())
            """,
            [
                dataset.code,
                dataset.version,
                dataset.title,
                dataset.description,
                dataset.asset_class,
                dataset.region,
                dataset.entitlement,
                dataset.default_schema,
            ],
        )
        for schema in dataset.schemas:
            conn.execute(
                """
                INSERT OR REPLACE INTO api_schema_catalog (
                    dataset_id,schema_code,schema_version,title,description,source_table,
                    time_field,natural_key_json,pit_policy,supported_encodings_json,
                    max_sync_rows,is_active,source_loaded_at,updated_at
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,true,now(),now())
                """,
                [
                    dataset.code,
                    schema.code,
                    schema.version,
                    schema.title,
                    schema.description,
                    schema.source_table,
                    schema.time_column,
                    json.dumps(schema.natural_key, separators=(",", ":")),
                    "available_at_lte_request_as_of_then_select_requested_revision_vintage",
                    '["json","jsonl","csv","parquet","arrow"]',
                    schema.max_sync_rows,
                ],
            )
            conn.execute(
                "DELETE FROM api_field_catalog WHERE dataset_id=? AND schema_code=? AND schema_version=?",
                [dataset.code, schema.code, schema.version],
            )
            conn.executemany(
                """
                INSERT INTO api_field_catalog (
                    dataset_id,schema_code,schema_version,ordinal,field_name,source_column,
                    data_type,semantic_type,unit,nullable,is_filterable,description,
                    source_loaded_at,updated_at
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,now(),now())
                """,
                [
                    (
                        dataset.code,
                        schema.code,
                        schema.version,
                        ordinal,
                        field.name,
                        field.source_column,
                        field.data_type,
                        "identifier" if field.name.endswith("_id") else "observation",
                        field.unit,
                        field.nullable,
                        field.filterable,
                        field.description,
                    )
                    for ordinal, field in enumerate(schema.fields, start=1)
                ],
            )


def _saas_api_contract(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS api_dataset_catalog (
            dataset_id VARCHAR PRIMARY KEY,
            contract_version VARCHAR NOT NULL,
            title VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            asset_class VARCHAR NOT NULL,
            region VARCHAR NOT NULL,
            entitlement_code VARCHAR NOT NULL,
            default_schema VARCHAR NOT NULL,
            is_active BOOLEAN NOT NULL DEFAULT true,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS api_schema_catalog (
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            schema_version VARCHAR NOT NULL,
            title VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            source_table VARCHAR NOT NULL,
            time_field VARCHAR NOT NULL,
            natural_key_json VARCHAR NOT NULL,
            pit_policy VARCHAR NOT NULL,
            supported_encodings_json VARCHAR NOT NULL,
            max_sync_rows BIGINT NOT NULL,
            is_active BOOLEAN NOT NULL DEFAULT true,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id,schema_code,schema_version)
        );
        CREATE TABLE IF NOT EXISTS api_field_catalog (
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            schema_version VARCHAR NOT NULL,
            ordinal INTEGER NOT NULL,
            field_name VARCHAR NOT NULL,
            source_column VARCHAR NOT NULL,
            data_type VARCHAR NOT NULL,
            semantic_type VARCHAR NOT NULL,
            unit VARCHAR,
            nullable BOOLEAN NOT NULL,
            is_filterable BOOLEAN NOT NULL,
            description VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id,schema_code,schema_version,field_name)
        );
        CREATE TABLE IF NOT EXISTS saas_accounts (
            account_id VARCHAR PRIMARY KEY,
            account_name VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            plan_code VARCHAR NOT NULL,
            billing_customer_ref VARCHAR,
            monthly_spend_limit_usd DECIMAL(18,6),
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS saas_api_keys (
            key_id VARCHAR PRIMARY KEY,
            account_id VARCHAR NOT NULL,
            key_prefix VARCHAR NOT NULL,
            secret_digest VARCHAR NOT NULL,
            digest_algorithm VARCHAR NOT NULL DEFAULT 'sha256',
            status VARCHAR NOT NULL,
            scopes_json VARCHAR NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            expires_at TIMESTAMP,
            last_used_at TIMESTAMP,
            revoked_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS saas_entitlements (
            entitlement_id VARCHAR PRIMARY KEY,
            account_id VARCHAR NOT NULL,
            dataset_id VARCHAR NOT NULL,
            allowed_schemas_json VARCHAR NOT NULL,
            valid_from TIMESTAMP NOT NULL,
            valid_to TIMESTAMP,
            max_sync_rows BIGINT NOT NULL,
            requests_per_minute BIGINT NOT NULL,
            bytes_per_month BIGINT,
            created_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS saas_usage_events (
            request_id VARCHAR PRIMARY KEY,
            account_id VARCHAR NOT NULL,
            key_id VARCHAR NOT NULL,
            endpoint VARCHAR NOT NULL,
            dataset_id VARCHAR,
            schema_code VARCHAR,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP NOT NULL,
            status_code INTEGER NOT NULL,
            record_count BIGINT NOT NULL,
            response_bytes BIGINT NOT NULL,
            billed_units DECIMAL(24,9) NOT NULL DEFAULT 0,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS saas_batch_jobs (
            job_id VARCHAR PRIMARY KEY,
            account_id VARCHAR NOT NULL,
            key_id VARCHAR NOT NULL,
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            request_json VARCHAR NOT NULL,
            encoding VARCHAR NOT NULL,
            compression VARCHAR NOT NULL,
            state VARCHAR NOT NULL,
            record_count BIGINT NOT NULL DEFAULT 0,
            billed_bytes BIGINT NOT NULL DEFAULT 0,
            package_bytes BIGINT NOT NULL DEFAULT 0,
            result_uri VARCHAR,
            result_sha256 VARCHAR,
            error_code VARCHAR,
            error_message VARCHAR,
            received_at TIMESTAMP NOT NULL DEFAULT now(),
            queued_at TIMESTAMP,
            processing_started_at TIMESTAMP,
            completed_at TIMESTAMP,
            expires_at TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
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
                "api_dataset_catalog",
                "api",
                "api_dataset",
                "dataset_id",
                "Versioned public dataset contract.",
                '["dataset_id"]',
                "Contract metadata; not an economic fact table.",
            ),
            (
                "api_schema_catalog",
                "api",
                "api_schema",
                "dataset_id,schema_code,schema_version",
                "Versioned public record-schema contract.",
                '["dataset_id","schema_code","schema_version"]',
                "Each version pins PIT and natural-key semantics.",
            ),
            (
                "api_field_catalog",
                "api",
                "api_field",
                "dataset_id,schema_code,schema_version,field_name",
                "Versioned public field dictionary.",
                '["dataset_id","schema_code","schema_version","field_name"]',
                "Field metadata belongs to a versioned public schema.",
            ),
            (
                "saas_accounts",
                "control",
                "customer_account",
                "account_id",
                "Customer account and plan state.",
                '["account_id"]',
                "Mutable control-plane state, not market data.",
            ),
            (
                "saas_api_keys",
                "control",
                "api_key",
                "key_id",
                "Hashed API-key credentials; plaintext secrets are never stored.",
                '["key_id"]',
                "Credential lifecycle state.",
            ),
            (
                "saas_entitlements",
                "control",
                "entitlement",
                "entitlement_id",
                "Dataset and schema entitlements with quota limits.",
                '["entitlement_id"]',
                "Validity is bitemporal customer-control state.",
            ),
            (
                "saas_usage_events",
                "control",
                "usage_event",
                "request_id",
                "Append-only request metering for audit and billing.",
                '["request_id"]',
                "started_at and finished_at are server event time.",
            ),
            (
                "saas_batch_jobs",
                "control",
                "batch_job",
                "job_id",
                "Asynchronous export job state and immutable result metadata.",
                '["job_id"]',
                "Request parameters plus received/queued/processed/completed timestamps make delivery reproducible.",
            ),
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "atx_api_contract",
                "atx_warehouse",
                "ATX public API contract",
                "Versioned customer-visible dataset, schema, and field catalog.",
                "dataset_id,schema_code,schema_version,field_name",
                "api_schema_catalog",
                "schema_version",
                "updated_at",
            ),
            (
                "atx_saas_control",
                "atx_warehouse",
                "ATX SaaS control plane",
                "Accounts, API keys, entitlements, usage, and batch jobs.",
                "account_id,request_id,job_id",
                "saas_accounts",
                "created_at",
                "updated_at",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, TABLES)
    _seed_public_contract(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=267, name="saas_api_contract_and_control_plane", up=_saas_api_contract)]
