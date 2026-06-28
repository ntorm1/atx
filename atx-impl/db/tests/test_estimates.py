"""Tests for Sprint S2 — Estimates store.

Groups:
1. migration:       all 9 est_* tables exist; schema_migrations records version 4.
2. measure seed:    5 rows; re-run idempotent.
3. actuals mapping: concept→measure, revenue coalesce, available_at carry, idempotent re-run.
4. SUE arithmetic:  hand-verified expected/surprise/sue values; NULL when insufficient history;
                    available_at of surprise row = originally-reported actual's available_at;
                    later restatement of a prior period does NOT change an earlier surprise.
5. surprise vs consensus: consensus_mean + surprise_pct populated when est_consensus row exists.
6. injectable loaders: provider inserts rows; no provider → 0 rows.
7. asof PIT:        est_actual_asof hides future rows, returns latest revision;
                    est_surprise_asof respects available_at.
8. quality checks:  bad fiscal_period and NULL value trip the relevant checks.
"""
from __future__ import annotations

import datetime as dt
import math
from typing import Any

import pandas as pd
import pytest

from db.estimates import (
    EstimateActualsDataset,
    EstimateActualsOptions,
    EstimateConsensusDataset,
    EstimateConsensusOptions,
    EstimateGuidanceDataset,
    EstimateGuidanceOptions,
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    EstimateRecommendationDataset,
    EstimateRecommendationOptions,
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
)
from db.asof import est_actual_asof, est_surprise_asof
from db.quality import run_warehouse_quality_checks


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _seed_security(store, security_id: str = "sec_test_001") -> None:
    """Insert a minimal securities row so FK-like checks don't trip."""
    store.con.execute(
        """
        INSERT OR IGNORE INTO securities (security_id, primary_symbol, name, source)
        VALUES (?, ?, 'Test Co', 'test')
        """,
        [security_id, security_id.upper()],
    )


def _insert_company_fact(
    store,
    *,
    security_id: str,
    concept: str,
    fiscal_year: int,
    fiscal_period: str,
    period_end: str,
    value: float,
    accession_number: str,
    available_at: dt.datetime,
    filed_date: str | None = None,
    form: str = "10-Q",
    unit: str = "USD",
) -> None:
    """Insert a row into sec_company_facts for testing."""
    store.con.execute(
        """
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, label, description,
            unit, period_start, period_end, filed_date, fiscal_year, fiscal_period,
            form, accession_number, frame, value, available_at, run_id, source_url, source_loaded_at
        )
        VALUES (
            'test', ?, '0000000001', 'us-gaap', ?, '', '',
            ?, NULL, ?, ?, ?, ?,
            ?, ?, NULL, ?, ?, NULL, '', now()
        )
        """,
        [
            security_id, concept, unit, period_end,
            filed_date or period_end, fiscal_year, fiscal_period,
            form, accession_number, value, available_at,
        ],
    )


# ─────────────────────────────────────────────────────────────────────────────
# 1. Migration
# ─────────────────────────────────────────────────────────────────────────────

class TestMigration:
    def test_all_est_tables_exist(self, tmp_store):
        expected_tables = {
            "est_measure", "est_actual", "est_consensus", "est_detail",
            "est_broker", "est_analyst", "est_guidance", "est_recommendation", "est_surprise",
        }
        existing = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT table_name FROM duckdb_tables() WHERE schema_name='main'"
            ).fetchall()
        }
        assert expected_tables.issubset(existing), f"Missing: {expected_tables - existing}"

    def test_schema_migrations_version_4(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE CAST(version AS INTEGER) = 4"
        ).fetchall()
        assert len(rows) == 1, "Migration version 4 not recorded in schema_migrations"


# ─────────────────────────────────────────────────────────────────────────────
# 2. Measure seed
# ─────────────────────────────────────────────────────────────────────────────

class TestMeasureSeed:
    def test_five_rows_loaded(self, tmp_store):
        result = EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        assert result.rows_loaded == 5

    def test_idempotent_rerun(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        result2 = EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        count = tmp_store.con.execute("SELECT count(*) FROM est_measure").fetchone()[0]
        assert count == 5
        assert result2.rows_loaded == 5

    def test_revenue_has_two_concepts(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        row = tmp_store.con.execute(
            "SELECT us_gaap_concepts FROM est_measure WHERE measure_code='REVENUE'"
        ).fetchone()
        assert row is not None
        import json
        concepts = json.loads(row[0])
        assert "Revenues" in concepts
        assert "RevenueFromContractWithCustomerExcludingAssessedTax" in concepts


# ─────────────────────────────────────────────────────────────────────────────
# 3. Actuals mapping
# ─────────────────────────────────────────────────────────────────────────────

class TestActualsMapping:
    def _seed_actuals_fixture(self, store):
        """Seed sec_company_facts with:
        - Security A: EarningsPerShareDiluted (maps to EPS_DILUTED)
        - Security A: Revenues (maps to REVENUE)
        - Security B: RevenueFromContractWithCustomerExcludingAssessedTax only (maps to REVENUE)
        - Security A: Both Revenues AND fallback for same period (coalesce test)
        """
        available_at_1 = dt.datetime(2023, 2, 5, 10, 0, 0)
        available_at_2 = dt.datetime(2023, 2, 5, 11, 0, 0)

        # Security A, EPS_DILUTED
        _insert_company_fact(
            store,
            security_id="sec_A",
            concept="EarningsPerShareDiluted",
            fiscal_year=2022,
            fiscal_period="Q4",
            period_end="2022-12-31",
            value=2.50,
            accession_number="0001-EPS",
            available_at=available_at_1,
            form="10-K",
        )
        # Security A, REVENUE via Revenues
        _insert_company_fact(
            store,
            security_id="sec_A",
            concept="Revenues",
            fiscal_year=2022,
            fiscal_period="Q4",
            period_end="2022-12-31",
            value=100000.0,
            accession_number="0001-REV",
            available_at=available_at_1,
            form="10-K",
        )
        # Security B, REVENUE via fallback concept only
        _insert_company_fact(
            store,
            security_id="sec_B",
            concept="RevenueFromContractWithCustomerExcludingAssessedTax",
            fiscal_year=2022,
            fiscal_period="Q4",
            period_end="2022-12-31",
            value=50000.0,
            accession_number="0002-REV",
            available_at=available_at_2,
            form="10-K",
        )
        # Security A: same period/accession has BOTH Revenues and fallback — coalesce should pick Revenues
        _insert_company_fact(
            store,
            security_id="sec_A",
            concept="RevenueFromContractWithCustomerExcludingAssessedTax",
            fiscal_year=2022,
            fiscal_period="Q4",
            period_end="2022-12-31",
            value=99000.0,  # different value — must NOT appear
            accession_number="0001-REV",  # SAME accession as Revenues row
            available_at=available_at_1,
            form="10-K",
        )

    def test_concept_to_measure_mapping(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        self._seed_actuals_fixture(tmp_store)
        result = EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())
        assert result.rows_loaded > 0

        rows = tmp_store.con.execute(
            "SELECT security_id, measure_code, value, accession_number FROM est_actual ORDER BY 1,2,3"
        ).fetchall()
        row_dict = {(r[0], r[1]): r for r in rows}
        # Security A EPS_DILUTED mapped correctly
        assert ("sec_A", "EPS_DILUTED") in row_dict
        assert row_dict[("sec_A", "EPS_DILUTED")][2] == pytest.approx(2.50)

    def test_revenue_coalesce_prefers_revenues(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        self._seed_actuals_fixture(tmp_store)
        EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())

        # sec_A should have REVENUE = 100000 (from Revenues), NOT 99000 (from fallback)
        row = tmp_store.con.execute(
            "SELECT value FROM est_actual WHERE security_id='sec_A' AND measure_code='REVENUE'"
        ).fetchone()
        assert row is not None
        assert row[0] == pytest.approx(100000.0), f"Expected 100000 (Revenues), got {row[0]}"

    def test_revenue_fallback_concept_used_when_revenues_absent(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        self._seed_actuals_fixture(tmp_store)
        EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())

        # sec_B only has fallback concept; should still map to REVENUE
        row = tmp_store.con.execute(
            "SELECT value FROM est_actual WHERE security_id='sec_B' AND measure_code='REVENUE'"
        ).fetchone()
        assert row is not None
        assert row[0] == pytest.approx(50000.0)

    def test_available_at_carried_not_restamped(self, tmp_store):
        """available_at in est_actual must equal the source sec_company_facts.available_at."""
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        source_ts = dt.datetime(2023, 2, 5, 10, 0, 0)
        _insert_company_fact(
            tmp_store,
            security_id="sec_carry",
            concept="EarningsPerShareDiluted",
            fiscal_year=2022,
            fiscal_period="Q4",
            period_end="2022-12-31",
            value=1.23,
            accession_number="carry-acc-001",
            available_at=source_ts,
            form="10-K",
        )
        EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())
        row = tmp_store.con.execute(
            "SELECT available_at FROM est_actual WHERE security_id='sec_carry'"
        ).fetchone()
        assert row is not None
        carried_ts = row[0]
        if hasattr(carried_ts, "to_pydatetime"):
            carried_ts = carried_ts.to_pydatetime().replace(tzinfo=None)
        assert carried_ts == source_ts, f"available_at was restamped: {carried_ts} != {source_ts}"

    def test_idempotent_rerun(self, tmp_store):
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        self._seed_actuals_fixture(tmp_store)
        r1 = EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())
        r2 = EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())
        count_after = tmp_store.con.execute("SELECT count(*) FROM est_actual").fetchone()[0]
        assert count_after == r1.rows_loaded  # no new rows on re-run


# ─────────────────────────────────────────────────────────────────────────────
# 4. SUE arithmetic — hand-verified
# ─────────────────────────────────────────────────────────────────────────────
#
# Setup: security "sec_sue", measure EPS_DILUTED, quarterly series Q4 each year.
#
# Period     FY  FP  actual  prior(fy-1,Q4)  Δ_t     trailing_Δs_before_t   drift  sigma  expected  surprise  sue
# 2019-Q4    2019 Q4  1.00   none            —       []                     —      —      None      None      None
# 2020-Q4    2020 Q4  1.20   1.00            0.20    []   (no prior Δ yet)  —      —      None      None      None
# 2021-Q4    2021 Q4  1.50   1.20            0.30    [0.20]  (1 Δ; <4)      —      —      None      None      None
# 2022-Q4    2022 Q4  1.90   1.50            0.40    [0.20,0.30] (<4)       —      —      None      None      None
# 2023-Q4    2023 Q4  2.35   1.90            0.45    [0.20,0.30,0.40] (<4)  —      —      None      None      None
# 2024-Q4    2024 Q4  2.85   2.35            0.50    [0.20,0.30,0.40,0.45]  n=4    drift=0.3375  sigma=?  ...
#
# n=4 trailing Δs: [0.20, 0.30, 0.40, 0.45]
# drift   = (0.20 + 0.30 + 0.40 + 0.45) / 4 = 1.35 / 4 = 0.3375
# variance = [(0.20-0.3375)^2 + (0.30-0.3375)^2 + (0.40-0.3375)^2 + (0.45-0.3375)^2] / 3
#           = [0.018906 + 0.001406 + 0.003906 + 0.012656] / 3
#           = 0.036875 / 3 = 0.012291\overline{6}
# sigma   = sqrt(0.012291\overline{6}) ≈ 0.110868...
# expected = actual_prior + drift = 2.35 + 0.3375 = 2.6875
# surprise = actual_t - expected  = 2.85 - 2.6875 = 0.1625
# sue      = surprise / sigma     = 0.1625 / 0.110868... ≈ 1.46576...

_SUE_ACTUALS = [
    # (fy, fp, period_end, value, accession, available_at)
    (2019, "Q4", "2019-12-31", 1.00, "acc2019Q4", dt.datetime(2020, 2, 10, 9, 0, 0)),
    (2020, "Q4", "2020-12-31", 1.20, "acc2020Q4", dt.datetime(2021, 2, 10, 9, 0, 0)),
    (2021, "Q4", "2021-12-31", 1.50, "acc2021Q4", dt.datetime(2022, 2, 10, 9, 0, 0)),
    (2022, "Q4", "2022-12-31", 1.90, "acc2022Q4", dt.datetime(2023, 2, 10, 9, 0, 0)),
    (2023, "Q4", "2023-12-31", 2.35, "acc2023Q4", dt.datetime(2024, 2, 10, 9, 0, 0)),
    (2024, "Q4", "2024-12-31", 2.85, "acc2024Q4", dt.datetime(2025, 2, 10, 9, 0, 0)),
]

_SUE_SECURITY = "sec_sue_001"
_SUE_MEASURE = "EPS_DILUTED"


def _seed_sue_fixture(store) -> None:
    EstimateMeasureSeedDataset().run(store, EstimateMeasureSeedOptions())
    for (fy, fp, period_end, value, acc, avail) in _SUE_ACTUALS:
        _insert_company_fact(
            store,
            security_id=_SUE_SECURITY,
            concept="EarningsPerShareDiluted",
            fiscal_year=fy,
            fiscal_period=fp,
            period_end=period_end,
            value=value,
            accession_number=acc,
            available_at=avail,
            form="10-K",
        )
    EstimateActualsDataset().run(store, EstimateActualsOptions())


class TestSueArithmetic:
    def _get_surprise_row(self, store, fy: int, fp: str) -> dict[str, Any]:
        row = store.con.execute(
            """
            SELECT fiscal_year, fiscal_period, actual, expected, surprise, sue, available_at
            FROM est_surprise
            WHERE security_id = ? AND measure_code = ? AND fiscal_year = ? AND fiscal_period = ?
            """,
            [_SUE_SECURITY, _SUE_MEASURE, fy, fp],
        ).fetchone()
        assert row is not None, f"No surprise row for ({fy},{fp})"
        return dict(zip(["fiscal_year", "fiscal_period", "actual", "expected", "surprise", "sue", "available_at"], row))

    def test_sue_null_when_insufficient_history(self, tmp_store):
        _seed_sue_fixture(tmp_store)
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        # Periods 2020-2023 Q4 all have prior but fewer than 4 trailing Δs → sue=NULL
        for fy in (2020, 2021, 2022, 2023):
            r = self._get_surprise_row(tmp_store, fy, "Q4")
            assert r["sue"] is None, f"Expected NULL sue for FY{fy} Q4, got {r['sue']}"

    def test_sue_computed_correctly_at_2024_q4(self, tmp_store):
        """Hand-verified: 2024-Q4 has exactly 4 trailing Δs → sue ≈ 1.46576."""
        _seed_sue_fixture(tmp_store)
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        r = self._get_surprise_row(tmp_store, 2024, "Q4")

        # Arithmetic (see module docstring above for full derivation):
        trailing = [0.20, 0.30, 0.40, 0.45]
        n = len(trailing)
        drift = sum(trailing) / n  # 0.3375
        variance = sum((x - drift) ** 2 for x in trailing) / (n - 1)
        sigma = math.sqrt(variance)
        expected = 2.35 + drift    # 2.6875
        surprise = 2.85 - expected  # 0.1625
        sue = surprise / sigma      # ≈ 1.46576

        assert r["actual"] == pytest.approx(2.85, abs=1e-9)
        assert r["expected"] == pytest.approx(expected, abs=1e-6)
        assert r["surprise"] == pytest.approx(surprise, abs=1e-6)
        assert r["sue"] == pytest.approx(sue, abs=1e-4)

    def test_available_at_equals_originally_reported_actual(self, tmp_store):
        """available_at in est_surprise = the originally-filed actual's available_at."""
        _seed_sue_fixture(tmp_store)
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        # 2024-Q4 originally reported available_at
        expected_avail = dt.datetime(2025, 2, 10, 9, 0, 0)
        r = self._get_surprise_row(tmp_store, 2024, "Q4")
        avail = r["available_at"]
        if hasattr(avail, "to_pydatetime"):
            avail = avail.to_pydatetime().replace(tzinfo=None)
        assert avail == expected_avail, f"available_at mismatch: {avail} != {expected_avail}"

    def test_prior_period_restatement_does_not_change_earlier_surprise(self, tmp_store):
        """Adding a later restatement of 2023-Q4 must NOT change the 2024-Q4 surprise.

        The SUE series uses originally-reported actuals (earliest available_at).
        A restatement (higher available_at) of 2023-Q4 filed AFTER 2024-Q4 is filed
        should be ignored when computing 2024-Q4 SUE.
        """
        _seed_sue_fixture(tmp_store)
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        r_before = self._get_surprise_row(tmp_store, 2024, "Q4")

        # Now insert a restatement of 2023-Q4 with a LATER available_at and different value
        _insert_company_fact(
            tmp_store,
            security_id=_SUE_SECURITY,
            concept="EarningsPerShareDiluted",
            fiscal_year=2023,
            fiscal_period="Q4",
            period_end="2023-12-31",
            value=2.40,  # restated (was 2.35)
            accession_number="acc2023Q4-restated",
            available_at=dt.datetime(2025, 3, 1, 9, 0, 0),  # AFTER 2024-Q4 was filed
            form="10-K/A",
        )
        EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())

        # Re-run surprise
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        r_after = self._get_surprise_row(tmp_store, 2024, "Q4")

        # sue should not change — we still used originally-reported 2023-Q4 actual (2.35)
        assert r_after["sue"] == pytest.approx(r_before["sue"], abs=1e-6), (
            f"Restatement changed 2024-Q4 sue: {r_before['sue']} → {r_after['sue']}"
        )


# ─────────────────────────────────────────────────────────────────────────────
# 5. Surprise vs consensus
# ─────────────────────────────────────────────────────────────────────────────

class TestSurpriseVsConsensus:
    def test_consensus_mean_and_surprise_pct_populated(self, tmp_store):
        """When est_consensus has a row for (security_id, measure_code, period_end)
        with available_at <= actual.available_at, consensus_mean + surprise_pct should be set."""
        _seed_sue_fixture(tmp_store)

        # Insert a consensus row for 2024-Q4 (available before the actual filing)
        consensus_avail = dt.datetime(2025, 1, 15, 9, 0, 0)  # before actual's 2025-02-10
        tmp_store.con.execute(
            """
            INSERT INTO est_consensus (
                security_id, measure_code, fiscal_year, fiscal_period, period_end,
                consensus_date, mean, available_at, as_of_date, source
            )
            VALUES (?, 'EPS_DILUTED', 2024, 'Q4', '2024-12-31',
                    '2025-01-15', 2.70, ?, '2024-12-31', 'test')
            """,
            [_SUE_SECURITY, consensus_avail],
        )

        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        row = tmp_store.con.execute(
            """
            SELECT consensus_mean, surprise_pct
            FROM est_surprise
            WHERE security_id = ? AND measure_code = 'EPS_DILUTED'
              AND fiscal_year = 2024 AND fiscal_period = 'Q4'
            """,
            [_SUE_SECURITY],
        ).fetchone()
        assert row is not None
        consensus_mean, surprise_pct = row
        assert consensus_mean == pytest.approx(2.70, abs=1e-6)
        # surprise_pct = (2.85 - 2.70) / abs(2.70) ≈ 0.05556
        expected_pct = (2.85 - 2.70) / abs(2.70)
        assert surprise_pct == pytest.approx(expected_pct, abs=1e-4)


# ─────────────────────────────────────────────────────────────────────────────
# 6. Injectable loaders
# ─────────────────────────────────────────────────────────────────────────────

class TestInjectableLoaders:
    def test_consensus_no_provider_zero_rows(self, tmp_store):
        result = EstimateConsensusDataset().run(tmp_store, EstimateConsensusOptions(provider=None))
        assert result.rows_loaded == 0
        count = tmp_store.con.execute("SELECT count(*) FROM est_consensus").fetchone()[0]
        assert count == 0

    def test_consensus_with_provider_inserts_rows(self, tmp_store):
        def _provider():
            return [
                {
                    "security_id": "sec_X",
                    "measure_code": "EPS_DILUTED",
                    "fiscal_year": 2023,
                    "fiscal_period": "Q4",
                    "period_end": "2023-12-31",
                    "consensus_date": "2024-01-20",
                    "mean": 1.55,
                    "as_of_date": "2023-12-31",
                    "source": "test_provider",
                }
            ]

        result = EstimateConsensusDataset().run(
            tmp_store, EstimateConsensusOptions(provider=_provider)
        )
        assert result.rows_loaded == 1
        count = tmp_store.con.execute("SELECT count(*) FROM est_consensus").fetchone()[0]
        assert count == 1

    def test_consensus_stamps_available_at_when_not_provided(self, tmp_store):
        before = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)

        def _provider():
            return [
                {
                    "security_id": "sec_Y",
                    "measure_code": "EPS_DILUTED",
                    "fiscal_year": 2023,
                    "fiscal_period": "Q4",
                    "period_end": "2023-12-31",
                    "mean": 1.00,
                    "as_of_date": "2023-12-31",
                    "source": "test",
                }
            ]

        EstimateConsensusDataset().run(tmp_store, EstimateConsensusOptions(provider=_provider))
        after = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
        avail = tmp_store.con.execute(
            "SELECT available_at FROM est_consensus WHERE security_id='sec_Y'"
        ).fetchone()[0]
        if hasattr(avail, "to_pydatetime"):
            avail = avail.to_pydatetime().replace(tzinfo=None)
        assert before <= avail <= after

    def test_guidance_no_provider_zero_rows(self, tmp_store):
        result = EstimateGuidanceDataset().run(tmp_store, EstimateGuidanceOptions())
        assert result.rows_loaded == 0

    def test_guidance_with_provider_inserts_rows(self, tmp_store):
        def _fetch():
            return [{"raw": True}]

        def _parse(raw):
            return [
                {
                    "security_id": "sec_G",
                    "measure_code": "EPS_DILUTED",
                    "fiscal_year": 2023,
                    "fiscal_period": "Q4",
                    "period_end": "2023-12-31",
                    "low": 1.40,
                    "high": 1.60,
                    "mid": 1.50,
                    "basis": "GAAP",
                    "guidance_date": "2024-01-15",
                    "as_of_date": "2023-12-31",
                    "source": "test",
                }
            ]

        result = EstimateGuidanceDataset().run(
            tmp_store, EstimateGuidanceOptions(fetch=_fetch, parse=_parse)
        )
        assert result.rows_loaded == 1
        count = tmp_store.con.execute("SELECT count(*) FROM est_guidance").fetchone()[0]
        assert count == 1

    def test_recommendation_no_provider_zero_rows(self, tmp_store):
        result = EstimateRecommendationDataset().run(
            tmp_store, EstimateRecommendationOptions(provider=None)
        )
        assert result.rows_loaded == 0

    def test_recommendation_with_provider_inserts_rows(self, tmp_store):
        def _provider():
            return [
                {
                    "security_id": "sec_R",
                    "broker_id": "broker_001",
                    "analyst_id": "analyst_001",
                    "rating": "BUY",
                    "rating_standardized": "BUY",
                    "prior_rating": "HOLD",
                    "action": "UP",
                    "rating_date": "2024-01-20",
                    "as_of_date": "2024-01-20",
                    "source": "test",
                }
            ]

        result = EstimateRecommendationDataset().run(
            tmp_store, EstimateRecommendationOptions(provider=_provider)
        )
        assert result.rows_loaded == 1
        count = tmp_store.con.execute("SELECT count(*) FROM est_recommendation").fetchone()[0]
        assert count == 1


# ─────────────────────────────────────────────────────────────────────────────
# 7. Asof PIT
# ─────────────────────────────────────────────────────────────────────────────

class TestAsofPIT:
    def _seed_two_revisions(self, store) -> tuple[dt.datetime, dt.datetime]:
        """Insert two revisions of the same period for sec_pit / EPS_DILUTED.

        Rev1: available_at = 2024-02-10 09:00 (original filing)
        Rev2: available_at = 2024-03-15 09:00 (10-K/A restatement)
        """
        EstimateMeasureSeedDataset().run(store, EstimateMeasureSeedOptions())
        rev1 = dt.datetime(2024, 2, 10, 9, 0, 0)
        rev2 = dt.datetime(2024, 3, 15, 9, 0, 0)

        _insert_company_fact(
            store,
            security_id="sec_pit",
            concept="EarningsPerShareDiluted",
            fiscal_year=2023,
            fiscal_period="Q4",
            period_end="2023-12-31",
            value=1.50,
            accession_number="acc-rev1",
            available_at=rev1,
            form="10-K",
        )
        _insert_company_fact(
            store,
            security_id="sec_pit",
            concept="EarningsPerShareDiluted",
            fiscal_year=2023,
            fiscal_period="Q4",
            period_end="2023-12-31",
            value=1.55,  # restated
            accession_number="acc-rev2",
            available_at=rev2,
            form="10-K/A",
        )
        EstimateActualsDataset().run(store, EstimateActualsOptions())
        return rev1, rev2

    def test_asof_hides_future_rows(self, tmp_store):
        rev1, rev2 = self._seed_two_revisions(tmp_store)

        # as-of before rev1: no rows visible
        df = est_actual_asof(
            tmp_store,
            as_of_date=dt.date(2024, 1, 1),
            as_of_ts=dt.datetime(2024, 1, 1, 23, 59, 59),
        )
        pit_rows = df[df["security_id"] == "sec_pit"]
        assert len(pit_rows) == 0, "Row with future available_at should be hidden"

    def test_asof_returns_latest_revision_as_of(self, tmp_store):
        rev1, rev2 = self._seed_two_revisions(tmp_store)

        # as-of between rev1 and rev2: should see only rev1 (value=1.50)
        df = est_actual_asof(
            tmp_store,
            as_of_date=dt.date(2024, 3, 1),
            as_of_ts=dt.datetime(2024, 3, 1, 23, 59, 59),
        )
        pit_rows = df[df["security_id"] == "sec_pit"]
        assert len(pit_rows) == 1
        assert pit_rows.iloc[0]["value"] == pytest.approx(1.50)

        # as-of after rev2: should see rev2 (value=1.55) as the latest
        df2 = est_actual_asof(
            tmp_store,
            as_of_date=dt.date(2024, 4, 1),
            as_of_ts=dt.datetime(2024, 4, 1, 23, 59, 59),
        )
        pit_rows2 = df2[df2["security_id"] == "sec_pit"]
        assert len(pit_rows2) == 1
        assert pit_rows2.iloc[0]["value"] == pytest.approx(1.55)

    def test_asof_with_security_and_measure_filters(self, tmp_store):
        """Filtered as-of exercises row_number() OVER a JOIN to TWO registered
        DataFrames (sid + mc filters) — the exact pattern the S2 implementer
        claimed was bugged in DuckDB 1.5.1. This regression test proves the
        filtered path returns the correct columns/values (no scramble) and that
        the filter actually excludes other securities/measures.
        """
        self._seed_two_revisions(tmp_store)  # sec_pit / EPS_DILUTED, revs 1.50 & 1.55
        # Add a second security + a different measure so the filter has something to exclude.
        _insert_company_fact(
            tmp_store,
            security_id="sec_other",
            concept="Revenues",
            fiscal_year=2023,
            fiscal_period="Q4",
            period_end="2023-12-31",
            value=9999.0,
            accession_number="acc-other",
            available_at=dt.datetime(2024, 2, 1, 12, 0, 0),
            form="10-K",
        )
        EstimateActualsDataset().run(tmp_store, EstimateActualsOptions())

        df = est_actual_asof(
            tmp_store,
            as_of_date=dt.date(2024, 4, 1),
            as_of_ts=dt.datetime(2024, 4, 1, 23, 59, 59),
            security_ids=("sec_pit",),
            measure_codes=("EPS_DILUTED",),
        )
        assert len(df) == 1, "filter should return exactly the one matching series"
        row = df.iloc[0]
        assert row["security_id"] == "sec_pit"
        assert row["measure_code"] == "EPS_DILUTED"
        assert row["value"] == pytest.approx(1.55), "latest revision, columns not scrambled"
        # The excluded security/measure must not leak through the JOIN filter.
        assert "sec_other" not in set(df["security_id"])

    def test_est_surprise_asof_respects_available_at(self, tmp_store):
        _seed_sue_fixture(tmp_store)
        EstimateSurpriseDataset().run(tmp_store, EstimateSurpriseOptions(min_obs=4))

        # 2024-Q4 surprise available_at = 2025-02-10
        before_filing = dt.datetime(2025, 2, 9, 23, 59, 59)
        after_filing = dt.datetime(2025, 2, 10, 23, 59, 59)

        df_before = est_surprise_asof(
            tmp_store,
            as_of_date=dt.date(2025, 2, 9),
            as_of_ts=before_filing,
        )
        df_after = est_surprise_asof(
            tmp_store,
            as_of_date=dt.date(2025, 2, 10),
            as_of_ts=after_filing,
        )

        sue_rows_before = df_before[
            (df_before["security_id"] == _SUE_SECURITY)
            & (df_before["fiscal_year"] == 2024)
            & (df_before["fiscal_period"] == "Q4")
        ]
        sue_rows_after = df_after[
            (df_after["security_id"] == _SUE_SECURITY)
            & (df_after["fiscal_year"] == 2024)
            & (df_after["fiscal_period"] == "Q4")
        ]

        assert len(sue_rows_before) == 0, "2024-Q4 surprise should be hidden before filing date"
        assert len(sue_rows_after) == 1, "2024-Q4 surprise should be visible after filing date"


# ─────────────────────────────────────────────────────────────────────────────
# 8. Quality checks
# ─────────────────────────────────────────────────────────────────────────────

class TestQualityChecks:
    def test_invalid_fiscal_period_trips_check(self, tmp_store):
        """Inserting a row with fiscal_period='H1' (invalid) should fail the quality check."""
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        # Insert a bad row directly
        tmp_store.con.execute(
            """
            INSERT INTO est_actual (
                security_id, measure_code, fiscal_year, fiscal_period,
                period_end, value, as_of_date, available_at, source, accession_number
            )
            VALUES ('sec_bad', 'EPS_DILUTED', 2022, 'H1',
                    '2022-06-30', 1.00, '2022-06-30', now(), 'test', 'acc-bad')
            """
        )
        results = run_warehouse_quality_checks(tmp_store, record=False)
        check = next(
            (r for r in results if r.check_name == "est_actual_invalid_fiscal_period"), None
        )
        assert check is not None, "est_actual_invalid_fiscal_period check not found"
        assert check.status == "failed", f"Expected failed, got {check.status}"

    def test_null_value_trips_check(self, tmp_store):
        """Inserting a row with value=NULL should fail the est_actual_null_value check."""
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        tmp_store.con.execute(
            """
            INSERT INTO est_actual (
                security_id, measure_code, fiscal_year, fiscal_period,
                period_end, value, as_of_date, available_at, source, accession_number
            )
            VALUES ('sec_null', 'EPS_DILUTED', 2022, 'Q4',
                    '2022-12-31', NULL, '2022-12-31', now(), 'test', 'acc-null')
            """
        )
        results = run_warehouse_quality_checks(tmp_store, record=False)
        check = next(
            (r for r in results if r.check_name == "est_actual_null_value"), None
        )
        assert check is not None
        assert check.status == "failed", f"Expected failed for null value, got {check.status}"

    def test_clean_est_actual_passes_checks(self, tmp_store):
        """With POPULATED valid data, both checks should pass (not just on an
        empty table — that would only prove the check doesn't false-positive)."""
        EstimateMeasureSeedDataset().run(tmp_store, EstimateMeasureSeedOptions())
        # Populate real, valid actuals so the checks pass against actual data.
        for fp, pe, val, acc in [
            ("Q1", "2023-03-31", 1.00, "acc-q1"),
            ("Q2", "2023-06-30", 1.10, "acc-q2"),
            ("Q3", "2023-09-30", 1.20, "acc-q3"),
            ("FY", "2023-12-31", 4.50, "acc-fy"),
        ]:
            tmp_store.con.execute(
                """
                INSERT INTO est_actual (
                    security_id, measure_code, fiscal_year, fiscal_period,
                    period_end, value, as_of_date, available_at, source, accession_number
                )
                VALUES ('sec_clean', 'EPS_DILUTED', 2023, ?, ?, ?, ?, now(), 'test', ?)
                """,
                [fp, pe, val, pe, acc],
            )
        results = run_warehouse_quality_checks(tmp_store, record=False)
        fp_check = next(
            (r for r in results if r.check_name == "est_actual_invalid_fiscal_period"), None
        )
        null_check = next(
            (r for r in results if r.check_name == "est_actual_null_value"), None
        )
        assert fp_check is not None
        assert null_check is not None
        assert fp_check.status == "passed"
        assert null_check.status == "passed"
