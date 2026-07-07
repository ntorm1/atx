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
    # factor_breadth carries as_of_date (a strong bitemporal marker) as the cross-section
    # label of a *derived, deterministically-recomputable* evaluation metric -- not a
    # bitemporal fact row. as_of_date + run_id carry its PIT/lineage semantics; per-row
    # available_at / source_loaded_at / is_latest_revision are not meaningful for a metric
    # recomputed wholesale by evaluate_panel. Register that narrowly with the schema-contract
    # PIT-exemption registry (mirroring the est_* dimension exemptions) so
    # pit_column_presence_check stays green. factor_correlation / factor_crowding carry no
    # strong marker and need no exemption.
    conn.execute(
        """
        INSERT OR REPLACE INTO pit_exemption (
            table_name, missing_columns, reason, exempted_by, exempted_at, source_loaded_at
        )
        VALUES (
            'factor_breadth',
            '["available_at","source_loaded_at","is_latest_revision"]',
            'factor_breadth is a derived, deterministically-recomputable signal-evaluation metric keyed by (factor_id, as_of_date, universe_id, run_id). Its as_of_date is the cross-section label and run_id carries lineage; per-row available_at, source_loaded_at, and is_latest_revision are not meaningful for a metric recomputed wholesale from the read-only panel.',
            'pf4-s1-s1-2',
            now(),
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("factor_correlation", "factor_crowding", "factor_breadth"))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=176, name="pf4_s1_ic_surface", up=_pf4_s1_ic_surface),
    Migration(version=177, name="pf4_s1_quantile_turnover", up=_pf4_s1_quantile_turnover),
    Migration(version=178, name="pf4_s1_correlation_breadth", up=_pf4_s1_correlation_breadth),
    # 0179 appended by a later task in this same sprint
]
