"""PF4-S4 migration bodies: survivorship-safe returns (delisting-return stitching).

Migration 0185 (delisting_terminal_returns + delisting_code_reconciliation +
terminal_return_policy_dim, the latter seeded from TERMINAL_RETURN_POLICY_ROWS) lands in this
task (PF4-S4 S4-0/S4-1). 0186 (forward_returns_survivorship_safe + its view), 0187 (its
indexes), and 0188 (coverage view + quality_check_registry rows) land in later PF4-S4 tasks and
are appended to this same file/MIGRATIONS list when their tasks land -- do not renumber or edit
0185 to make room for them.

S4-1 note on editing 0185's own body (rather than adding a new migration): the dimension is
policy-as-data, and 0185 has never been applied to any persistent database -- it is this
sprint's own, unreleased migration. The seed below is an idempotent ``INSERT OR REPLACE``
directly over ``db.delisting.TERMINAL_RETURN_POLICY_ROWS``, so the migration and the Python
constant can never drift apart into two hand-maintained copies of the same seed data.
"""
from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf4_s4_delisting_terminal_return_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S4 S4-0/S4-1 (0185): terminal-return catalog + DLSTCD reconciliation + policy dim.

    ``delisting_terminal_returns`` collapses ``delisting_return_observations`` to one
    realized terminal return per ``(security_id, delist_date)`` (S4-0, observed only); S4-1
    adds the deterministic corporate-action policy path via ``terminal_return_policy_dim``,
    which this same function seeds (idempotent ``INSERT OR REPLACE`` over
    ``db.delisting.TERMINAL_RETURN_POLICY_ROWS``). ``delisting_code_reconciliation`` reports --
    never overwrites -- agreement between the vendor ``crsp_dlstcd`` and the warehouse's own
    public ``delist_code`` proxy.

    The plan's own ``delisting_code_reconciliation`` DDL omitted ``is_latest_revision`` while
    declaring ``as_of_date`` (a strong PIT marker), which would fail ``pit_column_presence_check``
    as an offender. The real column is added here (not a ``pit_exemption``): reconciliation
    status genuinely revises on re-run as new vendor evidence lands, unlike a one-shot
    monitoring/detection table.
    """
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_terminal_returns (
            terminal_return_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,          -- delisting-confirmation timestamp; never the event date
            terminal_return DOUBLE NOT NULL,          -- realized DLRET (fraction, may be negative)
            terminal_return_ex_div DOUBLE,
            terminal_return_source VARCHAR NOT NULL,  -- observed | policy | none  (imputed is never written here)
            terminal_return_policy VARCHAR,           -- FK-ish to terminal_return_policy_dim.policy_code when source='policy'
            crsp_dlstcd INTEGER,
            return_basis VARCHAR,
            successor_security_id VARCHAR,
            return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_code_reconciliation (
            reconciliation_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            warehouse_delist_code VARCHAR,
            warehouse_reason_category VARCHAR,
            vendor_crsp_dlstcd INTEGER,
            vendor_dlstcd_family VARCHAR,
            reconciliation_status VARCHAR NOT NULL,   -- match | mismatch | vendor_only | warehouse_only | unmapped
            mismatch_reason VARCHAR,
            delisting_event_id VARCHAR,
            delisting_return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS terminal_return_policy_dim (
            policy_code VARCHAR PRIMARY KEY,
            corporate_action_type VARCHAR NOT NULL,
            terminal_return_basis VARCHAR NOT NULL,
            combine_successor BOOLEAN NOT NULL DEFAULT false,
            default_return DOUBLE,
            is_observed_required BOOLEAN NOT NULL DEFAULT false,
            description VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    # S4-1: policy-as-data seed. Deferred import (not at module top) so this migration body
    # module never depends on db.delisting at import time -- only when this migration actually
    # runs -- keeping the migrations package importable standalone. INSERT OR REPLACE keyed on
    # policy_code (the table's PRIMARY KEY) makes re-applying this migration idempotent: it
    # always leaves exactly len(TERMINAL_RETURN_POLICY_ROWS) rows, never duplicates.
    from ..delisting import TERMINAL_RETURN_POLICY_ROWS

    conn.executemany(
        """
        INSERT OR REPLACE INTO terminal_return_policy_dim (
            policy_code, corporate_action_type, terminal_return_basis,
            combine_successor, default_return, is_observed_required, description
        )
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        list(TERMINAL_RETURN_POLICY_ROWS),
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
            (
                'delisting_terminal_returns', 'atx_warehouse',
                'Observed delisting terminal returns',
                'One realized terminal return per delisted (security_id, delist_date), collapsed '
                'from delisting_return_observations (latest-visible observation wins). '
                'terminal_return_source is observed or policy; imputed values are never persisted.',
                'source,security_id,delist_date',
                'delisting_terminal_returns', 'as_of_date', 'available_at', now()
            ),
            (
                'delisting_code_reconciliation', 'atx_warehouse',
                'Vendor DLSTCD vs. warehouse delist_code reconciliation',
                'Reports agreement/disagreement between the vendor crsp_dlstcd (coarse-mapped to a '
                'family) and the warehouse public delist_code proxy reason_category. Reports only; '
                'never overwrites the warehouse delist_code.',
                'source,security_id,delist_date',
                'delisting_code_reconciliation', 'as_of_date', 'available_at', now()
            ),
            (
                'terminal_return_policy_dim', 'atx_warehouse',
                'Deterministic corporate-action terminal-return policy',
                'Dimension of deterministic terminal-return policies for non-observed corporate '
                'actions (spinoff/merger/etc.), seeded by this same migration from '
                'db.delisting.TERMINAL_RETURN_POLICY_ROWS (PF4-S4 S4-1).',
                'policy_code',
                'terminal_return_policy_dim', NULL, NULL, now()
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
                'delisting_terminal_returns', 'gold', 'delisting_terminal_return',
                'source,security_id,delist_date',
                'One realized terminal return per delisted security-day; the survivorship-safe '
                'forward-return stitch point. terminal_return_source in (observed, policy); '
                'imputed is never written here.',
                '["source","security_id","delist_date"]',
                'available_at inherits the delisting_return_observations confirmation timestamp '
                '(never the delist event date) for observed rows.',
                now()
            ),
            (
                'delisting_code_reconciliation', 'control', 'delisting_code_reconciliation',
                'source,security_id,delist_date',
                'Vendor DLSTCD (coarse family) vs. warehouse delist_code proxy reason_category '
                'reconciliation. reconciliation_status in (match, mismatch, vendor_only, '
                'warehouse_only, unmapped); reports only, never overwrites delist_code.',
                '["source","security_id","delist_date"]',
                'available_at is max(event.available_at, observation.available_at). '
                'is_latest_revision tracks that reconciliation_status can change on re-run as '
                'new vendor evidence lands.',
                now()
            ),
            (
                'terminal_return_policy_dim', 'control', 'terminal_return_policy_dim',
                'policy_code',
                'Deterministic spinoff/merger terminal-return policy dimension; seeded here '
                '(PF4-S4 S4-1) via an idempotent INSERT OR REPLACE over '
                'db.delisting.TERMINAL_RETURN_POLICY_ROWS -- the migration is the single '
                'source of truth, there is no runtime seeder.',
                '["policy_code"]',
                'Dimension table; carries none of the three strong PIT temporal markers, so no '
                'PIT columns or exemption are required.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(
        conn,
        ("delisting_terminal_returns", "delisting_code_reconciliation", "terminal_return_policy_dim"),
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('delisting_terminal_returns', 'terminal_return', 'measure',
             'Realized terminal return fraction (may be negative; a -1.0 is a total loss).',
             false, 'fraction', 'delisting_return_observations.delisting_return', now()),
            ('delisting_terminal_returns', 'terminal_return_ex_div', 'measure',
             'Realized terminal return fraction excluding the final dividend, when the vendor '
             'reports it separately.',
             true, 'fraction', 'delisting_return_observations.delisting_return_ex_div', now()),
            ('delisting_terminal_returns', 'terminal_return_source', 'category',
             'observed (vendor DLRET) or policy (deterministic corporate-action policy); '
             'imputed is never persisted here.',
             false, NULL, NULL, now()),
            ('delisting_code_reconciliation', 'vendor_dlstcd_family', 'category',
             'Coarse CRSP DLSTCD family: 2xx merger, 3xx exchange, 4xx liquidation, 5xx dropped.',
             true, NULL, 'delisting_return_observations.crsp_dlstcd', now()),
            ('delisting_code_reconciliation', 'reconciliation_status', 'category',
             'match, mismatch, vendor_only, warehouse_only, or unmapped. mismatch is expected '
             'and non-failing; only unmapped is a genuine gap.',
             false, NULL, NULL, now()),
            ('terminal_return_policy_dim', 'default_return', 'measure',
             'Deterministic default terminal return fraction for this policy, when the policy '
             'does not require an observed input.',
             true, 'fraction', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=185,
        name="pf4_s4_delisting_terminal_return_catalog",
        up=_pf4_s4_delisting_terminal_return_catalog,
    ),
]
