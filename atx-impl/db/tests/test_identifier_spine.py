from __future__ import annotations

import datetime as dt

import duckdb
import pandas as pd


def _old_spine_conn(path=":memory:"):
    conn = duckdb.connect(str(path))
    conn.execute(
        """
        CREATE TABLE table_catalog (
            table_name VARCHAR PRIMARY KEY,
            layer VARCHAR,
            entity VARCHAR,
            grain VARCHAR,
            description VARCHAR,
            natural_key_json VARCHAR,
            pit_notes VARCHAR,
            updated_at TIMESTAMP
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE field_catalog (
            table_name VARCHAR NOT NULL,
            field_name VARCHAR NOT NULL,
            semantic_type VARCHAR,
            description VARCHAR,
            nullable BOOLEAN,
            unit VARCHAR,
            source_field VARCHAR,
            updated_at TIMESTAMP,
            PRIMARY KEY (table_name, field_name)
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE securities (
            security_id VARCHAR PRIMARY KEY,
            issuer_id VARCHAR,
            primary_symbol VARCHAR,
            name VARCHAR,
            asset_class VARCHAR NOT NULL DEFAULT 'EQUITY',
            country VARCHAR NOT NULL DEFAULT 'US',
            currency VARCHAR NOT NULL DEFAULT 'USD',
            active BOOLEAN NOT NULL DEFAULT true,
            first_seen_date DATE,
            last_seen_date DATE,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE security_identifier_history (
            security_id VARCHAR NOT NULL,
            id_type VARCHAR NOT NULL,
            id_value VARCHAR NOT NULL,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT INTO securities (
            security_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES
            ('SEC-CIK-0000320193', 'CIK-0000320193', 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '2020-01-01', NULL, 'fixture'),
            ('SEC-CIK-0000789019', NULL, 'MSFT', 'Microsoft Corp.', 'EQUITY', 'US', 'USD', true, DATE '2021-02-03', NULL, 'fixture')
        """
    )
    conn.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES
            ('SEC-CIK-0000320193', 'CIK', '0000320193', DATE '2019-05-04', NULL, DATE '2019-05-04', TIMESTAMP '2019-05-05 12:00:00', 'fixture', NULL),
            ('SEC-CIK-0000789019', 'CIK', '0000789019', DATE '2021-02-03', NULL, DATE '2021-02-03', TIMESTAMP '2021-02-04 12:00:00', 'fixture', NULL)
        """
    )
    return conn


def _index_names(conn) -> set[str]:
    return {row[0] for row in conn.execute("SELECT index_name FROM duckdb_indexes()").fetchall()}


def _record_migrations_through(conn, max_version: int) -> None:
    from db.migrations import MIGRATIONS

    conn.execute(
        """
        CREATE TABLE schema_migrations (
            version VARCHAR PRIMARY KEY,
            description VARCHAR NOT NULL,
            checksum VARCHAR,
            applied_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    rows = [
        (str(migration.version).zfill(4), migration.name)
        for migration in MIGRATIONS
        if migration.version <= max_version
    ]
    conn.executemany(
        "INSERT INTO schema_migrations (version, description) VALUES (?, ?)",
        rows,
    )


def test_legacy_pre_0079_initialize_applies_identifier_spine_after_safe_view_bootstrap(tmp_path):
    from db.connection import DuckDBStore

    db_path = tmp_path / "legacy_pre_0079.duckdb"
    conn = _old_spine_conn(db_path)
    _record_migrations_through(conn, 78)
    conn.close()

    with DuckDBStore(db_path) as store:
        versions = {
            int(row[0]): row[1]
            for row in store.con.execute(
                """
                SELECT version, description
                FROM schema_migrations
                WHERE version IN ('0079', '0080')
                """
            ).fetchall()
        }
        assert versions == {
            79: "identifier_spine_schema_catalog",
            80: "identifier_spine_indexes",
        }

        columns = {
            row[0]
            for row in store.con.execute(
                """
                SELECT column_name
                FROM duckdb_columns()
                WHERE table_name = 'securities'
                """
            ).fetchall()
        }
        assert "entity_id" in columns

        view_entities = dict(
            store.con.execute(
                """
                SELECT security_id, entity_id
                FROM v_security_master_current
                ORDER BY security_id
                """
            ).fetchall()
        )
        assert view_entities == {
            "SEC-CIK-0000320193": "CIK-0000320193",
            "SEC-CIK-0000789019": "CIK-0000789019",
        }


def test_identifier_spine_migration_0079_schema_catalog_backfill_idempotent():
    from db.migrations import MIGRATIONS

    conn = _old_spine_conn()
    migration_0079 = {migration.version: migration for migration in MIGRATIONS}[79]

    migration_0079.up(conn)
    migration_0079.up(conn)

    securities_columns = {
        row[0]
        for row in conn.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'securities'
            """
        ).fetchall()
    }
    identifier_columns = {
        row[0]
        for row in conn.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'security_identifier_history'
            """
        ).fetchall()
    }
    assert "entity_id" in securities_columns
    assert "internal_cusip" in identifier_columns
    assert _index_names(conn).isdisjoint(
        {
            "idx_securities_entity_id",
            "idx_security_identifier_history_entity_asof",
            "idx_security_identifier_history_internal_cusip",
        }
    )

    entities = dict(conn.execute("SELECT security_id, entity_id FROM securities").fetchall())
    assert entities == {
        "SEC-CIK-0000320193": "CIK-0000320193",
        "SEC-CIK-0000789019": "CIK-0000789019",
    }
    assert all(entities.values())

    history_rows = conn.execute(
        """
        SELECT security_id, id_value, valid_from, source, run_id
        FROM security_identifier_history
        WHERE id_type = 'ENTITY_ID'
        ORDER BY security_id
        """
    ).fetchall()
    assert history_rows == [
        (
            "SEC-CIK-0000320193",
            "CIK-0000320193",
            dt.date(2019, 5, 4),
            "migration_0079_identifier_spine",
            "migration-0079",
        ),
        (
            "SEC-CIK-0000789019",
            "CIK-0000789019",
            dt.date(2021, 2, 3),
            "migration_0079_identifier_spine",
            "migration-0079",
        ),
    ]

    assert conn.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'ENTITY_ID'"
    ).fetchone()[0] == 2

    cataloged = {
        tuple(row)
        for row in conn.execute(
            """
            SELECT table_name, field_name, semantic_type
            FROM field_catalog
            WHERE (table_name, field_name) IN (
                ('securities', 'entity_id'),
                ('security_identifier_history', 'internal_cusip')
            )
            """
        ).fetchall()
    }
    assert cataloged == {
        ("securities", "entity_id", "identifier"),
        ("security_identifier_history", "internal_cusip", "identifier"),
    }


def test_identifier_spine_migration_0080_indexes_only():
    from db.migrations import MIGRATIONS

    conn = _old_spine_conn()
    migrations = {migration.version: migration for migration in MIGRATIONS}

    migrations[79].up(conn)
    before = _index_names(conn)
    migrations[80].up(conn)
    migrations[80].up(conn)
    after = _index_names(conn)

    expected = {
        "idx_securities_entity_id",
        "idx_security_identifier_history_entity_asof",
        "idx_security_identifier_history_internal_cusip",
    }
    assert expected.issubset(after)
    assert expected.isdisjoint(before)
    assert {
        row[0]
        for row in conn.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'securities'
            """
        ).fetchall()
    } == {
        "security_id",
        "entity_id",
        "issuer_id",
        "primary_symbol",
        "name",
        "asset_class",
        "country",
        "currency",
        "active",
        "first_seen_date",
        "last_seen_date",
        "source",
        "source_loaded_at",
    }


def test_identifier_spine_bootstrap_records_migrations_columns_catalog_indexes(tmp_store):
    versions = {
        int(row[0]): row[1]
        for row in tmp_store.con.execute(
            """
            SELECT version, description
            FROM schema_migrations
            WHERE version IN ('0079', '0080')
            """
        ).fetchall()
    }
    assert versions == {
        79: "identifier_spine_schema_catalog",
        80: "identifier_spine_indexes",
    }

    fields = {
        tuple(row)
        for row in tmp_store.con.execute(
            """
            SELECT table_name, field_name
            FROM field_catalog
            WHERE (table_name, field_name) IN (
                ('securities', 'entity_id'),
                ('security_identifier_history', 'internal_cusip')
            )
            """
        ).fetchall()
    }
    assert fields == {
        ("securities", "entity_id"),
        ("security_identifier_history", "internal_cusip"),
    }
    assert {
        "idx_securities_entity_id",
        "idx_security_identifier_history_entity_asof",
        "idx_security_identifier_history_internal_cusip",
    }.issubset(_index_names(tmp_store.con))


def test_security_master_upsert_backfills_current_cik_entity(tmp_store):
    from db.security_master import normalize_company_tickers, upsert_security_master_from_frame

    frame = normalize_company_tickers(
        {
            "0": {"cik_str": 320193, "ticker": "AAPL", "title": "Apple Inc."},
        }
    )
    assert frame.iloc[0]["security_id"] == "SEC-CIK-0000320193"
    assert frame.iloc[0]["entity_id"] == "CIK-0000320193"

    upsert_security_master_from_frame(tmp_store, frame, source="fixture-security-master", run_id="run-1")

    security = tmp_store.con.execute(
        """
        SELECT security_id, entity_id, issuer_id
        FROM securities
        WHERE security_id = 'SEC-CIK-0000320193'
        """
    ).fetchone()
    assert security == ("SEC-CIK-0000320193", "CIK-0000320193", "CIK-0000320193")

    id_types = {
        row[0]: row[1]
        for row in tmp_store.con.execute(
            """
            SELECT id_type, id_value
            FROM security_identifier_history
            WHERE security_id = 'SEC-CIK-0000320193'
              AND source = 'fixture-security-master'
            """
        ).fetchall()
    }
    assert id_types == {
        "CIK": "0000320193",
        "ENTITY_ID": "CIK-0000320193",
        "TICKER": "AAPL",
    }


def test_security_entity_ids_asof_merger_fixture_no_lookahead(tmp_store):
    from db.security_master import ENTITY_IDENTIFIER_TYPE, security_entity_ids_asof

    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES
            ('SEC-CIK-0000000001', 'CIK-0000000001', 'CIK-0000000001', 'ACQ', 'Acquirer Inc.', 'EQUITY', 'US', 'USD', true, DATE '2020-01-01', NULL, 'fixture'),
            ('SEC-CIK-0000000002', 'CIK-0000000001', 'CIK-0000000002', 'TGT', 'Target Inc.', 'EQUITY', 'US', 'USD', true, DATE '2020-01-01', NULL, 'fixture')
        """
    )
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES
            ('SEC-CIK-0000000001', ?, 'CIK-0000000001', DATE '2020-01-01', NULL, DATE '2020-01-01', TIMESTAMP '2020-01-02 09:30:00', 'fixture-merger', NULL),
            ('SEC-CIK-0000000002', ?, 'CIK-0000000002', DATE '2020-01-01', DATE '2024-06-01', DATE '2020-01-01', TIMESTAMP '2020-01-02 09:30:00', 'fixture-merger', NULL),
            ('SEC-CIK-0000000002', ?, 'CIK-0000000001', DATE '2024-06-01', NULL, DATE '2024-06-01', TIMESTAMP '2024-06-01 12:00:00', 'fixture-merger', NULL)
        """,
        [ENTITY_IDENTIFIER_TYPE, ENTITY_IDENTIFIER_TYPE, ENTITY_IDENTIFIER_TYPE],
    )

    before = security_entity_ids_asof(
        tmp_store,
        ["SEC-CIK-0000000001", "SEC-CIK-0000000002"],
        as_of_date=dt.date(2024, 5, 31),
        as_of_ts=dt.datetime(2024, 5, 31, 23, 59, 59),
    )
    assert before == {
        "SEC-CIK-0000000001": "CIK-0000000001",
        "SEC-CIK-0000000002": "CIK-0000000002",
    }

    pre_availability = security_entity_ids_asof(
        tmp_store,
        ["SEC-CIK-0000000001", "SEC-CIK-0000000002"],
        as_of_date=dt.date(2024, 6, 1),
        as_of_ts=dt.datetime(2024, 6, 1, 9, 0, 0),
    )
    assert pre_availability == {"SEC-CIK-0000000001": "CIK-0000000001"}

    after = security_entity_ids_asof(
        tmp_store,
        ["SEC-CIK-0000000001", "SEC-CIK-0000000002"],
        as_of_date=dt.date(2024, 6, 1),
        as_of_ts=dt.datetime(2024, 6, 1, 13, 0, 0),
    )
    assert after == {
        "SEC-CIK-0000000001": "CIK-0000000001",
        "SEC-CIK-0000000002": "CIK-0000000001",
    }


# ---------------------------------------------------------------------------
# S5-4: export-scan quality check.
#
# security_identifier_history.internal_cusip is internal-only matching support
# (see field_catalog description seeded in migration 0079) and must never
# appear in a lake-exported / public / catalogued-public object. The boundary
# today is enforced by OMISSION from lake.DEFAULT_EXPORT_OBJECTS (that table is
# simply not in the allowlist). This check makes the boundary an enforced
# invariant instead of a tribal-knowledge omission: it fails if any object in
# DEFAULT_EXPORT_OBJECTS carries a column named internal_cusip, or if
# security_identifier_history itself (the only table with that column) is ever
# added to the export allowlist.
# ---------------------------------------------------------------------------


def test_export_scan_no_internal_cusip_check_passes_on_clean_export_allowlist(tmp_store):
    from db.quality import run_warehouse_quality_checks

    results = run_warehouse_quality_checks(tmp_store, record=False)
    by_name = {r.check_name: r for r in results}
    result = by_name["export_scan_internal_cusip_leak"]
    assert result.status == "passed", result.details
    assert result.observed_value == 0.0


def test_export_scan_no_internal_cusip_check_fails_when_export_object_carries_internal_cusip(tmp_store):
    from db.quality import _export_scan_internal_cusip_sql

    # Lower-level proof: the SQL builder itself flags any object list member
    # with an internal_cusip column, using a decoy table shaped like an
    # export-allowlisted object.
    tmp_store.con.execute(
        "CREATE TABLE leaky_export_decoy (security_id VARCHAR, internal_cusip VARCHAR)"
    )
    sql = _export_scan_internal_cusip_sql(("leaky_export_decoy",))
    observed = tmp_store.con.execute(sql).fetchone()[0]
    assert observed == 1.0


def test_export_scan_internal_cusip_leak_check_fails_end_to_end_through_registered_check(tmp_store):
    # End-to-end proof (not just the SQL-builder helper): make a REAL
    # DEFAULT_EXPORT_OBJECTS member -- v_security_master_current, which is a
    # view -- actually carry an internal_cusip column, then run the check the
    # way production does, through run_warehouse_quality_checks. This proves
    # the registered SqlQualityCheck (its sql= wiring, threshold, and
    # comparator) genuinely catches a real leak, not just that the builder
    # function returns the right SQL in isolation. A regression that
    # disconnected sql= from _export_scan_internal_cusip_sql(DEFAULT_EXPORT_OBJECTS),
    # or that flipped the comparator/threshold, would be invisible to the
    # SQL-builder-only test above but must fail this one.
    from db.lake import DEFAULT_EXPORT_OBJECTS
    from db.quality import run_warehouse_quality_checks
    from db.schema import create_security_master_current_view

    assert "v_security_master_current" in DEFAULT_EXPORT_OBJECTS

    # Rename the real production view out of the way, then stand up a
    # same-named decoy that layers an internal_cusip column on top of it.
    # This avoids hand-duplicating create_security_master_current_view's
    # schema-drift-sensitive column list (e.g. the legacy-vs-fresh entity_id
    # branch) while still making the actual exported object name carry the
    # forbidden column.
    tmp_store.con.execute("ALTER VIEW v_security_master_current RENAME TO v_security_master_current_real")
    try:
        tmp_store.con.execute(
            """
            CREATE OR REPLACE VIEW v_security_master_current AS
            SELECT
                real.*,
                any_value(cusip.internal_cusip) AS internal_cusip
            FROM v_security_master_current_real real
            LEFT JOIN security_identifier_history cusip
              ON cusip.security_id = real.security_id
             AND cusip.id_type = 'CUSIP'
             AND cusip.valid_to IS NULL
            GROUP BY ALL
            """
        )
        results = run_warehouse_quality_checks(tmp_store, record=False)
        by_name = {r.check_name: r for r in results}
        result = by_name["export_scan_internal_cusip_leak"]
        assert result.status == "failed", result.details
        assert result.observed_value == 1.0
    finally:
        # Restore the real view via the production builder (not a
        # hand-duplicated copy) so this test cannot silently drift from the
        # real schema, and so tmp_store is left clean for anything that
        # inspects v_security_master_current afterwards.
        tmp_store.con.execute("DROP VIEW IF EXISTS v_security_master_current")
        tmp_store.con.execute("DROP VIEW IF EXISTS v_security_master_current_real")
        create_security_master_current_view(tmp_store.con)

    after = run_warehouse_quality_checks(tmp_store, record=False)
    after_by_name = {r.check_name: r for r in after}
    assert after_by_name["export_scan_internal_cusip_leak"].status == "passed"


def test_export_scan_sql_scopes_to_default_export_objects():
    from db.lake import DEFAULT_EXPORT_OBJECTS
    from db.quality import _export_scan_internal_cusip_sql

    sql = _export_scan_internal_cusip_sql(DEFAULT_EXPORT_OBJECTS)
    for object_name in DEFAULT_EXPORT_OBJECTS:
        assert object_name in sql


def test_export_scan_sql_empty_object_list_is_a_safe_no_op():
    from db.quality import _export_scan_internal_cusip_sql

    sql = _export_scan_internal_cusip_sql(())
    assert "duckdb_columns" not in sql


def test_default_export_objects_never_include_security_identifier_history():
    # security_identifier_history is the only table carrying internal_cusip.
    # It must never be added to the lake export allowlist -- this is the
    # cheapest, most direct enforcement of the 13f_holdings.md B.3 boundary.
    from db.lake import DEFAULT_EXPORT_OBJECTS

    assert "security_identifier_history" not in DEFAULT_EXPORT_OBJECTS


# ---------------------------------------------------------------------------
# S5-4: migration 0083 one-time self-overlap repair.
#
# identifier_same_source_self_overlaps was driven to 0 once (S32, migration
# 0054) but reaccumulated (528 rows per PARITY_GAP.md) because later write
# paths did not all guard against re-inserting an open-ended row for a key
# that already has one. Migration 0083 is the matching one-time repair,
# mirroring migration 0054's _repair_identifier_history_overlaps.
# ---------------------------------------------------------------------------


def _dup_identifier_row(*, security_id, id_type, id_value, valid_from, available_at, source):
    return {
        "security_id": security_id,
        "id_type": id_type,
        "id_value": id_value,
        "valid_from": valid_from,
        "valid_to": None,
        "as_of_date": valid_from,
        "available_at": available_at,
        "source": source,
        "run_id": None,
    }


def test_migration_0083_repairs_reaccumulated_self_overlaps(tmp_store):
    from db.migrations import MIGRATIONS
    from db.quality import run_warehouse_quality_checks

    # Reproduce the reaccumulation this migration exists to fix: two
    # open-ended rows for the same (security_id, id_type, id_value, source)
    # key, as if a post-S32 write path (e.g. a resolution-decision re-apply)
    # inserted a second row instead of updating the first.
    rows = [
        _dup_identifier_row(
            security_id="SEC-CIK-0000320193",
            id_type="FIGI",
            id_value="BBG000B9XRY4",
            valid_from=dt.date(2026, 5, 1),
            available_at=dt.datetime(2026, 5, 2, 22, 0, 0),
            source="OpenFIGI",
        ),
        _dup_identifier_row(
            security_id="SEC-CIK-0000320193",
            id_type="FIGI",
            id_value="BBG000B9XRY4",
            valid_from=dt.date(2026, 5, 15),
            available_at=dt.datetime(2026, 5, 16, 22, 0, 0),
            source="OpenFIGI",
        ),
    ]
    frame_cols = ["security_id", "id_type", "id_value", "valid_from", "valid_to", "as_of_date", "available_at", "source", "run_id"]
    frame = pd.DataFrame(rows)[frame_cols]
    tmp_store.con.register("seed_dupes", frame)
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history
            (security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id)
        SELECT security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id
        FROM seed_dupes
        """
    )
    tmp_store.con.unregister("seed_dupes")

    before = tmp_store.con.execute(
        """
        SELECT count(*) FROM security_identifier_history a JOIN security_identifier_history b
          ON a.security_id=b.security_id AND a.id_type=b.id_type AND a.id_value=b.id_value AND a.source=b.source
         AND a.valid_from < b.valid_from
         AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert before > 0

    migration_0083 = {migration.version: migration for migration in MIGRATIONS}[83]
    migration_0083.up(tmp_store.con)

    after = tmp_store.con.execute(
        """
        SELECT count(*) FROM security_identifier_history a JOIN security_identifier_history b
          ON a.security_id=b.security_id AND a.id_type=b.id_type AND a.id_value=b.id_value AND a.source=b.source
         AND a.valid_from < b.valid_from
         AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert after == 0

    kept = tmp_store.con.execute(
        "SELECT valid_from FROM security_identifier_history WHERE id_type = 'FIGI' AND source = 'OpenFIGI'"
    ).fetchall()
    assert kept == [(dt.date(2026, 5, 1),)]

    results = run_warehouse_quality_checks(tmp_store, record=False)
    by_name = {r.check_name: r for r in results}
    result = by_name["identifier_same_source_self_overlaps"]
    assert result.status == "passed", result.details
    assert result.observed_value == 0.0


def test_migration_0083_is_idempotent(tmp_store):
    from db.migrations import MIGRATIONS

    # Seed the same reaccumulated-self-overlap shape as
    # test_migration_0083_repairs_reaccumulated_self_overlaps, plus an
    # unrelated clean row, so a second .up() call that silently deletes rows
    # it shouldn't touch (not just "fails to raise") is caught.
    rows = [
        _dup_identifier_row(
            security_id="SEC-CIK-0000320193",
            id_type="FIGI",
            id_value="BBG000B9XRY4",
            valid_from=dt.date(2026, 5, 1),
            available_at=dt.datetime(2026, 5, 2, 22, 0, 0),
            source="OpenFIGI",
        ),
        _dup_identifier_row(
            security_id="SEC-CIK-0000320193",
            id_type="FIGI",
            id_value="BBG000B9XRY4",
            valid_from=dt.date(2026, 5, 15),
            available_at=dt.datetime(2026, 5, 16, 22, 0, 0),
            source="OpenFIGI",
        ),
    ]
    frame_cols = ["security_id", "id_type", "id_value", "valid_from", "valid_to", "as_of_date", "available_at", "source", "run_id"]
    frame = pd.DataFrame(rows)[frame_cols]
    tmp_store.con.register("seed_dupes_idempotency", frame)
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history
            (security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id)
        SELECT security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id
        FROM seed_dupes_idempotency
        """
    )
    tmp_store.con.unregister("seed_dupes_idempotency")

    migration_0083 = {migration.version: migration for migration in MIGRATIONS}[83]
    migration_0083.up(tmp_store.con)

    row_count_after_first = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history"
    ).fetchone()[0]
    self_overlaps_after_first = tmp_store.con.execute(
        """
        SELECT count(*) FROM security_identifier_history a JOIN security_identifier_history b
          ON a.security_id=b.security_id AND a.id_type=b.id_type AND a.id_value=b.id_value AND a.source=b.source
         AND a.valid_from < b.valid_from
         AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert self_overlaps_after_first == 0

    # No further duplicates to repair -- the migration must be a safe no-op
    # on a re-run (e.g. on a fresh bootstrap where it runs as part of
    # apply_pending_migrations, or if it is ever re-applied). Row count and
    # the self-overlap outcome must be UNCHANGED, not just "does not raise" --
    # a second run that silently deletes unrelated rows would otherwise pass
    # undetected.
    migration_0083.up(tmp_store.con)

    row_count_after_second = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history"
    ).fetchone()[0]
    self_overlaps_after_second = tmp_store.con.execute(
        """
        SELECT count(*) FROM security_identifier_history a JOIN security_identifier_history b
          ON a.security_id=b.security_id AND a.id_type=b.id_type AND a.id_value=b.id_value AND a.source=b.source
         AND a.valid_from < b.valid_from
         AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert row_count_after_second == row_count_after_first
    assert self_overlaps_after_second == self_overlaps_after_first == 0


def test_migration_0083_preserves_closed_intervals(tmp_store):
    from db.migrations import MIGRATIONS

    # A genuine ticker change: closed old interval + open new interval for a
    # DIFFERENT id_value under the same key prefix. Must survive the repair
    # untouched -- the collapse only ever touches valid_to IS NULL rows.
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history
            (security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id)
        VALUES
            ('SEC-CIK-0000320193', 'TICKER', 'OLDTICK', DATE '2015-01-01', DATE '2020-01-01', DATE '2015-01-01', TIMESTAMP '2015-01-02 12:00:00', 'fixture-tickerchange', NULL),
            ('SEC-CIK-0000320193', 'TICKER', 'NEWTICK', DATE '2020-01-01', NULL, DATE '2020-01-01', TIMESTAMP '2020-01-02 12:00:00', 'fixture-tickerchange', NULL)
        """
    )

    migration_0083 = {migration.version: migration for migration in MIGRATIONS}[83]
    migration_0083.up(tmp_store.con)

    rows = tmp_store.con.execute(
        """
        SELECT id_value, valid_from, valid_to
        FROM security_identifier_history
        WHERE source = 'fixture-tickerchange'
        ORDER BY valid_from
        """
    ).fetchall()
    assert rows == [
        ("OLDTICK", dt.date(2015, 1, 1), dt.date(2020, 1, 1)),
        ("NEWTICK", dt.date(2020, 1, 1), None),
    ]
