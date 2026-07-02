"""PF-S5 S5-3: link sec_company_facts (and, by inheritance, statement points and
ratios) to the stable security/entity spine established by S5-0/S5-1/S5-2.

Covers the brief's three accept criteria:
  1. Every sec_company_facts row resolves to a security_id + entity_id, or lands
     in identifier_resolution_candidates as unresolved (never silently dropped).
  2. fundamental_statement_points (and therefore ratios, which read security_id
     off statement points) carry the resolved security_id.
  3. The as-of reader returns the filing-time-correct identifier for a fixture
     with a mid-history entity change, honoring available_at (no lookahead).
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


def _seed_spine(store, *, cik="0000320193", security_id="SEC-CIK-0000320193"):
    store.con.execute(
        """
        INSERT INTO securities (
            security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES (?, ?, ?, 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-01-01', NULL, 'fixture')
        """,
        [security_id, f"CIK-{cik}", f"CIK-{cik}"],
    )
    store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES
            (?, 'CIK', ?, DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL),
            (?, 'ENTITY_ID', ?, DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL)
        """,
        [security_id, cik, security_id, f"CIK-{cik}"],
    )


class TestSecCompanyFactsColumn:
    def test_migration_0082_adds_entity_id_column_and_catalog(self, tmp_store):
        columns = {
            row[0]
            for row in tmp_store.con.execute(
                """
                SELECT column_name
                FROM duckdb_columns()
                WHERE table_name = 'sec_company_facts'
                """
            ).fetchall()
        }
        assert "security_id" in columns
        assert "entity_id" in columns

        fields = {
            row[0]
            for row in tmp_store.con.execute(
                """
                SELECT field_name
                FROM field_catalog
                WHERE table_name = 'sec_company_facts'
                  AND field_name IN ('security_id', 'entity_id')
                """
            ).fetchall()
        }
        assert fields == {"security_id", "entity_id"}

    def test_migration_0082_is_idempotent(self):
        from db.migrations import MIGRATIONS

        conn = _bare_facts_conn()
        migration_0082 = {migration.version: migration for migration in MIGRATIONS}[82]
        migration_0082.up(conn)
        migration_0082.up(conn)

        columns = {
            row[0]
            for row in conn.execute(
                """
                SELECT column_name
                FROM duckdb_columns()
                WHERE table_name = 'sec_company_facts'
                """
            ).fetchall()
        }
        assert "entity_id" in columns


def _bare_facts_conn():
    import duckdb

    conn = duckdb.connect(":memory:")
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
        CREATE TABLE sec_company_facts (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            label VARCHAR,
            description VARCHAR,
            unit VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            filed_date DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR,
            frame VARCHAR,
            value DOUBLE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_url VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    return conn


class TestSecurityAndEntityIdsForCiksAsof:
    """PIT-correct as-of CIK -> (security_id, entity_id) reader.

    Mirrors security_ids_for_symbols (priority UNION ALL / QUALIFY row_number)
    and security_entity_ids_asof (interval filter, available_at guard).
    """

    def test_resolves_current_cik_mapping(self, tmp_store):
        from db.security_master import security_and_entity_ids_for_ciks_asof

        _seed_spine(tmp_store)
        result = security_and_entity_ids_for_ciks_asof(
            tmp_store, ["0000320193"], as_of_ts=dt.datetime(2024, 1, 1, 12, 0, 0)
        )
        assert result == {"0000320193": ("SEC-CIK-0000320193", "CIK-0000320193")}

    def test_unknown_cik_absent_from_result(self, tmp_store):
        from db.security_master import security_and_entity_ids_for_ciks_asof

        _seed_spine(tmp_store)
        result = security_and_entity_ids_for_ciks_asof(
            tmp_store, ["0009999999"], as_of_ts=dt.datetime(2024, 1, 1, 12, 0, 0)
        )
        assert result == {}

    def test_mid_history_entity_change_no_lookahead(self, tmp_store):
        """A CIK whose entity_id changes mid-history (e.g. a resolved merger /
        re-parented entity) must resolve to the OLD entity_id for facts filed
        before the change becomes available, and the NEW entity_id after --
        never leaking a future identifier state into an earlier as-of read.
        """
        from db.security_master import security_and_entity_ids_for_ciks_asof

        security_id = "SEC-CIK-0000320193"
        cik = "0000320193"
        tmp_store.con.execute(
            """
            INSERT INTO securities (
                security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
                country, currency, active, first_seen_date, last_seen_date, source
            )
            VALUES (?, 'CIK-NEW-PARENT', ?, 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-01-01', NULL, 'fixture')
            """,
            [security_id, cik],
        )
        tmp_store.con.execute(
            """
            INSERT INTO security_identifier_history (
                security_id, id_type, id_value, valid_from, valid_to,
                as_of_date, available_at, source, run_id
            )
            VALUES
                (?, 'CIK', ?, DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL),
                (?, 'ENTITY_ID', 'CIK-OLD-PARENT', DATE '2019-01-01', DATE '2023-06-01',
                    DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL),
                (?, 'ENTITY_ID', 'CIK-NEW-PARENT', DATE '2023-06-01', NULL,
                    DATE '2023-06-01', TIMESTAMP '2023-06-01 15:00:00', 'fixture', NULL)
            """,
            [security_id, cik, security_id, security_id],
        )

        # Fact filed well before the re-parenting: must resolve to OLD entity.
        before = security_and_entity_ids_for_ciks_asof(
            tmp_store, [cik], as_of_ts=dt.datetime(2022, 1, 1, 9, 0, 0)
        )
        assert before == {cik: (security_id, "CIK-OLD-PARENT")}

        # Fact filed the same day the re-parenting event happened, but BEFORE
        # the warehouse actually knew about it (available_at not yet reached):
        # the OLD interval's valid_to boundary is same-day exclusive (matches
        # security_entity_ids_asof's existing interval semantics -- see
        # test_security_entity_ids_asof_merger_fixture_no_lookahead), and NEW
        # is not yet available -- so entity_id resolves to None rather than
        # leaking the future NEW value. Security_id itself still resolves (CIK
        # history has no such gap here). No lookahead either way.
        pre_availability = security_and_entity_ids_for_ciks_asof(
            tmp_store, [cik], as_of_ts=dt.datetime(2023, 6, 1, 9, 0, 0)
        )
        assert pre_availability == {cik: (security_id, None)}

        # After the re-parenting became available: must resolve to NEW entity.
        after = security_and_entity_ids_for_ciks_asof(
            tmp_store, [cik], as_of_ts=dt.datetime(2023, 6, 1, 16, 0, 0)
        )
        assert after == {cik: (security_id, "CIK-NEW-PARENT")}


class TestResolveCompanyFactsIdentifiers:
    """Enrichment step wired into the SEC companyfacts load path: resolves
    security_id/entity_id per-fact (own available_at) through the spine, and
    routes unresolved CIKs to the resolution ledger instead of dropping them.
    """

    def test_resolved_facts_get_spine_security_id_and_entity_id(self, tmp_store):
        from db.fundamentals import resolve_company_facts_identifiers

        _seed_spine(tmp_store)
        facts = pd.DataFrame(
            [
                {
                    "cik": "0000320193",
                    "security_id": "SEC-CIK-0000320193",
                    "available_at": pd.Timestamp("2024-02-15 22:00:00"),
                }
            ]
        )
        resolved, unresolved = resolve_company_facts_identifiers(tmp_store, facts)
        assert resolved.loc[0, "security_id"] == "SEC-CIK-0000320193"
        assert resolved.loc[0, "entity_id"] == "CIK-0000320193"
        assert unresolved.empty

    def test_unresolved_cik_lands_in_resolution_ledger_not_dropped(self, tmp_store):
        from db.fundamentals import resolve_company_facts_identifiers

        facts = pd.DataFrame(
            [
                {
                    "cik": "0009999999",
                    "security_id": "SEC-CIK-0009999999",
                    "available_at": pd.Timestamp("2024-02-15 22:00:00"),
                }
            ]
        )
        resolved, unresolved = resolve_company_facts_identifiers(tmp_store, facts)
        # The fact row is never dropped -- it keeps flowing with its best-effort
        # security_id (entity_id left null) -- and it is flagged as unresolved
        # for ledger routing by the caller.
        assert len(resolved) == 1
        assert resolved.loc[0, "security_id"] == "SEC-CIK-0009999999"
        assert pd.isna(resolved.loc[0, "entity_id"])
        assert list(unresolved["cik"]) == ["0009999999"]

    def test_null_available_at_never_resolves_through_todays_state(self, tmp_store):
        """A fact with NaT available_at must NEVER be resolved via now/today's
        identifier state -- that would be a lookahead violation relative to the
        fact's true (unknown) filing time. It must be routed to the unresolved
        ledger path instead, exactly like an unresolvable CIK, even though the
        CIK itself is perfectly resolvable as-of today.
        """
        from db.fundamentals import resolve_company_facts_identifiers

        _seed_spine(tmp_store)
        facts = pd.DataFrame(
            [
                {
                    "cik": "0000320193",
                    "security_id": "SEC-CIK-0000320193",
                    "available_at": pd.NaT,
                }
            ]
        )
        resolved, unresolved = resolve_company_facts_identifiers(tmp_store, facts)
        # The fact is never dropped -- it keeps flowing with its best-effort
        # passthrough security_id -- but it must NOT pick up today's spine
        # state: entity_id stays null and it is flagged for ledger routing.
        assert len(resolved) == 1
        assert resolved.loc[0, "security_id"] == "SEC-CIK-0000320193"
        assert pd.isna(resolved.loc[0, "entity_id"])
        assert list(unresolved["cik"]) == ["0000320193"]

    def test_full_load_writes_unresolved_candidate_to_ledger(self, tmp_store, monkeypatch):
        from db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions

        monkeypatch.setattr(
            "db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [("GHOST", "0009999999", "SEC-CIK-0009999999")],
        )

        def _boom(ua):
            raise AssertionError("network must not be used in this test")

        monkeypatch.setattr("db.fundamentals.sec_session", _boom)

        import json
        import zipfile

        zpath = None

        def _fake_fetcher(path):
            raise AssertionError("companyfacts_zip not used in this test")

        # Use the injectable zip fetcher with a tiny in-memory-equivalent fixture.
        payload = {
            "cik": 9999999,
            "entityName": "Ghost Co",
            "facts": {
                "us-gaap": {
                    "Assets": {
                        "label": "Assets",
                        "units": {
                            "USD": [
                                {
                                    "end": "2023-12-31",
                                    "val": 42.0,
                                    "accn": "0009999999-24-000001",
                                    "fy": 2023,
                                    "fp": "FY",
                                    "form": "10-K",
                                    "filed": "2024-02-15",
                                }
                            ]
                        },
                    }
                }
            },
        }

        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory() as tmpdir:
            zpath = Path(tmpdir) / "companyfacts.zip"
            with zipfile.ZipFile(zpath, "w") as zf:
                zf.writestr("CIK0009999999.json", json.dumps(payload))

            res = SecCompanyFactsDataset().run(
                tmp_store,
                SecCompanyFactsOptions(companyfacts_zip=zpath, concepts=("Assets",)),
            )

        assert res.rows_loaded >= 1

        candidates = tmp_store.con.execute(
            """
            SELECT source_key_type, source_key_value, candidate_status
            FROM identifier_resolution_candidates
            WHERE source_dataset_id = 'sec_company_facts'
            """
        ).fetchall()
        assert ("CIK", "0009999999", "proposed") in candidates


class TestStatementPointsCarrySecurityId:
    def test_statement_points_inherit_resolved_security_id_from_facts(self, tmp_store, monkeypatch):
        from db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions

        _seed_spine(tmp_store)
        monkeypatch.setattr(
            "db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [("AAPL", "0000320193", "SEC-CIK-0000320193")],
        )

        def _boom(ua):
            raise AssertionError("network must not be used in this test")

        monkeypatch.setattr("db.fundamentals.sec_session", _boom)

        import json
        import zipfile
        import tempfile
        from pathlib import Path

        payload = {
            "cik": 320193,
            "entityName": "Apple Inc.",
            "facts": {
                "us-gaap": {
                    "Assets": {
                        "label": "Assets",
                        "units": {
                            "USD": [
                                {
                                    "end": "2023-12-31",
                                    "val": 100.0,
                                    "accn": "0000320193-24-000001",
                                    "fy": 2023,
                                    "fp": "FY",
                                    "form": "10-K",
                                    "filed": "2024-02-15",
                                }
                            ]
                        },
                    }
                }
            },
        }

        with tempfile.TemporaryDirectory() as tmpdir:
            zpath = Path(tmpdir) / "companyfacts.zip"
            with zipfile.ZipFile(zpath, "w") as zf:
                zf.writestr("CIK0000320193.json", json.dumps(payload))

            SecCompanyFactsDataset().run(
                tmp_store,
                SecCompanyFactsOptions(companyfacts_zip=zpath, concepts=("Assets",)),
            )

        fact_row = tmp_store.con.execute(
            "SELECT security_id, entity_id FROM sec_company_facts WHERE cik = '0000320193'"
        ).fetchone()
        assert fact_row == ("SEC-CIK-0000320193", "CIK-0000320193")

        stmt_row = tmp_store.con.execute(
            "SELECT security_id FROM fundamental_statement_points WHERE cik = '0000320193' LIMIT 1"
        ).fetchone()
        assert stmt_row is not None
        assert stmt_row[0] == "SEC-CIK-0000320193"
