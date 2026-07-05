"""Test the versioned migration framework."""

from __future__ import annotations


def test_migrations_recorded_after_bootstrap(tmp_store):
    """After bootstrap, schema_migrations records every registered migration."""
    from db.migrations import MIGRATIONS

    rows = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$' ORDER BY 1"
    ).fetchall()
    versions = [row[0] for row in rows]
    expected = [migration.version for migration in MIGRATIONS]
    assert versions == expected


def test_apply_pending_idempotent(tmp_store):
    """Calling apply_pending_migrations again returns [] — already up to date."""
    from db.migrations import apply_pending_migrations

    result = apply_pending_migrations(tmp_store.con)
    assert result == [], f"Expected [] but got {result}"


def test_migration_0002_columns_exist(tmp_store):
    """Columns added by migration 0002 (schema_evolution_alters) must be present."""
    cols_query = """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main'
          AND table_name = ?
    """
    # equity_daily_bars gains vendor_security_id and available_at
    bar_cols = {
        row[0]
        for row in tmp_store.con.execute(cols_query, ["equity_daily_bars"]).fetchall()
    }
    assert "vendor_security_id" in bar_cols, "equity_daily_bars.vendor_security_id missing"
    assert "available_at" in bar_cols, "equity_daily_bars.available_at missing"
    assert "run_id" in bar_cols, "equity_daily_bars.run_id missing"

    # etl_job_definitions gains max_retries and retry_delay_seconds
    job_cols = {
        row[0]
        for row in tmp_store.con.execute(cols_query, ["etl_job_definitions"]).fetchall()
    }
    assert "max_retries" in job_cols, "etl_job_definitions.max_retries missing"
    assert "retry_delay_seconds" in job_cols, "etl_job_definitions.retry_delay_seconds missing"


def test_migrations_ordered_ascending():
    """MIGRATIONS registry must be in ascending version order."""
    from db.migrations import MIGRATIONS

    versions = [m.version for m in MIGRATIONS]
    assert versions == sorted(versions), f"Migrations not in ascending order: {versions}"


def test_migrations_unique_versions():
    """Each migration version must be unique."""
    from db.migrations import MIGRATIONS

    versions = [m.version for m in MIGRATIONS]
    assert len(versions) == len(set(versions)), f"Duplicate migration versions: {versions}"


def test_migration_0046_catalogs_short_interest_acceleration_fields(tmp_store):
    """Migration 0046 adds and catalogs short-interest acceleration metrics."""

    expected = {
        "short_interest_momentum_6",
        "days_to_cover_change_6",
        "short_interest_change_pct_accel",
        "days_to_cover_change_accel",
        "short_pressure_score",
        "is_persistent_short_pressure",
    }
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'short_interest_metrics'
            """
        ).fetchall()
    }
    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'short_interest_metrics'
            """
        ).fetchall()
    }
    assert expected.issubset(columns)
    assert expected.issubset(fields)


def test_migration_0047_catalogs_short_interest_liquidity_pressure_fields(tmp_store):
    """Migration 0047 adds and catalogs tradeable short-pressure diagnostics."""

    expected = {
        "average_daily_volume_percentile",
        "days_to_cover_winsorized",
        "days_to_cover_winsorized_zscore",
        "liquid_short_pressure_score",
        "is_liquid_short_pressure",
    }
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'short_interest_metrics'
            """
        ).fetchall()
    }
    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'short_interest_metrics'
            """
        ).fetchall()
    }
    assert expected.issubset(columns)
    assert expected.issubset(fields)


def test_migration_0048_catalogs_macro_real_rates(tmp_store):
    """Migration 0048 updates macro_metrics catalog text for real-rate series."""

    row = tmp_store.con.execute(
        """
        SELECT d.description, t.pit_notes
        FROM dataset_catalog d
        JOIN table_catalog t ON t.table_name = d.primary_table
        WHERE d.dataset_id = 'macro_metrics'
        """
    ).fetchone()
    assert row is not None
    assert "REAL_FEDFUNDS" in row[0]
    assert "Synthetic cross-series" in row[1]


def test_migration_0049_catalogs_finra_daily_short_volume(tmp_store):
    """Migration 0049 adds FINRA daily short-volume raw and metric tables."""

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND table_name IN ('finra_short_volume', 'short_volume_metrics')
            """
        ).fetchall()
    }
    assert tables == {"finra_short_volume", "short_volume_metrics"}

    datasets = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT dataset_id
            FROM dataset_catalog
            WHERE dataset_id IN ('finra_short_volume', 'short_volume_metrics')
            """
        ).fetchall()
    }
    assert datasets == {"finra_short_volume", "short_volume_metrics"}

    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'short_volume_metrics'
            """
        ).fetchall()
    }
    assert {
        "short_volume_ratio",
        "short_volume_ratio_percentile",
        "dominant_market_share_pct",
        "is_high_short_flow",
    }.issubset(fields)


def test_migration_0050_catalogs_offexchange_quality_report(tmp_store):
    """Migration 0050 adds the off-exchange control-quality report table."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'offexchange_quality_report'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_row = tmp_store.con.execute(
        """
        SELECT d.dataset_id, d.grain, t.layer, t.pit_notes
        FROM dataset_catalog d
        JOIN table_catalog t ON t.table_name = d.primary_table
        WHERE d.dataset_id = 'offexchange_quality_report'
        """
    ).fetchone()
    assert catalog_row is not None
    assert catalog_row[1] == "surface,input_source,period_type,period_start_date"
    assert catalog_row[2] == "control"
    assert "available_at" in catalog_row[3]

    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'offexchange_quality_report'
            """
        ).fetchall()
    }
    assert {
        "surface",
        "row_count",
        "short_volume_ratio",
        "ats_share_pct",
        "restated_key_count",
        "multiple_latest_key_count",
    }.issubset(fields)


def test_migration_0051_catalogs_insider_transaction_metrics(tmp_store):
    """Migration 0051 adds the derived insider transaction signal table."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'insider_transaction_metrics'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_row = tmp_store.con.execute(
        """
        SELECT d.dataset_id, d.grain, t.layer, t.pit_notes
        FROM dataset_catalog d
        JOIN table_catalog t ON t.table_name = d.primary_table
        WHERE d.dataset_id = 'insider_transaction_metrics'
        """
    ).fetchone()
    assert catalog_row is not None
    assert catalog_row[1] == "source,security_id,signal_date,window_days"
    assert catalog_row[2] == "gold"
    assert "available_at" in catalog_row[3]

    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name = 'insider_transaction_metrics'
            """
        ).fetchall()
    }
    assert {
        "open_market_purchase_count",
        "plan_sale_value_ratio",
        "cluster_buyer_count",
        "cluster_purchase_value",
        "is_cluster_buy",
        "is_10b5_1_heavy_sale",
    }.issubset(fields)


def test_migration_0052_catalogs_form144_reconciliation(tmp_store):
    """Migration 0052 adds PIT Form 144 intent and execution-reconciliation metadata."""

    intent_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'form144_intent'
            """
        ).fetchall()
    }
    link_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'form144_to_form4_link'
            """
        ).fetchall()
    }
    assert {
        "seller_name_norm",
        "seller_cik",
        "sale_window_end_date",
        "approx_price_per_share",
        "source_file_sha256",
        "as_of_date",
        "available_at",
    }.issubset(intent_columns)
    assert {
        "match_status",
        "shares_matched",
        "share_match_ratio",
        "sale_date",
        "as_of_date",
        "available_at",
        "details_json",
    }.issubset(link_columns)

    datasets = {
        row[0]: row[1:]
        for row in tmp_store.con.execute(
            """
            SELECT d.dataset_id, d.grain, t.layer, t.pit_notes
            FROM dataset_catalog d
            JOIN table_catalog t ON t.table_name = d.primary_table
            WHERE d.dataset_id IN ('form144_intent', 'form144_to_form4_link')
            """
        ).fetchall()
    }
    assert datasets["form144_intent"][0] == "accession_number"
    assert datasets["form144_to_form4_link"][0] == "form144_filing_id,insider_transaction_id"
    assert datasets["form144_intent"][1] == "bronze"
    assert datasets["form144_to_form4_link"][1] == "silver"
    assert "available_at" in datasets["form144_intent"][2]
    assert "available_at" in datasets["form144_to_form4_link"][2]

    fields = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT field_name
            FROM field_catalog
            WHERE table_name IN ('form144_intent', 'form144_to_form4_link')
            """
        ).fetchall()
    }
    assert {"seller_name_norm", "match_method", "match_status", "share_match_ratio"}.issubset(fields)


def test_migration_0042_catalogs_corporate_action_split_metrics(tmp_store):
    """Migration 0042 adds and catalogs split-event reconciliation metrics."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'corporate_action_split_metrics'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'corporate_action_split_metrics'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'corporate_action_split_metrics'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'corporate_action_split_metrics'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0043_catalogs_thirteenf_option_metrics(tmp_store):
    """Migration 0043 adds and catalogs issuer-level 13F option metrics."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'thirteenf_option_metrics'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'thirteenf_option_metrics'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'thirteenf_option_metrics'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'thirteenf_option_metrics'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0044_catalogs_corporate_action_factor_reconciliation(tmp_store):
    """Migration 0044 adds and catalogs event-level factor reconciliation controls."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'corporate_action_factor_reconciliation'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'corporate_action_factor_reconciliation'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'corporate_action_factor_reconciliation'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'corporate_action_factor_reconciliation'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0045_catalogs_thirteenf_concentration_metrics(tmp_store):
    """Migration 0045 adds and catalogs issuer-level 13F concentration metrics."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'thirteenf_concentration_metrics'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'thirteenf_concentration_metrics'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'thirteenf_concentration_metrics'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'thirteenf_concentration_metrics'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_description_recorded(tmp_store):
    """The description (name) column should be stored alongside the version."""
    rows = tmp_store.con.execute(
        "SELECT version, description FROM schema_migrations WHERE version ~ '^[0-9]+$' ORDER BY version"
    ).fetchall()
    by_version = {int(row[0]): row[1] for row in rows}
    assert by_version.get(1) == "baseline_schema", f"Expected baseline_schema, got {by_version.get(1)!r}"
    assert by_version.get(2) == "schema_evolution_alters", (
        f"Expected schema_evolution_alters, got {by_version.get(2)!r}"
    )
    assert by_version.get(3) == "reference_classifications", (
        f"Expected reference_classifications, got {by_version.get(3)!r}"
    )


def test_migration_0003_tables_exist(tmp_store):
    """Tables introduced by migration 0003 must exist after bootstrap."""
    tables_query = """
        SELECT table_name
        FROM information_schema.tables
        WHERE table_schema = 'main'
    """
    table_names = {row[0] for row in tmp_store.con.execute(tables_query).fetchall()}
    for expected in ("taxonomy", "taxonomy_node", "entity_classification", "taxonomy_mapping"):
        assert expected in table_names, f"Table '{expected}' missing after bootstrap"


def test_migration_0010_shares_outstanding_history_exists(tmp_store):
    """Migration 0010 adds the PIT share-count spine."""
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'shares_outstanding_history'
            """
        ).fetchall()
    }
    assert {
        "share_history_id",
        "security_id",
        "share_count_type",
        "effective_date",
        "available_at",
        "share_count",
    }.issubset(columns)


def test_migration_0011_adjustment_factor_tables_exist(tmp_store):
    """Migration 0011 adds corporate-action type and adjustment-factor history tables."""
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'main'
            """
        ).fetchall()
    }
    assert {"corp_action_type_dim", "adjustment_factor_history"}.issubset(tables)

    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'adjustment_factor_history'
            """
        ).fetchall()
    }
    assert {
        "adjustment_factor_id",
        "security_id",
        "ex_date",
        "event_type",
        "classification_reason",
        "factor_price",
        "factor_shares",
        "available_at",
    }.issubset(columns)


def test_migration_0021_catalogs_est_recommendation_fields(tmp_store):
    """Migration 0021 backfills field_catalog for recommendation-event columns."""

    missing = tmp_store.con.execute(
        """
        SELECT c.column_name
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'est_recommendation'
          AND NOT EXISTS (
              SELECT 1
              FROM field_catalog f
              WHERE f.table_name = c.table_name
                AND f.field_name = c.column_name
          )
        ORDER BY c.column_index
        """
    ).fetchall()
    assert missing == []


def test_migration_0022_catalogs_est_recommendation_identifiers(tmp_store):
    """Migration 0022 preserves identifier semantics for key recommendation fields."""

    rows = tmp_store.con.execute(
        """
        SELECT field_name, semantic_type, description, unit, source_field
        FROM field_catalog
        WHERE table_name = 'est_recommendation'
          AND field_name IN ('price_target', 'symbol', 'source_file_sha256')
        ORDER BY field_name
        """
    ).fetchall()
    assert rows == [
        (
            "price_target",
            "price",
            "Broker price target value.",
            "target_currency",
            None,
        ),
        (
            "source_file_sha256",
            "identifier",
            "SHA-256 hash of the injected source file.",
            None,
            None,
        ),
        (
            "symbol",
            "identifier",
            "symbol field on est_recommendation.",
            None,
            None,
        ),
    ]


def test_migration_0024_catalogs_est_recommendation_summary(tmp_store):
    """Migration 0024 indexes and catalogs aggregate recommendation snapshots."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'est_recommendation_summary'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'est_recommendation_summary'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'est_recommendation_summary'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'est_recommendation_summary'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0026_catalogs_est_security_link(tmp_store):
    """Migration 0026 indexes and catalogs estimate security links."""

    table_exists = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = 'est_security_link'
        """
    ).fetchone()[0]
    assert table_exists == 1

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'est_security_link'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'est_security_link'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'est_security_link'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0027_catalogs_est_guidance_extraction(tmp_store):
    """Migration 0027 adds SEC guidance extraction lineage columns."""

    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'est_guidance'
            """
        ).fetchall()
    }
    assert {
        "est_guidance_id",
        "guidance_type",
        "currency",
        "unit",
        "units_scale",
        "source_item",
        "extraction_confidence",
        "evidence_text",
        "source_file",
        "source_file_sha256",
        "raw_payload_json",
    }.issubset(columns)

    catalog_rows = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'est_guidance'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'est_guidance'),
            (
                SELECT count(*)
                FROM duckdb_columns() c
                WHERE c.schema_name = 'main'
                  AND c.table_name = 'est_guidance'
                  AND NOT EXISTS (
                      SELECT 1
                      FROM field_catalog f
                      WHERE f.table_name = c.table_name
                        AND f.field_name = c.column_name
                  )
            )
        """
    ).fetchone()
    assert catalog_rows == (1, 1, 0)


def test_migration_0013_daily_adjustment_factors_exist(tmp_store):
    """Migration 0013 adds PIT daily adjustment factors."""
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'daily_adjustment_factors'
            """
        ).fetchall()
    }
    assert {
        "daily_adjustment_id",
        "security_id",
        "trade_date",
        "as_of_date",
        "split_price_factor",
        "split_share_factor",
        "dividend_total_return_factor",
        "split_adjusted_close",
        "total_return_adjusted_close",
        "available_at",
    }.issubset(columns)


def test_migration_0014_delisting_events_exist(tmp_store):
    """Migration 0014 adds public delisting evidence and code dimension tables."""
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'main'
            """
        ).fetchall()
    }
    assert {"delist_code_dim", "delisting_events"}.issubset(tables)

    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'delisting_events'
            """
        ).fetchall()
    }
    assert {
        "delisting_event_id",
        "source_listing_status_id",
        "delist_date",
        "available_at",
        "delist_code",
        "delisting_return",
        "is_return_imputed",
        "return_policy",
        "evidence_confidence",
    }.issubset(columns)


def test_migration_0015_delisting_return_observations_exist(tmp_store):
    """Migration 0015 adds injectable observed terminal-return facts."""
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'main'
            """
        ).fetchall()
    }
    assert "delisting_return_observations" in tables

    event_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'delisting_events'
            """
        ).fetchall()
    }
    assert {
        "return_observation_id",
        "return_observation_source",
        "return_observation_provider",
    }.issubset(event_columns)

    observation_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'delisting_return_observations'
            """
        ).fetchall()
    }
    assert {
        "delisting_return_observation_id",
        "provider",
        "vendor_security_id",
        "delist_date",
        "delisting_return",
        "available_at",
        "return_basis",
    }.issubset(observation_columns)
