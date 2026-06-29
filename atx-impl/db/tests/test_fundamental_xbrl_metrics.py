"""Tests for S10a consolidated inline-XBRL metric extraction (`fundamental_xbrl_metric`).

The extractor pulls entity-level (non-segment) us-gaap facts out of the already-cached
`xbrl_filing_facts` and turns them into canonical metric rows that the ratio engine can
consume — unlocking liquidity/solvency ratios that the narrow companyfacts feed never
carried. The math-free normalize step is a pure transform tested here without DuckDB;
the DB refresh just feeds it the consolidated candidate facts.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from db.fundamental_xbrl_metrics import (
    CONCEPT_MAP,
    XBRL_METRIC_COLUMNS,
    normalize_xbrl_metric_rows,
)


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
