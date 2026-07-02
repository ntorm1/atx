from __future__ import annotations

import datetime as dt

import duckdb


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
