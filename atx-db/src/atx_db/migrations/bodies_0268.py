"""Add serving-path indexes for the SaaS control plane."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _saas_serving_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE UNIQUE INDEX IF NOT EXISTS idx_saas_api_keys_prefix
            ON saas_api_keys(key_prefix);
        CREATE INDEX IF NOT EXISTS idx_saas_api_keys_account_status
            ON saas_api_keys(account_id,status);
        CREATE INDEX IF NOT EXISTS idx_saas_entitlements_account_dataset
            ON saas_entitlements(account_id,dataset_id,valid_from,valid_to);
        CREATE INDEX IF NOT EXISTS idx_saas_usage_account_started
            ON saas_usage_events(account_id,started_at);
        CREATE INDEX IF NOT EXISTS idx_saas_usage_dataset_started
            ON saas_usage_events(dataset_id,started_at);
        CREATE INDEX IF NOT EXISTS idx_saas_batch_account_received
            ON saas_batch_jobs(account_id,received_at);
        CREATE INDEX IF NOT EXISTS idx_saas_batch_state_received
            ON saas_batch_jobs(state,received_at)
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=268, name="saas_serving_indexes", up=_saas_serving_indexes)]
