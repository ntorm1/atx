"""Tests for consolidated filing-instance XBRL metric extraction (`fundamental_xbrl_metric`).

The extractor pulls entity-level (non-segment) us-gaap facts out of the already-cached
`xbrl_filing_facts` and turns them into canonical metric rows that the ratio engine can
consume — unlocking liquidity/solvency ratios that the narrow companyfacts feed never
carried. The math-free normalize step is a pure transform tested here without DuckDB;
the DB refresh just feeds it the consolidated candidate facts.
"""

from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.fundamental_statements import CONCEPT_MAP_SUPPORTED_TAXONOMIES, FUNDAMENTAL_STATEMENT_MAP_ROWS
from atx_db.fundamental_xbrl_metrics import (
    COMPANYFACTS_DURATION_CONCEPT_MAP,
    COMPANYFACTS_INSTANT_CONCEPT_MAP,
    CONCEPT_MAP,
    DURATION_CONCEPT_MAP,
    INSTANT_CONCEPT_MAP,
    XBRL_METRIC_COLUMNS,
    FundamentalXbrlMetricOptions,
    normalize_xbrl_metric_rows,
    refresh_fundamental_xbrl_metrics,
)

_DEFAULT_AVAILABLE_AT = pd.Timestamp("2024-11-01")


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _cand(concept, value, period_end, av, accession, **over):
    base = {
        "security_id": "SEC-CIK-0000320193",
        "symbol": "AAPL",
        "cik": "320193",
        "concept": concept,
        "taxonomy": "us-gaap",
        "unit": "USD",
        "value": value,
        "period_type": "instant",
        "period_start": None,
        "period_end": period_end,
        "accession_number": accession,
        "available_at": av,
        "fiscal_year": None,
        "fiscal_period": None,
    }
    base.update(over)
    return base


class TestNormalizeXbrlMetrics:
    def test_maps_known_concepts_and_drops_unknown(self):
        rows = [
            _cand("AssetsCurrent", 144.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),
            _cand("LiabilitiesCurrent", 134.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),
            _cand("SomethingUnmapped", 1.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),
        ]
        out = normalize_xbrl_metric_rows(pd.DataFrame(rows), source="x")
        canon = set(out["canonical_metric"])
        assert canon == {"current_assets", "current_liabilities"}
        assert list(XBRL_METRIC_COLUMNS) == list(out.columns)

    def test_canonical_values_and_bitemporal_fields(self):
        out = normalize_xbrl_metric_rows(
            pd.DataFrame([_cand("AssetsCurrent", 144.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1")]),
            source="x",
        )
        r = out.iloc[0]
        assert r["canonical_metric"] == "current_assets"
        assert r["value"] == 144.0
        assert r["period_end"] == dt.date(2026, 3, 28)
        assert r["as_of_date"] == dt.date(2026, 3, 28)
        assert r["available_at"] == _ts("2026-05-01")
        assert bool(r["is_latest_revision"]) is True

    def test_restatement_keeps_all_vintages_flags_latest(self):
        rows = [
            _cand("AssetsCurrent", 140.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),  # 10-Q
            _cand("AssetsCurrent", 144.0, dt.date(2026, 3, 28), _ts("2026-08-01"), "acc2"),  # later amend
        ]
        out = normalize_xbrl_metric_rows(pd.DataFrame(rows), source="x").sort_values("available_at")
        assert len(out) == 2
        latest = out[out["is_latest_revision"]]
        assert len(latest) == 1
        assert latest.iloc[0]["value"] == 144.0
        assert latest.iloc[0]["available_at"] == _ts("2026-08-01")

    def test_deterministic_metric_id_unique(self):
        rows = [
            _cand("AssetsCurrent", 144.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),
            _cand("LiabilitiesCurrent", 134.0, dt.date(2026, 3, 28), _ts("2026-05-01"), "acc1"),
        ]
        a = normalize_xbrl_metric_rows(pd.DataFrame(rows), source="x")
        b = normalize_xbrl_metric_rows(pd.DataFrame(rows), source="x")
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_input_returns_typed_empty(self):
        out = normalize_xbrl_metric_rows(pd.DataFrame(), source="x")
        assert out.empty
        assert list(out.columns) == list(XBRL_METRIC_COLUMNS)

    def test_concept_map_covers_liquidity_inputs(self):
        assert CONCEPT_MAP["AssetsCurrent"] == "current_assets"
        assert CONCEPT_MAP["LiabilitiesCurrent"] == "current_liabilities"
        assert CONCEPT_MAP["CashAndCashEquivalentsAtCarryingValue"] == "cash_and_equivalents"
        assert CONCEPT_MAP["InventoryNet"] == "inventory"

    def test_concept_map_covers_debt_input(self):
        assert CONCEPT_MAP["LongTermDebt"] == "long_term_debt"

    def test_concept_map_covers_retained_earnings(self):
        assert CONCEPT_MAP["RetainedEarningsAccumulatedDeficit"] == "retained_earnings"

    def test_concept_map_covers_legacy_temporary_equity_alternatives(self):
        assert CONCEPT_MAP["TemporaryEquityRedemptionValue"] == "temporary_equity"
        assert CONCEPT_MAP["RedeemableNoncontrollingInterestEquityCarryingAmount"] == "temporary_equity"
        assert CONCEPT_MAP["RedeemableNoncontrollingInterestEquityPreferredCarryingAmount"] == "temporary_equity"

    def test_concept_map_covers_period_end_shares(self):
        # period-end common shares outstanding (S10e Piotroski issuance signal)
        assert CONCEPT_MAP["CommonStockSharesOutstanding"] == "common_shares_outstanding"

    def test_normalizes_period_end_shares_as_instant(self):
        out = normalize_xbrl_metric_rows(
            pd.DataFrame(
                [_cand("CommonStockSharesOutstanding", 15.0e9, dt.date(2025, 9, 27), _ts("2025-10-31"), "acc1")]
            ),
            source="x",
        )
        r = out.iloc[0]
        assert r["canonical_metric"] == "common_shares_outstanding"
        assert r["period_type"] == "instant"
        assert r["value"] == 15.0e9

    def test_concept_map_covers_asset_structure_instants(self):
        # S10g asset-structure / activity inputs
        assert CONCEPT_MAP["PropertyPlantAndEquipmentNet"] == "property_plant_equipment_net"
        assert CONCEPT_MAP["AccountsReceivableNetCurrent"] == "accounts_receivable"

    def test_concept_map_covers_flow_inputs(self):
        assert CONCEPT_MAP["GrossProfit"] == "gross_profit"
        assert CONCEPT_MAP["CostOfGoodsAndServicesSold"] == "cost_of_revenue"
        assert CONCEPT_MAP["InterestExpense"] == "interest_expense"
        assert CONCEPT_MAP["DepreciationDepletionAndAmortization"] == "depreciation_amortization"
        assert CONCEPT_MAP["SellingGeneralAndAdministrativeExpense"] == "selling_general_and_administrative_expense"

    def test_duration_flow_preserves_period_start_and_type(self):
        cand = _cand(
            "GrossProfit",
            180.0,
            dt.date(2025, 9, 27),
            _ts("2025-10-31"),
            "acc1",
            period_type="duration",
            period_start=dt.date(2024, 9, 29),
        )
        out = normalize_xbrl_metric_rows(pd.DataFrame([cand]), source="x")
        r = out.iloc[0]
        assert r["canonical_metric"] == "gross_profit"
        assert r["period_type"] == "duration"
        assert r["period_start"] == dt.date(2024, 9, 29)
        assert r["period_end"] == dt.date(2025, 9, 27)

    def test_companyfacts_feed_carries_noncurrent_lt_debt(self):
        # S44: the broad companyfacts feed tags noncurrent LT debt; it maps to the
        # same canonical long_term_debt as the inline-XBRL total LongTermDebt.
        assert CONCEPT_MAP["LongTermDebtNoncurrent"] == "long_term_debt"

    def test_companyfacts_maps_cover_s3_active_period_concepts(self):
        supported = set(CONCEPT_MAP_SUPPORTED_TAXONOMIES)
        expected_instants = {
            row.concept
            for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
            if row.taxonomy in supported
            and row.period_type == "instant"
            and row.is_active
            and not row.is_derived
            and not row.concept.startswith("__")
        }
        expected_durations = {
            row.concept
            for row in FUNDAMENTAL_STATEMENT_MAP_ROWS
            if row.taxonomy in supported
            and row.period_type == "duration"
            and row.is_active
            and not row.is_derived
            and not row.concept.startswith("__")
        }

        assert expected_instants <= set(COMPANYFACTS_INSTANT_CONCEPT_MAP)
        assert expected_durations <= set(COMPANYFACTS_DURATION_CONCEPT_MAP)
        assert len(COMPANYFACTS_INSTANT_CONCEPT_MAP) > len(INSTANT_CONCEPT_MAP)
        assert len(COMPANYFACTS_DURATION_CONCEPT_MAP) > len(DURATION_CONCEPT_MAP)
        assert "AccountsPayableCurrent" in COMPANYFACTS_INSTANT_CONCEPT_MAP
        assert "ResearchAndDevelopmentExpense" in COMPANYFACTS_DURATION_CONCEPT_MAP
        assert "LongTermDebt" in INSTANT_CONCEPT_MAP
        assert "LongTermDebt" not in COMPANYFACTS_INSTANT_CONCEPT_MAP


def _insert_companyfacts(store, rows: list[dict]) -> None:
    frame = pd.DataFrame(rows)
    store.con.register("cf_seed", frame)
    store.con.execute("INSERT INTO sec_company_facts BY NAME SELECT * FROM cf_seed")
    store.con.unregister("cf_seed")


def _cf_row(
    concept,
    value,
    period_end,
    *,
    period_start=None,
    accession="acc-2024",
    security_id="SEC-CIK-0000320193",
    cik="320193",
    taxonomy="us-gaap",
    unit="USD",
    filed_date=dt.date(2024, 11, 1),
    available_at=_DEFAULT_AVAILABLE_AT,
):
    return {
        "source": "sec_companyfacts",
        "security_id": security_id,
        "cik": cik,
        "taxonomy": taxonomy,
        "concept": concept,
        "unit": unit,
        "period_start": period_start,
        "period_end": period_end,
        "filed_date": filed_date,
        "form": "10-K",
        "accession_number": accession,
        "value": value,
        "available_at": available_at,
        "source_url": "https://data.sec.gov/api/xbrl/companyfacts/CIK0000320193.json",
    }


def _insert_inline_instant_context(store) -> None:
    store.con.execute(
        """
        INSERT INTO xbrl_filing_contexts (
            filing_context_id,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            report_date,
            acceptance_datetime,
            primary_document,
            context_id,
            entity_identifier_scheme,
            entity_identifier,
            period_type,
            period_start,
            period_end,
            instant_date,
            has_segment,
            has_scenario,
            explicit_member_count,
            typed_member_count,
            dimension_count,
            context_hash,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            'ctx-inline-instant',
            'SEC-CIK-0000320193',
            '320193',
            'acc-inline',
            '10-K',
            DATE '2024-11-01',
            DATE '2024-09-28',
            TIMESTAMP '2024-11-01 18:00:00',
            'aapl-20240928.htm',
            'c-inline',
            'http://www.sec.gov/CIK',
            '0000320193',
            'instant',
            NULL,
            NULL,
            DATE '2024-09-28',
            false,
            false,
            0,
            0,
            0,
            'ctx-inline-instant-hash',
            'https://www.sec.gov/Archives/edgar/data/320193/aapl-20240928.htm',
            'test',
            TIMESTAMP '2024-11-01 18:05:00'
        )
        """
    )


def _insert_inline_fact(store, fact_id: str, ordinal: int, concept: str, value: float) -> None:
    store.con.execute(
        """
        INSERT INTO xbrl_filing_facts (
            filing_fact_id,
            filing_context_id,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            fact_ordinal,
            fact_kind,
            ix_id,
            qname,
            taxonomy,
            concept,
            context_ref,
            unit_ref,
            unit_measures_json,
            unit_numerator_measures_json,
            unit_denominator_measures_json,
            decimals,
            precision,
            scale,
            sign,
            format,
            continued_at,
            is_hidden,
            raw_value,
            normalized_value,
            numeric_value,
            is_numeric,
            source_line,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            ?,
            'ctx-inline-instant',
            'SEC-CIK-0000320193',
            '320193',
            'acc-inline',
            '10-K',
            DATE '2024-11-01',
            TIMESTAMP '2024-11-01 18:00:00',
            'aapl-20240928.htm',
            ?,
            'nonFraction',
            NULL,
            'us-gaap:' || ?,
            'us-gaap',
            ?,
            'c-inline',
            'usd',
            '["iso4217:USD"]',
            '[]',
            '[]',
            '0',
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            false,
            CAST(? AS VARCHAR),
            CAST(? AS VARCHAR),
            ?,
            true,
            ?,
            'https://www.sec.gov/Archives/edgar/data/320193/aapl-20240928.htm',
            'test',
            TIMESTAMP '2024-11-01 18:05:00'
        )
        """,
        [fact_id, ordinal, concept, concept, value, value, value, ordinal],
    )


class TestCompanyFactsCandidatePath:
    """S44: refresh_fundamental_xbrl_metrics also sources the broad companyfacts feed."""

    def _seed_security(self, store, security_id="SEC-CIK-0000320193", symbol="AAPL"):
        store.con.execute(
            "INSERT INTO securities (security_id, primary_symbol, source) VALUES (?, ?, ?)",
            [security_id, symbol, "test"],
        )

    def test_companyfacts_rows_become_canonical_metrics(self, tmp_store):
        store = tmp_store
        self._seed_security(store)
        _insert_companyfacts(
            store,
            [
                _cf_row("AssetsCurrent", 152_987e6, dt.date(2024, 9, 28)),
                _cf_row("LiabilitiesCurrent", 176_392e6, dt.date(2024, 9, 28)),
                _cf_row("LongTermDebtNoncurrent", 85_750e6, dt.date(2024, 9, 28)),
                _cf_row("GrossProfit", 180_683e6, dt.date(2024, 9, 28), period_start=dt.date(2023, 10, 1)),
            ],
        )
        n = refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_cf"))
        assert n == 4
        got = store.con.execute(
            "SELECT canonical_metric, period_type, value, symbol FROM fundamental_xbrl_metric WHERE source = 'test_cf'"
        ).df()
        metrics = set(got["canonical_metric"])
        assert {"current_assets", "current_liabilities", "long_term_debt", "gross_profit"} <= metrics
        assert set(got["symbol"]) == {"AAPL"}  # resolved via securities join

    def test_companyfacts_widened_s3_concepts_emit_across_securities(self, tmp_store):
        store = tmp_store
        self._seed_security(store, "SEC-CIK-0000320193", "AAPL")
        self._seed_security(store, "SEC-CIK-0000789019", "MSFT")
        annual_start = dt.date(2023, 10, 1)
        period_end = dt.date(2024, 9, 28)
        _insert_companyfacts(
            store,
            [
                _cf_row("AccountsPayableCurrent", 68_960e6, period_end),
                _cf_row("ResearchAndDevelopmentExpense", 31_370e6, period_end, period_start=annual_start),
                _cf_row(
                    "DeferredRevenue",
                    45_538e6,
                    period_end,
                    security_id="SEC-CIK-0000789019",
                    cik="789019",
                    accession="msft-bs",
                ),
                _cf_row(
                    "PaymentsToAcquireBusinessesNetOfCashAcquired",
                    5_000e6,
                    period_end,
                    period_start=annual_start,
                    security_id="SEC-CIK-0000789019",
                    cik="789019",
                    accession="msft-cf",
                ),
                _cf_row(
                    "EntityCommonStockSharesOutstanding",
                    7_430e6,
                    period_end,
                    security_id="SEC-CIK-0000789019",
                    cik="789019",
                    taxonomy="dei",
                    unit="shares",
                    accession="msft-dei",
                ),
            ],
        )

        n = refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_cf"))

        assert n == 5
        got = store.con.execute(
            """
            SELECT security_id, symbol, concept, canonical_metric, period_type, period_start, available_at
            FROM fundamental_xbrl_metric
            WHERE source = 'test_cf'
            ORDER BY security_id, canonical_metric
            """
        ).df()
        assert set(got["security_id"]) == {"SEC-CIK-0000320193", "SEC-CIK-0000789019"}
        assert set(got["symbol"]) == {"AAPL", "MSFT"}
        assert {
            ("AccountsPayableCurrent", "ap", "instant"),
            ("ResearchAndDevelopmentExpense", "rd_expense", "duration"),
            ("DeferredRevenue", "deferred_revenue", "instant"),
            ("PaymentsToAcquireBusinessesNetOfCashAcquired", "acquisitions", "duration"),
            ("EntityCommonStockSharesOutstanding", "shares_outstanding", "instant"),
        } <= set(zip(got["concept"], got["canonical_metric"], got["period_type"], strict=True))
        assert got.loc[got["canonical_metric"] == "rd_expense", "period_start"].iloc[0] == pd.Timestamp(annual_start)
        assert got["available_at"].min() == pd.Timestamp("2024-11-01")

    def test_symbol_scoped_refresh_preserves_other_securities(self, tmp_store):
        store = tmp_store
        self._seed_security(store, "SEC-CIK-0000320193", "AAPL")
        self._seed_security(store, "SEC-CIK-0000789019", "MSFT")
        _insert_companyfacts(
            store,
            [
                _cf_row("AssetsCurrent", 152_987e6, dt.date(2024, 9, 28)),
                _cf_row(
                    "AssetsCurrent",
                    159_734e6,
                    dt.date(2024, 9, 28),
                    security_id="SEC-CIK-0000789019",
                    cik="789019",
                    accession="msft-bs",
                ),
            ],
        )
        refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_scoped"))

        rows_loaded = refresh_fundamental_xbrl_metrics(
            store,
            FundamentalXbrlMetricOptions(source="test_scoped", symbols=("aapl",)),
        )

        assert rows_loaded == 1
        got = store.con.execute(
            """
            SELECT security_id, count(*) AS row_count
            FROM fundamental_xbrl_metric
            WHERE source = 'test_scoped'
            GROUP BY security_id
            ORDER BY security_id
            """
        ).fetchall()
        assert got == [("SEC-CIK-0000320193", 1), ("SEC-CIK-0000789019", 1)]

    def test_filing_instance_path_uses_full_governed_concept_map(self, tmp_store):
        store = tmp_store
        self._seed_security(store)
        _insert_inline_instant_context(store)
        _insert_inline_fact(store, "fact-assets-current", 1, "AssetsCurrent", 152_987e6)
        _insert_inline_fact(store, "fact-ap-current", 2, "AccountsPayableCurrent", 68_960e6)

        n = refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_inline"))

        assert n == 2
        got = store.con.execute(
            "SELECT concept, canonical_metric FROM fundamental_xbrl_metric WHERE source = 'test_inline'"
        ).fetchall()
        assert set(got) == {
            ("AccountsPayableCurrent", "ap"),
            ("AssetsCurrent", "current_assets"),
        }

    def test_inline_and_companyfacts_overlap_dedupes_one_metric_row(self, tmp_store):
        store = tmp_store
        self._seed_security(store)
        _insert_inline_instant_context(store)
        _insert_inline_fact(store, "fact-assets-current", 1, "AssetsCurrent", 152_987e6)
        _insert_companyfacts(
            store,
            [
                _cf_row(
                    "AssetsCurrent",
                    152_987e6,
                    dt.date(2024, 9, 28),
                    accession="acc-inline",
                    available_at=pd.Timestamp("2024-11-01 18:00:00"),
                ),
            ],
        )

        refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_overlap"))

        got = store.con.execute(
            """
            SELECT concept, canonical_metric, accession_number, count(*) AS n
            FROM fundamental_xbrl_metric
            WHERE source = 'test_overlap'
            GROUP BY concept, canonical_metric, accession_number
            """
        ).fetchall()
        assert got == [("AssetsCurrent", "current_assets", "acc-inline", 1)]

    def test_non_annual_duration_excluded(self, tmp_store):
        store = tmp_store
        self._seed_security(store)
        _insert_companyfacts(
            store,
            [
                # annual window (363 days) — kept
                _cf_row(
                    "GrossProfit", 180_683e6, dt.date(2024, 9, 28), period_start=dt.date(2023, 10, 1), accession="fy"
                ),
                # quarterly window (~91 days) — dropped (avoids Q-vs-FY ambiguity)
                _cf_row(
                    "GrossProfit", 43_000e6, dt.date(2024, 9, 28), period_start=dt.date(2024, 6, 29), accession="q"
                ),
            ],
        )
        refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_cf"))
        gp = store.con.execute(
            "SELECT value FROM fundamental_xbrl_metric WHERE source = 'test_cf' AND canonical_metric = 'gross_profit'"
        ).df()
        assert len(gp) == 1
        assert gp.iloc[0]["value"] == 180_683e6

    def test_instant_concept_in_duration_window_not_double_counted(self, tmp_store):
        # An instant balance fact (period_start NULL) must produce exactly one instant
        # metric row, never leak into the duration branch.
        store = tmp_store
        self._seed_security(store)
        _insert_companyfacts(
            store,
            [
                _cf_row("InventoryNet", 7_286e6, dt.date(2024, 9, 28)),
            ],
        )
        refresh_fundamental_xbrl_metrics(store, FundamentalXbrlMetricOptions(source="test_cf"))
        rows = store.con.execute(
            "SELECT period_type FROM fundamental_xbrl_metric "
            "WHERE source = 'test_cf' AND canonical_metric = 'inventory'"
        ).df()
        assert list(rows["period_type"]) == ["instant"]
