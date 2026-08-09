"""PF4-S1 migration bodies: signal-evaluation surface (IC / decay / quantile / turnover / crowding / breadth / DQC)."""
from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf4_s1_ic_surface(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S1-0: factor_eval_manifest + factor_ic + factor_ic_decay (rank-IC surface)."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_eval_manifest (
            eval_id VARCHAR PRIMARY KEY,
            factor_id VARCHAR NOT NULL,
            eval_kind VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            start_date DATE,
            end_date DATE,
            horizon_days INTEGER,
            n_quantiles INTEGER,
            evaluation_days BIGINT,
            factor_row_count BIGINT,
            params_json VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_ic (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            mean_rank_ic DOUBLE,
            ic_std DOUBLE,
            ic_information_ratio DOUBLE,
            ic_tstat DOUBLE,
            sign_consistency DOUBLE,
            n_dates BIGINT,
            mean_names DOUBLE,
            universe_id VARCHAR NOT NULL,
            start_date DATE,
            end_date DATE,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_ic_decay (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            ladder_position INTEGER NOT NULL,
            mean_rank_ic DOUBLE,
            decay_ratio DOUBLE,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_eval_manifest','control','factor_eval_manifest','eval_id',
         'Per-factor signal-evaluation manifest (one row per factor/eval-kind/params run) mirroring the alpha-backtest manifest shape.',
         '["eval_id"]',
         'Manifest lineage only; evaluation reads v_factor_panel read-only and never rewrites factor values.', now()),
        ('factor_ic','metric','factor_ic','factor_id,horizon,universe_id,run_id',
         'Per-factor aggregate rank-IC over the horizon ladder: mean rank-IC, IC information ratio, IC t-stat, sign-consistency.',
         '["factor_id","horizon","universe_id","run_id"]',
         'Rank-IC computed cross-sectionally per as_of_date then aggregated across dates; forward returns strictly t+1..t+h.', now()),
        ('factor_ic_decay','metric','factor_ic_decay','factor_id,horizon,universe_id,run_id',
         'Per-factor rank-IC decay profile across the horizon ladder with decay ratio vs the shortest horizon.',
         '["factor_id","horizon","universe_id","run_id"]',
         'Decay = mean rank-IC per horizon ordered by the ladder; no cross-date pooling.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_ic_factor_horizon ON factor_ic(factor_id, horizon)",
        "CREATE INDEX IF NOT EXISTS idx_factor_ic_decay_factor ON factor_ic_decay(factor_id, ladder_position)",
        "CREATE INDEX IF NOT EXISTS idx_factor_eval_manifest_factor_kind ON factor_eval_manifest(factor_id, eval_kind)",
    ):
        conn.execute(stmt)
    _catalog_fields_for_tables(conn, ("factor_eval_manifest", "factor_ic", "factor_ic_decay"))
    _refresh_schema_contract_v2_pin(conn)


def _pf4_s1_quantile_turnover(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S1-1: factor_quantile_spread + factor_turnover (decile spread + turnover)."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_quantile_spread (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            n_quantiles INTEGER NOT NULL,
            quantile INTEGER NOT NULL,
            mean_forward_return DOUBLE,
            mean_factor_value DOUBLE,
            n_obs BIGINT,
            long_short_spread DOUBLE,
            long_short_hit_rate DOUBLE,
            decile_monotonicity DOUBLE,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_turnover (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            n_quantiles INTEGER NOT NULL,
            top_decile_turnover DOUBLE,
            bottom_decile_turnover DOUBLE,
            mean_rank_autocorrelation DOUBLE,
            n_rebalances BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_quantile_spread','metric','factor_quantile_spread','factor_id,horizon,quantile,universe_id,run_id',
         'Per-factor per-decile mean forward return (monotonicity) plus denormalized top-minus-bottom long-short spread, hit-rate, and decile monotonicity.',
         '["factor_id","horizon","quantile","universe_id","run_id"]',
         'Quantile buckets formed within the as-of cross-section only; forward returns future-dated.', now()),
        ('factor_turnover','metric','factor_turnover','factor_id,universe_id,run_id',
         'Per-factor top/bottom-decile membership churn rebalance-to-rebalance plus factor rank autocorrelation.',
         '["factor_id","universe_id","run_id"]',
         'Turnover compares consecutive as-of dates in forward chronological order; no lookahead.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_quantile_spread_factor ON factor_quantile_spread(factor_id, horizon, quantile)",
        "CREATE INDEX IF NOT EXISTS idx_factor_turnover_factor ON factor_turnover(factor_id)",
    ):
        conn.execute(stmt)
    _catalog_fields_for_tables(conn, ("factor_quantile_spread", "factor_turnover"))
    _refresh_schema_contract_v2_pin(conn)


def _pf4_s1_correlation_breadth(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S1-2: factor_correlation + factor_crowding + factor_breadth."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_correlation (
            eval_id VARCHAR NOT NULL,
            factor_id_a VARCHAR NOT NULL,
            factor_id_b VARCHAR NOT NULL,
            mean_correlation DOUBLE,
            mean_abs_correlation DOUBLE,
            n_dates BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_crowding (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            max_abs_correlation DOUBLE,
            avg_abs_correlation DOUBLE,
            most_correlated_factor_id VARCHAR,
            n_peers BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_breadth (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            n_names BIGINT,
            n_non_null BIGINT,
            universe_size BIGINT,
            coverage_fraction DOUBLE,
            effective_breadth DOUBLE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP,
            is_latest_revision BOOLEAN,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_correlation','metric','factor_correlation','factor_id_a,factor_id_b,universe_id,run_id',
         'Pairwise cross-sectional factor-value correlation averaged over dates (ordered pairs a!=b) for redundancy analysis.',
         '["factor_id_a","factor_id_b","universe_id","run_id"]',
         'Correlations computed cross-sectionally per date then averaged; no date pooling.', now()),
        ('factor_crowding','metric','factor_crowding','factor_id,universe_id,run_id',
         'Per-factor crowding = max and average absolute correlation to the rest of the namespace, with the most-correlated peer.',
         '["factor_id","universe_id","run_id"]',
         'Derived from factor_correlation; a factor highly correlated with many others is crowded/redundant.', now()),
        ('factor_breadth','metric','factor_breadth','factor_id,as_of_date,universe_id,run_id',
         'Per-date cross-sectional breadth: non-null name count, as-of universe size, coverage fraction, effective breadth.',
         '["factor_id","as_of_date","universe_id","run_id"]',
         'Breadth is an as-of coverage measure over the as-of universe.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_correlation_a ON factor_correlation(factor_id_a, factor_id_b)",
        "CREATE INDEX IF NOT EXISTS idx_factor_crowding_factor ON factor_crowding(factor_id)",
        "CREATE INDEX IF NOT EXISTS idx_factor_breadth_factor_date ON factor_breadth(factor_id, as_of_date)",
    ):
        conn.execute(stmt)
    # factor_breadth carries as_of_date (a strong bitemporal marker), so the schema contract
    # classifies it as a fact/derived table that must carry the full canonical PIT column set.
    # Rather than exempt it, it declares all five physically: as_of_date + run_id were already
    # present; available_at is populated PIT-correctly in compute_breadth (max input
    # available_at, falling back to a conservative compute-time now()); source_loaded_at /
    # is_latest_revision are auto-filled by db.warehouse._insert_projection on insert. No
    # pit_exemption is needed. factor_correlation / factor_crowding carry no strong marker and
    # are (correctly) not treated as PIT-fact tables.
    _catalog_fields_for_tables(conn, ("factor_correlation", "factor_crowding", "factor_breadth"))
    _refresh_schema_contract_v2_pin(conn)


def _pf4_s1_factor_dqc(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S1-3: factor_dqc_result + quality_check_registry seeds (leakage + coverage, gated)."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_dqc_result (
            check_name VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            observed_value DOUBLE,
            threshold_value DOUBLE,
            severity VARCHAR NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            checked_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_dqc_result','control','factor_dqc_result','check_name,factor_id,run_id',
         'Per-factor data-quality-check outcomes (leakage t+0 probe, coverage) recorded for the gated factor DQC.',
         '["check_name","factor_id","run_id"]',
         'Leakage uses the same-day t+0 return purely as an adversarial probe, never as a scoring target or factor input.', now())
        """
    )
    conn.execute("CREATE INDEX IF NOT EXISTS idx_factor_dqc_result_check ON factor_dqc_result(check_name, factor_id)")
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, now())
        """,
        [
            ("factor_leakage_tplus0", "factor_panel", "v_factor_panel", "critical", 0.0, "eq", True, "failed", "pf4_s1"),
            ("factor_coverage_asof_universe", "factor_panel", "v_factor_panel", "critical", 0.0, "eq", True, "failed", "pf4_s1"),
        ],
    )
    _catalog_fields_for_tables(conn, ("factor_dqc_result",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=176, name="pf4_s1_ic_surface", up=_pf4_s1_ic_surface),
    Migration(version=177, name="pf4_s1_quantile_turnover", up=_pf4_s1_quantile_turnover),
    Migration(version=178, name="pf4_s1_correlation_breadth", up=_pf4_s1_correlation_breadth),
    Migration(version=179, name="pf4_s1_factor_dqc", up=_pf4_s1_factor_dqc),
]
