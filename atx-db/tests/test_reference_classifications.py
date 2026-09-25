"""Tests for the reference-classifications layer (S1).

All tests are OFFLINE — no real network calls. The EntityClassificationDataset
uses an injected fake fetcher instead of the real SEC endpoint.

Run from atx-db/: python -m pytest db/tests/test_reference_classifications.py
"""
from __future__ import annotations

import datetime as dt

import pytest

# ---------------------------------------------------------------------------
# Helper: build a minimal fake SEC submission response
# ---------------------------------------------------------------------------

def _fake_submission(sic: int, sic_description: str = "Test industry") -> dict:
    return {"sic": str(sic), "sicDescription": sic_description}


# ===========================================================================
# 1. Pure function: fama_french_12_for_sic
# ===========================================================================


class TestFamaFrench12ForSic:
    """Canonical SIC-to-FF12 mappings must be exact (brief §loaders point 2)."""

    def test_sic_2082_beverages_is_nodur(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(2082) == "NoDur"

    def test_sic_7372_software_is_buseq(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(7372) == "BusEq"

    def test_sic_6022_state_banks_is_money(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(6022) == "Money"

    def test_sic_2834_pharma_is_hlth(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(2834) == "Hlth"

    def test_sic_4911_electric_utility_is_utils(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(4911) == "Utils"

    def test_sic_1311_crude_petro_is_enrgy(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(1311) == "Enrgy"

    def test_sic_9995_nonclassifiable_is_other(self):
        from atx_db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(9995) == "Other"


# ===========================================================================
# 2. Schema: seeding taxonomy tables
# ===========================================================================


class TestTaxonomySeeding:
    """Seeding loaders must populate taxonomy + taxonomy_node correctly."""

    def test_sic_taxonomy_seeds_taxonomy_row(self, tmp_store):
        from atx_db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        row = tmp_store.con.execute(
            "SELECT code FROM taxonomy WHERE code = 'SIC'"
        ).fetchone()
        assert row is not None, "Expected a taxonomy row with code='SIC'"

    def test_sic_taxonomy_seeds_divisions_and_major_groups(self, tmp_store):
        from atx_db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        # 10 divisions + 83 two-digit major groups
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy_node WHERE taxonomy_id = (SELECT taxonomy_id FROM taxonomy WHERE code = 'SIC')"
        ).fetchone()[0]
        # At minimum: 10 divisions + some major groups (>= 10 + 1)
        assert count >= 11, f"Expected >=11 SIC nodes, got {count}"
        # Must have a level-1 division node
        div_count = tmp_store.con.execute(
            """
            SELECT count(*) FROM taxonomy_node tn
            JOIN taxonomy t ON t.taxonomy_id = tn.taxonomy_id
            WHERE t.code = 'SIC' AND tn.level = 1
            """
        ).fetchone()[0]
        assert div_count == 10, f"Expected 10 SIC divisions, got {div_count}"
        # Must have level-2 major group nodes
        mg_count = tmp_store.con.execute(
            """
            SELECT count(*) FROM taxonomy_node tn
            JOIN taxonomy t ON t.taxonomy_id = tn.taxonomy_id
            WHERE t.code = 'SIC' AND tn.level = 2
            """
        ).fetchone()[0]
        assert mg_count >= 50, f"Expected >=50 SIC major groups, got {mg_count}"

    def test_fama_french_taxonomy_seeds_12_nodes(self, tmp_store):
        from atx_db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        count = tmp_store.con.execute(
            """
            SELECT count(*) FROM taxonomy_node tn
            JOIN taxonomy t ON t.taxonomy_id = tn.taxonomy_id
            WHERE t.code = 'FAMA_FRENCH_12'
            """
        ).fetchone()[0]
        assert count == 12, f"Expected 12 FF12 nodes, got {count}"

    def test_fama_french_taxonomy_seeds_mapping_rows(self, tmp_store):
        from atx_db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions, SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy_mapping"
        ).fetchone()[0]
        assert count > 0, "Expected taxonomy_mapping rows for SIC->FF12"

    def test_naics_taxonomy_seeds_20_sectors(self, tmp_store):
        from atx_db.reference_classifications import NaicsTaxonomyDataset, NaicsTaxonomyOptions
        NaicsTaxonomyDataset().run(tmp_store, NaicsTaxonomyOptions())
        count = tmp_store.con.execute(
            """
            SELECT count(*) FROM taxonomy_node tn
            JOIN taxonomy t ON t.taxonomy_id = tn.taxonomy_id
            WHERE t.code = 'NAICS_2022'
            """
        ).fetchone()[0]
        assert count == 20, f"Expected 20 NAICS sectors, got {count}"

    def test_all_four_taxonomies_seeded_gives_4_taxonomy_rows(self, tmp_store):
        from atx_db.reference_classifications import (
            SicTaxonomyDataset, SicTaxonomyOptions,
            FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions,
            NaicsTaxonomyDataset, NaicsTaxonomyOptions,
        )
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        NaicsTaxonomyDataset().run(tmp_store, NaicsTaxonomyOptions())
        count = tmp_store.con.execute("SELECT count(*) FROM taxonomy").fetchone()[0]
        # SIC + FAMA_FRENCH_12 + (optionally FAMA_FRENCH_48 if implemented) + NAICS_2022
        # Required: at minimum 3 rows
        assert count >= 3, f"Expected >=3 taxonomy rows, got {count}"

    def test_seeding_sic_twice_is_idempotent(self, tmp_store):
        from atx_db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy WHERE code = 'SIC'"
        ).fetchone()[0]
        assert count == 1, "Re-seeding SIC must not duplicate taxonomy row"

    def test_seeding_ff12_twice_is_idempotent(self, tmp_store):
        from atx_db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy WHERE code = 'FAMA_FRENCH_12'"
        ).fetchone()[0]
        assert count == 1, "Re-seeding FF12 must not duplicate taxonomy row"


# ===========================================================================
# 3. EntityClassificationDataset (offline with injected fetcher)
# ===========================================================================


def _seed_all_taxonomies(store):
    from atx_db.reference_classifications import (
        SicTaxonomyDataset, SicTaxonomyOptions,
        FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions,
        NaicsTaxonomyDataset, NaicsTaxonomyOptions,
    )
    SicTaxonomyDataset().run(store, SicTaxonomyOptions())
    FamaFrenchTaxonomyDataset().run(store, FamaFrenchTaxonomyOptions())
    NaicsTaxonomyDataset().run(store, NaicsTaxonomyOptions())


def _insert_security(store, security_id: str, cik: str, primary_symbol: str = "TEST") -> None:
    store.con.execute(
        """
        INSERT OR IGNORE INTO securities (security_id, primary_symbol, source)
        VALUES (?, ?, 'test')
        """,
        [security_id, primary_symbol],
    )
    store.con.execute(
        """
        INSERT INTO security_identifier_history
            (security_id, id_type, id_value, valid_from, as_of_date, source)
        VALUES (?, 'CIK', ?, DATE '2000-01-01', DATE '2000-01-01', 'test')
        """,
        [security_id, cik],
    )


class TestEntityClassificationDataset:
    """Offline tests for EntityClassificationDataset with fake fetcher."""

    def test_primary_sic_row_written(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        fetcher = lambda cik: _fake_submission(7372, "Prepackaged Software")  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher),
        )

        count = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification
            WHERE security_id = 'SEC-CIK-0000789019'
              AND is_primary = true
            """
        ).fetchone()[0]
        assert count == 1, f"Expected 1 primary SIC row, got {count}"

    def test_derived_ff12_row_written(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        fetcher = lambda cik: _fake_submission(7372, "Prepackaged Software")  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher),
        )

        # Derived FF12 row
        ff_count = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'FAMA_FRENCH_12'
              AND ec.is_primary = false
            """
        ).fetchone()[0]
        assert ff_count == 1, f"Expected 1 derived FF12 row, got {ff_count}"

    def test_derived_naics_row_written(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        fetcher = lambda cik: _fake_submission(7372, "Prepackaged Software")  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher),
        )

        naics_count = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'NAICS_2022'
              AND ec.is_primary = false
            """
        ).fetchone()[0]
        assert naics_count >= 1, f"Expected >=1 derived NAICS row, got {naics_count}"

    def test_four_digit_sic_leaf_node_auto_created(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        fetcher = lambda cik: _fake_submission(7372)  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher),
        )

        leaf = tmp_store.con.execute(
            """
            SELECT node_code FROM taxonomy_node tn
            JOIN taxonomy t ON t.taxonomy_id = tn.taxonomy_id
            WHERE t.code = 'SIC' AND tn.node_code = '7372' AND tn.level = 3
            """
        ).fetchone()
        assert leaf is not None, "Expected a level-3 leaf node for SIC 7372"

    def test_multiple_securities_classified(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")
        _insert_security(tmp_store, "SEC-CIK-0000320193", "320193", "AAPL")
        _insert_security(tmp_store, "SEC-CIK-0000019617", "19617", "JPM")

        responses = {
            "789019": _fake_submission(7372, "Software"),
            "320193": _fake_submission(3674, "Semiconductors"),
            "19617": _fake_submission(6022, "State Banks"),
        }
        fetcher = lambda cik: responses.get(str(int(cik)))  # noqa: E731

        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher),
        )

        count = tmp_store.con.execute(
            "SELECT count(DISTINCT security_id) FROM entity_classification WHERE is_primary = true"
        ).fetchone()[0]
        assert count == 3, f"Expected 3 classified securities, got {count}"

    def test_fetch_failure_skips_security_no_crash(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        def bad_fetcher(cik):
            raise RuntimeError("Network error")

        # Must not raise
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=bad_fetcher),
        )
        count = tmp_store.con.execute("SELECT count(*) FROM entity_classification").fetchone()[0]
        assert count == 0, "Fetch failure should produce 0 rows, not crash"


# ===========================================================================
# 4. entity_classification_asof PIT reader
# ===========================================================================


class TestEntityClassificationAsof:
    """PIT reader must respect valid_from/valid_to and available_at."""

    def _seed_and_classify(self, store, security_id, cik, sic):
        _seed_all_taxonomies(store)
        _insert_security(store, security_id, cik)
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        fetcher = lambda c: _fake_submission(sic)  # noqa: E731
        EntityClassificationDataset().run(
            store,
            EntityClassificationOptions(fetcher=fetcher),
        )

    def test_asof_returns_row_within_validity(self, tmp_store):
        from atx_db.asof import entity_classification_asof
        from atx_db.warehouse import now_utc_naive

        self._seed_and_classify(tmp_store, "SEC-CIK-0000789019", "789019", 7372)
        today = now_utc_naive().date()
        # Omit as_of_ts: the reader defaults to a UTC-naive end-of-day timestamp,
        # matching how the writer stores available_at (now_utc_naive, UTC). Passing
        # a local datetime.now() would compare local wall-clock against a UTC-naive
        # store and spuriously drop rows whenever local != UTC.
        result = entity_classification_asof(
            tmp_store,
            security_id="SEC-CIK-0000789019",
            taxonomy_code="SIC",
            as_of_date=today,
        )
        assert len(result) >= 1, "Expected >=1 SIC row for today"

    def test_asof_returns_nothing_before_valid_from(self, tmp_store):
        from atx_db.asof import entity_classification_asof
        self._seed_and_classify(tmp_store, "SEC-CIK-0000789019", "789019", 7372)
        # Use a date far in the past
        past = dt.date(1990, 1, 1)
        result = entity_classification_asof(
            tmp_store,
            security_id="SEC-CIK-0000789019",
            taxonomy_code="SIC",
            as_of_date=past,
            as_of_ts=dt.datetime(1990, 1, 1, 12, 0, 0),
        )
        assert len(result) == 0, f"Expected 0 rows before valid_from, got {len(result)}"


# ===========================================================================
# 5. Bitemporal: re-classifying closes old interval and opens new
# ===========================================================================


class TestBitemporalReclassification:
    """When a security's SIC changes, old interval must be closed."""

    def test_reclassify_closes_old_interval(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        # First classification: SIC 7372
        fetcher1 = lambda cik: _fake_submission(7372)  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher1),
        )

        # Second classification: SIC 7371 (Computer Programming)
        fetcher2 = lambda cik: _fake_submission(7371)  # noqa: E731
        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(fetcher=fetcher2),
        )

        # The old row should be closed (valid_to is not NULL)
        old_open = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'SIC'
              AND ec.node_code = '7372'
              AND ec.is_primary = true
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert old_open == 0, "Old SIC 7372 interval should be closed (valid_to set)"

        # New row should be open
        new_open = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'SIC'
              AND ec.node_code = '7371'
              AND ec.is_primary = true
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert new_open == 1, "New SIC 7371 interval should be open (valid_to NULL)"

    def test_same_sic_twice_does_not_create_two_open_intervals(self, tmp_store):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        fetcher = lambda cik: _fake_submission(7372)  # noqa: E731
        EntityClassificationDataset().run(tmp_store, EntityClassificationOptions(fetcher=fetcher))
        EntityClassificationDataset().run(tmp_store, EntityClassificationOptions(fetcher=fetcher))

        open_count = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'SIC'
              AND ec.is_primary = true
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert open_count == 1, f"Expected exactly 1 open SIC interval, got {open_count}"

    def test_reclassify_across_ff12_boundary_closes_old_derived_interval(self, tmp_store):
        """SIC 7372 (FF12 BusEq) -> SIC 2082 (FF12 NoDur): exactly one open FF12
        interval (NoDur) and the old BusEq interval must be closed."""
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        EntityClassificationDataset().run(
            tmp_store, EntityClassificationOptions(fetcher=lambda cik: _fake_submission(7372))
        )
        EntityClassificationDataset().run(
            tmp_store, EntityClassificationOptions(fetcher=lambda cik: _fake_submission(2082))
        )

        # Exactly one OPEN FF12 interval for this security
        open_ff = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'FAMA_FRENCH_12'
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert open_ff == 1, f"Expected exactly 1 open FF12 interval, got {open_ff}"

        # The single open FF12 row must be NoDur (the new industry)
        open_ff_code = tmp_store.con.execute(
            """
            SELECT ec.node_code FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'FAMA_FRENCH_12'
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert open_ff_code == "NoDur", f"Expected open FF12 = NoDur, got {open_ff_code}"

        # The old BusEq interval must be closed (valid_to set)
        busq_open = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'FAMA_FRENCH_12'
              AND ec.node_code = 'BusEq'
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert busq_open == 0, "Old FF12 BusEq interval should be closed (valid_to set)"

    def test_reclassify_across_naics_boundary_closes_old_derived_interval(self, tmp_store):
        """SIC 7372 (NAICS 54) -> SIC 2082 (NAICS 31-33): exactly one open NAICS
        interval and the old NAICS interval must be closed."""
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        EntityClassificationDataset().run(
            tmp_store, EntityClassificationOptions(fetcher=lambda cik: _fake_submission(7372))
        )
        EntityClassificationDataset().run(
            tmp_store, EntityClassificationOptions(fetcher=lambda cik: _fake_submission(2082))
        )

        open_naics = tmp_store.con.execute(
            """
            SELECT count(*) FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'NAICS_2022'
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert open_naics == 1, f"Expected exactly 1 open NAICS interval, got {open_naics}"

        # The single open NAICS row must be the new sector (31-33), not the old (54)
        open_naics_code = tmp_store.con.execute(
            """
            SELECT ec.node_code FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019'
              AND t.code = 'NAICS_2022'
              AND ec.valid_to IS NULL
            """
        ).fetchone()[0]
        assert open_naics_code == "31-33", f"Expected open NAICS = 31-33, got {open_naics_code}"


class TestOfflineSicCsvFetcher:
    """Offline CIK->SIC CSV injection so reference classification needs no network."""

    def test_make_csv_fetcher_handles_padded_and_int_cik(self, tmp_path):
        from atx_db.reference_classifications import _make_csv_fetcher
        csv = tmp_path / "sic.csv"
        csv.write_text("cik,sic\n0000789019,7372\n", encoding="utf-8")
        fetch = _make_csv_fetcher(csv)
        assert fetch("789019")["sic"] == "7372"
        assert fetch(789019)["sic"] == "7372"
        assert fetch("999999") is None

    def test_sic_file_populates_entity_classification_offline(self, tmp_store, tmp_path):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")

        csv = tmp_path / "cik_sic.csv"
        csv.write_text("cik,sic\n789019,7372\n", encoding="utf-8")

        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(sic_file=csv),
        )

        primary = tmp_store.con.execute(
            """
            SELECT node_code FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019' AND t.code = 'SIC' AND ec.is_primary
            """
        ).fetchone()
        assert primary is not None and primary[0] == "7372"

        ff = tmp_store.con.execute(
            """
            SELECT node_code FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            WHERE ec.security_id = 'SEC-CIK-0000789019' AND t.code = 'FAMA_FRENCH_12'
            """
        ).fetchone()
        assert ff is not None and ff[0] == "BusEq"


class TestSecBulkSubmissionsZipFetcher:
    """Offline SEC bulk submissions.zip -> CIK/SIC fetcher (no network)."""

    def _make_fixture_zip(self, tmp_path):
        import json
        import zipfile
        zip_path = tmp_path / "submissions.zip"
        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.writestr(
                "CIK0000789019.json",
                json.dumps({"cik": "789019", "sic": "7372", "sicDescription": "Prepackaged Software"}),
            )
            zf.writestr(
                "CIK0000019617.json",
                json.dumps({"cik": 19617, "sic": "6022", "sicDescription": "State Banks"}),
            )
        return zip_path

    def test_zip_fetcher_resolves_padded_cik(self, tmp_path):
        from atx_db.reference_classifications import _make_submissions_zip_fetcher
        fetch = _make_submissions_zip_fetcher(self._make_fixture_zip(tmp_path))
        assert fetch("789019")["sic"] == "7372"
        assert fetch(19617)["sic"] == "6022"
        assert fetch("999999") is None

    def test_submissions_zip_populates_entity_classification_offline(self, tmp_store, tmp_path):
        from atx_db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        _seed_all_taxonomies(tmp_store)
        _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")
        _insert_security(tmp_store, "SEC-CIK-0000019617", "19617", "JPM")

        EntityClassificationDataset().run(
            tmp_store,
            EntityClassificationOptions(submissions_zip=self._make_fixture_zip(tmp_path)),
        )

        rows = tmp_store.con.execute(
            """
            SELECT s.primary_symbol, ec.node_code
            FROM entity_classification ec
            JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
            JOIN securities s ON s.security_id = ec.security_id
            WHERE t.code = 'SIC' AND ec.is_primary
            ORDER BY s.primary_symbol
            """
        ).fetchall()
        assert rows == [("JPM", "6022"), ("MSFT", "7372")]
        source_file = tmp_store.con.execute(
            """
            SELECT sha256, byte_count, status
            FROM raw_source_files
            WHERE dataset_id = 'entity_classification'
            """
        ).fetchone()
        assert source_file is not None
        assert len(source_file[0]) == 64
        assert source_file[1] > 0
        assert source_file[2] == "cached"


# ===========================================================================
# P6: French Siccodes49 / Siccodes12 boundaries and the ladder snapshot stage
# ===========================================================================

# (SIC, FF49 per French Siccodes49 or None when listed under no industry, FF12 per Siccodes12)
_FRENCH_BOUNDARIES = [
    (100, "Agric", "NoDur"), (2046, "Food", "NoDur"), (2047, "Hshld", "NoDur"), (2048, "Agric", "NoDur"),
    (2049, None, "NoDur"), (2063, "Food", "NoDur"), (2064, "Soda", "NoDur"), (2080, "Beer", "NoDur"),
    (2081, None, "NoDur"), (2086, "Soda", "NoDur"), (2830, "Drugs", "Hlth"), (2832, None, "Hlth"),
    (2836, "Drugs", "Hlth"), (2821, "Chems", "Chems"), (2844, "Hshld", "Chems"), (1040, "Gold", "Other"),
    (1221, "Coal", "Enrgy"), (1311, "Oil", "Enrgy"), (3570, "Hardw", "BusEq"), (3580, "Mach", "Manuf"),
    (3622, "Chips", "Manuf"), (3660, "ElcEq", "BusEq"), (3661, "Chips", "BusEq"), (3674, "Chips", "BusEq"),
    (3693, "MedEq", "Hlth"), (3694, "Autos", "BusEq"), (3695, "Hardw", "BusEq"), (3811, "LabEq", "BusEq"),
    (3812, "Chips", "BusEq"), (3845, "MedEq", "Hlth"), (4213, "Trans", "Other"), (4220, "BusSv", "Other"),
    (4911, "Util", "Utils"), (4949, None, "Utils"), (4950, "Other", "Other"), (4991, "Other", "Other"),
    (5047, "Whlsl", "Shops"), (5065, "Whlsl", "Shops"), (5812, "Meals", "Shops"), (5912, "Rtail", "Shops"),
    (6022, "Banks", "Money"), (6199, "Banks", "Money"), (6200, "Fin", "Money"), (6411, "Insur", "Money"),
    (6412, None, "Money"), (6770, "Fin", "Money"), (6798, "Fin", "Money"), (7011, "Meals", "Other"),
    (7370, "Softw", "BusEq"), (7372, "Softw", "BusEq"), (7373, "Softw", "BusEq"), (7374, "BusSv", "BusEq"),
    (7375, "Softw", "BusEq"), (7379, "BusSv", "BusEq"), (8000, "Hlth", "Hlth"), (9995, None, "Other"),
]


@pytest.mark.parametrize(("sic", "ff49", "ff12"), _FRENCH_BOUNDARIES)
def test_french_boundary_sics_match_siccodes49_and_siccodes12(sic, ff49, ff12):
    from atx_db.reference_classifications import fama_french_12_for_sic, fama_french_49_for_sic
    assert fama_french_49_for_sic(sic) == ff49
    assert fama_french_12_for_sic(sic) == ff12


def test_entity_classification_stage_is_offline_receipt_dated_and_bitemporal(tmp_store, tmp_path):
    """Retained submissions.zip only (fake downloader must never run); every row is valid
    from the archive's receipt date; a differing older open SIC closes there; rerun no-op."""
    import hashlib
    import json
    import zipfile

    from atx_db.activation import ActivationOptions, stage_entity_classification
    from atx_db.reference_classifications import ensure_sic_leaf_node
    from atx_db.symbol_directory import SnapshotAfterCutoffError

    received = dt.datetime(2026, 9, 20, 0, 7, 35)
    archive = tmp_path / "submissions.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        for cik, sic in (("0000789019", "7372"), ("0000019617", "6022"), ("0000000555", "9995")):
            zf.writestr(f"CIK{cik}.json", json.dumps({"cik": cik, "sic": sic, "filings": {"recent": {}}}))
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    (tmp_path / "submissions.zip.receipt.json").write_text(
        json.dumps({"received_at": received.isoformat(), "sha256": sha}), encoding="utf-8")

    _seed_all_taxonomies(tmp_store)
    _insert_security(tmp_store, "SEC-CIK-0000789019", "789019", "MSFT")
    _insert_security(tmp_store, "SEC-CIK-0000000555", "555", "SHEL")
    _insert_security(tmp_store, "SEC-CIK-0000000777", "777", "GONE")  # no member in the archive
    # A delisted issuer reachable only as a Company Facts owner (not in the ticker map).
    tmp_store.con.execute("""
        INSERT INTO sec_company_facts (source, security_id, cik, taxonomy, concept, unit, filed_date, source_url)
        VALUES ('test', 'SEC-COMPANYFACTS-UNRESOLVED-CIK-0000019617', '19617', 'us-gaap', 'Assets', 'USD',
                DATE '2015-02-01', 'test')
    """)
    sic_tax, old_leaf = ensure_sic_leaf_node(tmp_store, 3674)
    tmp_store.con.execute("""
        INSERT INTO entity_classification (classification_id, security_id, taxonomy_id, node_id, node_code,
            is_primary, valid_from, as_of_date, available_at, source)
        VALUES ('legacy-1', 'SEC-CIK-0000789019', ?, ?, '3674', true, DATE '2026-01-02', DATE '2026-01-02',
                TIMESTAMP '2026-01-02 12:00:00', 'legacy')
    """, [sic_tax, old_leaf])

    def no_network(url, dest, *, user_agent):
        raise AssertionError(f"entity_classification must not download {url}")

    options = ActivationOptions(cache_dir=tmp_path, as_of_date=dt.date(2026, 9, 20), downloader=no_network)
    result = stage_entity_classification(tmp_store, options)

    detail = result.detail
    assert detail["classification_basis"] == "current_sic_snapshot" and detail["network_requests"] == 0
    assert detail["snapshot"]["receipt_basis"] == "cache_receipt"
    assert detail["sic_read"] == {"cik_member_missing": 1, "sic_read": 3}
    assert detail["ff49_unlisted_sic_ids"] == 1 and detail["superseded_same_day"] == 0
    assert detail["targets"]["outside_securities"] == 1  # the Company Facts-only (delisted) owner
    rows = tmp_store.con.execute("""
        SELECT ec.security_id, t.code, ec.node_code, ec.valid_from, ec.as_of_date, ec.available_at
        FROM entity_classification ec JOIN taxonomy t USING (taxonomy_id)
        WHERE ec.valid_to IS NULL ORDER BY 1, 2
    """).fetchall()
    assert {(r[0], r[1]): r[2] for r in rows} == {
        ("SEC-CIK-0000000555", "FAMA_FRENCH_12"): "Other", ("SEC-CIK-0000000555", "NAICS_2022"): "92",
        ("SEC-CIK-0000000555", "SIC"): "9995",
        ("SEC-CIK-0000789019", "FAMA_FRENCH_12"): "BusEq", ("SEC-CIK-0000789019", "FAMA_FRENCH_49"): "Softw",
        ("SEC-CIK-0000789019", "NAICS_2022"): "54", ("SEC-CIK-0000789019", "SIC"): "7372",
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000019617", "FAMA_FRENCH_12"): "Money",
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000019617", "FAMA_FRENCH_49"): "Banks",
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000019617", "NAICS_2022"): "52",
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000019617", "SIC"): "6022",
    }
    # Never backdated: every snapshot row is valid from (and known at) the receipt, no earlier.
    assert {(r[3], r[4], r[5]) for r in rows} == {(received.date(), received.date(), received)}
    assert tmp_store.con.execute(
        "SELECT valid_to FROM entity_classification WHERE classification_id = 'legacy-1'"
    ).fetchone() == (received.date(),)

    rerun = stage_entity_classification(tmp_store, options)
    assert rerun.rows == 0 and rerun.detail["intervals_closed"] == 0

    with pytest.raises(SnapshotAfterCutoffError):
        stage_entity_classification(tmp_store, ActivationOptions(
            cache_dir=tmp_path, as_of_date=dt.date(2026, 9, 19), downloader=no_network))
