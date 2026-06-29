"""Tests for the reference-classifications layer (S1).

All tests are OFFLINE — no real network calls. The EntityClassificationDataset
uses an injected fake fetcher instead of the real SEC endpoint.

Run from atx-impl/: python -m pytest db/tests/test_reference_classifications.py
"""
from __future__ import annotations

import datetime as dt
from typing import Callable

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
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(2082) == "NoDur"

    def test_sic_7372_software_is_buseq(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(7372) == "BusEq"

    def test_sic_6022_state_banks_is_money(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(6022) == "Money"

    def test_sic_2834_pharma_is_hlth(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(2834) == "Hlth"

    def test_sic_4911_electric_utility_is_utils(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(4911) == "Utils"

    def test_sic_1311_crude_petro_is_enrgy(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(1311) == "Enrgy"

    def test_sic_9995_nonclassifiable_is_other(self):
        from db.reference_classifications import fama_french_12_for_sic
        assert fama_french_12_for_sic(9995) == "Other"


# ===========================================================================
# 2. Schema: seeding taxonomy tables
# ===========================================================================


class TestTaxonomySeeding:
    """Seeding loaders must populate taxonomy + taxonomy_node correctly."""

    def test_sic_taxonomy_seeds_taxonomy_row(self, tmp_store):
        from db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        row = tmp_store.con.execute(
            "SELECT code FROM taxonomy WHERE code = 'SIC'"
        ).fetchone()
        assert row is not None, "Expected a taxonomy row with code='SIC'"

    def test_sic_taxonomy_seeds_divisions_and_major_groups(self, tmp_store):
        from db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
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
        from db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions
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
        from db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions, SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        FamaFrenchTaxonomyDataset().run(tmp_store, FamaFrenchTaxonomyOptions())
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy_mapping"
        ).fetchone()[0]
        assert count > 0, "Expected taxonomy_mapping rows for SIC->FF12"

    def test_naics_taxonomy_seeds_20_sectors(self, tmp_store):
        from db.reference_classifications import NaicsTaxonomyDataset, NaicsTaxonomyOptions
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
        from db.reference_classifications import (
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
        from db.reference_classifications import SicTaxonomyDataset, SicTaxonomyOptions
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        SicTaxonomyDataset().run(tmp_store, SicTaxonomyOptions())
        count = tmp_store.con.execute(
            "SELECT count(*) FROM taxonomy WHERE code = 'SIC'"
        ).fetchone()[0]
        assert count == 1, "Re-seeding SIC must not duplicate taxonomy row"

    def test_seeding_ff12_twice_is_idempotent(self, tmp_store):
        from db.reference_classifications import FamaFrenchTaxonomyDataset, FamaFrenchTaxonomyOptions
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
    from db.reference_classifications import (
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
        fetcher = lambda c: _fake_submission(sic)  # noqa: E731
        EntityClassificationDataset().run(
            store,
            EntityClassificationOptions(fetcher=fetcher),
        )

    def test_asof_returns_row_within_validity(self, tmp_store):
        from db.asof import entity_classification_asof
        from db.warehouse import now_utc_naive

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
        from db.asof import entity_classification_asof
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
        from db.reference_classifications import EntityClassificationDataset, EntityClassificationOptions
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
