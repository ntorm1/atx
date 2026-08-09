"""Production 13F amendment-chain tables."""

from __future__ import annotations

import duckdb

from ..connection import DuckDBStore
from ..thirteenf_amendments import ensure_thirteenf_amendment_schema
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _thirteenf_amendment_chain_schema(conn: duckdb.DuckDBPyConnection) -> None:
    store = DuckDBStore(":memory:")
    store.connection = conn
    ensure_thirteenf_amendment_schema(store)
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
            (
                'thirteenf_effective_positions', 'atx_warehouse',
                'Effective SEC 13F positions',
                'Manager-quarter position state after applying RESTATEMENT replacements and '
                'ADD NEW HOLDINGS supplements in filing order.',
                'manager_cik,report_period,position_key',
                'thirteenf_effective_positions', 'report_period', 'available_at', now()
            ),
            (
                'thirteenf_amendment_corrections', 'sec_edgar',
                'SEC 13F amendment corrections',
                'Position-level changes introduced by each 13F-HR/A filing after reconstructing '
                'the immediately preceding effective state.',
                'manager_cik,report_period,amendment_accession,position_key',
                'thirteenf_amendment_corrections', 'report_period', 'available_at', now()
            ),
            (
                'thirteenf_amendment_rates', 'atx_warehouse',
                'SEC 13F manager-quarter amendment rates',
                'Distinct corrected positions divided by final effective positions, with a '
                'lookback-only 24-quarter z-score.',
                'manager_cik,report_period',
                'thirteenf_amendment_rates', 'report_period', 'available_at', now()
            )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'thirteenf_effective_positions', 'gold', '13f_effective_position',
                'manager_cik,report_period,position_key',
                'Final manager-quarter position state reconstructed from the complete amendment chain.',
                '["manager_cik","report_period","position_key"]',
                'available_at is the latest filing date in the chain; consumers must not use the '
                'report-period date as an availability timestamp.', now()
            ),
            (
                'thirteenf_amendment_corrections', 'silver', '13f_amendment_correction',
                'manager_cik,report_period,amendment_accession,position_key',
                'Added, removed, or changed position produced by a specific 13F amendment.',
                '["manager_cik","report_period","amendment_accession","position_key"]',
                'available_at is the amendment filing date. Old and new values are reconstructed '
                'only from filings available through that amendment.', now()
            ),
            (
                'thirteenf_amendment_rates', 'gold', '13f_amendment_rate',
                'manager_cik,report_period',
                'Manager-quarter amendment intensity and trailing 24-quarter standardized score.',
                '["manager_cik","report_period"]',
                'The trailing mean and standard deviation exclude the current quarter. available_at '
                'is the latest filing date for the manager-quarter.', now()
            )
        """
    )
    _catalog_fields_for_tables(
        conn,
        (
            "thirteenf_effective_positions",
            "thirteenf_amendment_corrections",
            "thirteenf_amendment_rates",
        ),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=189,
        name="thirteenf_amendment_chain_schema",
        up=_thirteenf_amendment_chain_schema,
    )
]
