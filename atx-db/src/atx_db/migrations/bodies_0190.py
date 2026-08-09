"""Consensus 13F amendment signal tables."""

from __future__ import annotations

import duckdb

from ..connection import DuckDBStore
from ..thirteenf_signals import ensure_thirteenf_signal_schema
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _thirteenf_consensus_signal_schema(conn: duckdb.DuckDBPyConnection) -> None:
    store = DuckDBStore(":memory:")
    store.connection = conn
    ensure_thirteenf_signal_schema(store)
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
            (
                'thirteenf_amendment_market_regimes', 'atx_warehouse',
                '13F amendment market regimes',
                'Cross-manager amendment breadth and trailing-only stress classification.',
                'report_period', 'thirteenf_amendment_market_regimes',
                'report_period', 'available_at', now()
            ),
            (
                'thirteenf_consensus_amendment_signals', 'atx_warehouse',
                'Consensus 13F amendment signals',
                'CUSIPs corrected by multiple managers whose own amendment rates are anomalous.',
                'report_period,cusip', 'thirteenf_consensus_amendment_signals',
                'report_period', 'signal_available_at', now()
            ),
            (
                'thirteenf_consensus_signal_outcomes', 'atx_warehouse',
                'Consensus 13F signal disclosed-exit outcomes',
                'Next-quarter disclosed position exits; not inferred transaction dates.',
                'signal_id', 'thirteenf_consensus_signal_outcomes',
                'report_period', 'signal_available_at', now()
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
                'thirteenf_amendment_market_regimes', 'gold', '13f_amendment_regime',
                'report_period', 'Quarterly cross-manager amendment breadth and stress score.',
                '["report_period"]',
                'Trailing baseline excludes the current quarter; available_at is the last manager filing date.', now()
            ),
            (
                'thirteenf_consensus_amendment_signals', 'gold', '13f_consensus_signal',
                'report_period,cusip', 'Multi-filer consensus amendment signals ranked within quarter.',
                '["report_period","cusip"]',
                'signal_available_at is the latest contributing amendment filing date.', now()
            ),
            (
                'thirteenf_consensus_signal_outcomes', 'research', '13f_signal_outcome',
                'signal_id', 'Next-disclosed-filing position exit outcomes for consensus signals.',
                '["signal_id"]',
                'Quarterly disclosures cannot reveal actual trade dates; outcomes are explicitly disclosure-based.', now()
            )
        """
    )
    _catalog_fields_for_tables(
        conn,
        (
            "thirteenf_amendment_market_regimes",
            "thirteenf_consensus_amendment_signals",
            "thirteenf_consensus_signal_outcomes",
        ),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=190,
        name="thirteenf_consensus_signal_schema",
        up=_thirteenf_consensus_signal_schema,
    )
]
