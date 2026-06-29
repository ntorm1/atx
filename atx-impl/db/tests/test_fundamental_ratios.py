"""Tests for the derived ``fundamental_ratios`` dataset (S9a).

The ratio engine is split into a pure, DB-free transform (``compute_ratio_rows``)
that maps a wide per-(security, period) input frame to long ratio rows, plus a
thin DuckDB materializer (``refresh_fundamental_ratios`` / ``FundamentalRatiosDataset``)
that pivots the bitemporal TTM flows + instant balances into that wide frame and
writes the result. Tests cover the math, the bitemporal availability watermark,
the meaningfulness flags, idempotency, and the as-of reader.

No network: ratios derive purely from already-loaded warehouse tables.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from db.fundamental_ratios import (
    FundamentalRatiosDataset,
    FundamentalRatiosOptions,
    RATIO_DEFS,
    compute_ratio_rows,
    fundamental_ratios_asof,
    refresh_fundamental_ratios,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _wide_row(**overrides) -> dict:
    """One fully-populated wide input row; override fields per test."""
    base = {
        "security_id": "SEC-CIK-0000320193",
        "symbol": "AAPL",
        "cik": "320193",
        "upstream_source": "sec_companyfacts",
        "period_start": dt.date(2025, 3, 30),
        "period_end": dt.date(2026, 3, 28),
        "fiscal_year": 2026,
        "fiscal_period": "Q2",
        # flows (TTM)
        "rev": 400.0, "rev_av": _ts("2026-05-01"),
        "ni": 100.0, "ni_av": _ts("2026-05-02"),
        "oi": 120.0, "oi_av": _ts("2026-05-01"),
        "ocf": 130.0, "ocf_av": _ts("2026-05-01"),
        "capex": -30.0, "capex_av": _ts("2026-05-01"),
        "div": -15.0, "div_av": _ts("2026-05-01"),
        "repurch": -50.0, "repurch_av": _ts("2026-05-01"),
        # balances (instant @ period_end)
        "assets": 350.0, "assets_av": _ts("2026-05-03"),
        "liabilities": 290.0, "liabilities_av": _ts("2026-05-01"),
        "equity": 60.0, "equity_av": _ts("2026-05-01"),
        "shares": 15.0, "shares_av": _ts("2026-05-01"),
    }
    base.update(overrides)
    return base


def _by_code(frame: pd.DataFrame) -> dict[str, pd.Series]:
    return {row.ratio_code: row for row in frame.itertuples(index=False)}


class TestComputeRatioRows:
    def test_full_row_emits_all_ratio_codes(self):
        out = compute_ratio_rows(pd.DataFrame([_wide_row()]))
        codes = {d.code for d in RATIO_DEFS}
        assert set(out["ratio_code"]) == codes
        assert len(out) == len(RATIO_DEFS)

    @pytest.mark.parametrize(
        "code,expected",
        [
            ("net_profit_margin", 0.25),
            ("operating_margin", 0.30),
            ("return_on_assets", 100.0 / 350.0),
            ("return_on_equity", 100.0 / 60.0),
            ("assets_to_equity", 350.0 / 60.0),
            ("liabilities_to_assets", 290.0 / 350.0),
            ("liabilities_to_equity", 290.0 / 60.0),
            ("free_cash_flow", 100.0),
            ("fcf_margin", 0.25),
            ("operating_cash_flow_to_net_income", 1.3),
            ("capex_to_revenue", 0.075),
            ("dividend_payout_ratio", 0.15),
            ("buyback_to_net_income", 0.50),
            ("total_payout_ratio", 0.65),
            ("book_value_per_share", 4.0),
        ],
    )
    def test_ratio_values(self, code, expected):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows[code].value == pytest.approx(expected)

    def test_free_cash_flow_is_level_kind_in_currency(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        fcf = rows["free_cash_flow"]
        assert fcf.ratio_kind == "level"
        assert fcf.unit == "currency"
        assert fcf.numerator_value == 130.0  # ocf
        assert fcf.denominator_value == -30.0  # capex (negative)

    def test_available_at_is_max_of_inputs(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        # net_profit_margin gated by rev_av(05-01), ni_av(05-02) -> 05-02
        assert rows["net_profit_margin"].available_at == _ts("2026-05-02")
        # return_on_assets gated by ni_av(05-02), assets_av(05-03) -> 05-03
        assert rows["return_on_assets"].available_at == _ts("2026-05-03")

    def test_as_of_date_is_period_end(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows["net_profit_margin"].as_of_date == dt.date(2026, 3, 28)

    def test_missing_input_drops_only_dependent_ratios(self):
        row = _wide_row(rev=None, rev_av=None)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        # revenue-dependent ratios absent
        for code in ("net_profit_margin", "operating_margin", "fcf_margin", "capex_to_revenue"):
            assert code not in rows
        # non-revenue ratios still present
        assert rows["return_on_assets"].value == pytest.approx(100.0 / 350.0)
        assert rows["book_value_per_share"].value == pytest.approx(4.0)

    def test_negative_denominator_flags_not_meaningful(self):
        row = _wide_row(equity=-10.0)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        roe = rows["return_on_equity"]
        assert roe.value == pytest.approx(-10.0)
        assert roe.is_meaningful is False
        # assets>0 ratio stays meaningful
        assert rows["return_on_assets"].is_meaningful is True

    def test_zero_denominator_skips_ratio(self):
        row = _wide_row(ni=0.0)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        assert "dividend_payout_ratio" not in rows  # den = ni = 0
        assert "operating_cash_flow_to_net_income" not in rows
        # numerator-zero ratio still emitted
        assert rows["net_profit_margin"].value == pytest.approx(0.0)

    def test_deterministic_ratio_id(self):
        a = compute_ratio_rows(pd.DataFrame([_wide_row()]))
        b = compute_ratio_rows(pd.DataFrame([_wide_row()]))
        assert list(a["ratio_id"]) == list(b["ratio_id"])
        assert a["ratio_id"].is_unique

    def test_empty_input_returns_empty_frame(self):
        out = compute_ratio_rows(pd.DataFrame())
        assert out.empty
        assert "ratio_code" in out.columns


# --------------------------------------------------------------------------- #
# Integration: pivot real warehouse input tables -> ratios
# --------------------------------------------------------------------------- #

_TTM_DEFAULTS = {
    "ttm_revision_group_id": "g", "anchor_statement_point_id": "a",
    "cik": "320193", "statement_type": "income_statement",
    "statement_section": "profitability", "canonical_label": "lbl",
    "unit": "USD", "unit_type": "monetary", "as_of_date": dt.date(2026, 3, 28),
    "accession_number": "acc", "quarter_count": 4, "coverage_days": 365,
    "input_statement_point_ids_json": "[]", "input_accessions_json": "[]",
    "input_period_ends_json": "[]", "revision_sequence": 0, "revision_count": 1,
    "is_latest_revision": True, "is_value_changed": False,
    "calculation_method": "sum_4q",
}
_STMT_DEFAULTS = {
    "fact_revision_id": "fr", "revision_group_id": "rg",
    "statement_type": "balance_sheet", "statement_section": "assets",
    "canonical_label": "lbl", "taxonomy": "us-gaap", "concept": "Assets",
    "unit": "USD", "unit_type": "monetary", "period_type": "instant",
    "normal_balance": "debit", "as_of_date": dt.date(2026, 3, 28),
    "accession_number": "acc", "revision_sequence": 0, "revision_count": 1,
    "is_latest_revision": True, "is_value_changed": False,
    "source_url": "http://x",
}


def _insert_ttm(store, *, security_id, symbol, metric, value, end, av,
                start=dt.date(2025, 3, 30), source="sec_companyfacts"):
    cols = dict(_TTM_DEFAULTS)
    cols.update(
        ttm_point_id=f"{security_id}|{metric}|{end}", source=source,
        security_id=security_id, symbol=symbol, canonical_metric=metric,
        ttm_value=value, ttm_start_date=start, ttm_end_date=end,
        available_at=av, updated_at=av,
    )
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_ttm_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_stmt(store, *, security_id, symbol, metric, value, end, av,
                 source="sec_companyfacts"):
    cols = dict(_STMT_DEFAULTS)
    cols.update(
        statement_point_id=f"{security_id}|{metric}|{end}", source=source,
        security_id=security_id, symbol=symbol, cik="320193",
        canonical_metric=metric, value=value, period_end=end,
        available_at=av, updated_at=av,
    )
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_statement_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _seed_minimal(store):
    sid, sym, end = "SEC-CIK-0000320193", "AAPL", dt.date(2026, 3, 28)
    av = dt.datetime(2026, 5, 1, 22, 0)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="revenue", value=400.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="net_income", value=100.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="operating_cash_flow", value=130.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="capital_expenditures", value=-30.0, end=end, av=av)
    _insert_stmt(store, security_id=sid, symbol=sym, metric="assets", value=350.0, end=end, av=av)
    _insert_stmt(store, security_id=sid, symbol=sym, metric="stockholders_equity", value=60.0, end=end, av=av)
    return sid, sym, end


class TestRefreshIntegration:
    def test_materializes_ratios_from_warehouse_tables(self, tmp_store):
        _seed_minimal(tmp_store)
        rows = refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        assert rows > 0
        df = tmp_store.con.execute(
            "SELECT ratio_code, value FROM fundamental_ratios ORDER BY ratio_code"
        ).df()
        codes = dict(zip(df["ratio_code"], df["value"]))
        assert codes["net_profit_margin"] == pytest.approx(0.25)
        assert codes["return_on_equity"] == pytest.approx(100.0 / 60.0)
        assert codes["free_cash_flow"] == pytest.approx(100.0)

    def test_dataset_run_is_idempotent(self, tmp_store):
        _seed_minimal(tmp_store)
        ds = FundamentalRatiosDataset()
        r1 = ds.run(tmp_store, FundamentalRatiosOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM fundamental_ratios").fetchone()[0]
        r2 = ds.run(tmp_store, FundamentalRatiosOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM fundamental_ratios").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2  # no duplication on re-run

    def test_no_duplicate_natural_keys(self, tmp_store):
        _seed_minimal(tmp_store)
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        dupes = tmp_store.con.execute(
            """
            SELECT count(*) FROM (
                SELECT source, security_id, ratio_code, basis, period_end, count(*) n
                FROM fundamental_ratios GROUP BY 1,2,3,4,5 HAVING count(*) > 1
            )
            """
        ).fetchone()[0]
        assert dupes == 0


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _seed_minimal(tmp_store)
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        # before availability (input available_at = 2026-05-01): nothing visible
        early = fundamental_ratios_asof(dt.date(2026, 4, 1), store=tmp_store, symbols=["AAPL"])
        assert early.empty
        # after availability: ratios visible
        late = fundamental_ratios_asof(dt.date(2026, 6, 1), store=tmp_store, symbols=["AAPL"])
        assert not late.empty
        assert "net_profit_margin" in set(late["ratio_code"])
