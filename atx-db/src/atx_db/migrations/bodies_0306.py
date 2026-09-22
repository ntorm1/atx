"""Explicit Shumway terminal-return policy and the non-vacuous survivorship gate.

Re-seeds ``delist_code_dim`` (four new codes the public evidence streams emit:
``SEC_FORM_25``, ``SEC_FORM_15``, ``ARCHIVE_LAST_TRADE``, ``NASDAQ_FINANCIAL_STATUS_BANKRUPT``)
and ``terminal_return_policy_dim`` (the two ``performance_unknown*`` rows) from the widened Python
tuples in ``atx_db.delisting``, and registers
``delisting_events_without_terminal_return`` in ``quality_check_registry`` --
``atx_db.quality.checks_survivorship.SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME`` -- the
companion check that fails when a delisting event carries no terminal return, so an empty
``delisting_terminal_returns`` can no longer make the critical survivorship check pass on an
empty anti-join (audit sec:4.4).

The INSERT column lists below are hand-verified against the live DDL (``schema.py``:
``delist_code_dim`` and ``terminal_return_policy_dim``), not copied from the brief verbatim --
the brief's own column names (``code_scheme``, ``reason_code``, ``status_label``,
``imputation_note``) do not exist on either table; the real columns are ``code_system``,
``crsp_dlstcd_family``, ``terminal_trading_status``, ``imputation_policy``. ``POLICY_DIM_COLUMNS``
is already the authoritative, verified column order for the policy table.
"""

from __future__ import annotations

import duckdb

from ..delisting import (
    DELIST_CODE_ROWS,
    POLICY_DIM_COLUMNS,
    TERMINAL_RETURN_POLICY_ROWS,
)
from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _delisting_terminal_return_policy(conn: duckdb.DuckDBPyConnection) -> None:
    conn.executemany(
        f"""
        INSERT OR REPLACE INTO terminal_return_policy_dim (
            {", ".join(POLICY_DIM_COLUMNS)}
        ) VALUES ({", ".join("?" for _ in POLICY_DIM_COLUMNS)})
        """,
        [tuple(row) for row in TERMINAL_RETURN_POLICY_ROWS],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO delist_code_dim (
            delist_code, code_system, vendor_code, crsp_dlstcd, crsp_dlstcd_family,
            reason_category, description, terminal_trading_status, imputation_allowed,
            default_imputed_return, imputation_policy, source
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [tuple(row) for row in DELIST_CODE_ROWS],
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
                "delisting_events_without_terminal_return",
                "delisting_terminal_returns",
                "delisting_terminal_returns",
                "error",
                0.0,
                "le",
                True,
                "failed",
                "atx_tier1_parity",
            )
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=306,
        name="delisting_terminal_return_policy",
        up=_delisting_terminal_return_policy,
    )
]
