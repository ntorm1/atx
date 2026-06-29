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
    _attach_prior_year,
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
        # prior-year (YoY) inputs for growth + average-balance ratios (S9c/S9d)
        "rev_prior": 320.0, "rev_prior_av": _ts("2025-05-01"),
        "ni_prior": 80.0, "ni_prior_av": _ts("2025-05-02"),
        "oi_prior": 100.0, "oi_prior_av": _ts("2025-05-01"),
        "ocf_prior": 104.0, "ocf_prior_av": _ts("2025-05-01"),
        "assets_prior": 300.0, "assets_prior_av": _ts("2025-05-03"),
        "equity_prior": 50.0, "equity_prior_av": _ts("2025-05-01"),
        # consolidated inline-XBRL instant metrics for liquidity ratios (S10a)
        "current_assets": 200.0, "current_assets_av": _ts("2026-05-01"),
        "current_liabilities": 100.0, "current_liabilities_av": _ts("2026-05-01"),
        "cash_and_equivalents": 40.0, "cash_and_equivalents_av": _ts("2026-05-01"),
        "inventory": 30.0, "inventory_av": _ts("2026-05-01"),
        "long_term_debt": 90.0, "long_term_debt_av": _ts("2026-05-01"),
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
            # S9b: efficiency / cash-flow-coverage / reinvestment ratios
            ("asset_turnover", 400.0 / 350.0),
            ("equity_turnover", 400.0 / 60.0),
            ("operating_return_on_assets", 120.0 / 350.0),
            ("operating_cash_flow_margin", 130.0 / 400.0),
            ("operating_cash_flow_to_assets", 130.0 / 350.0),
            ("operating_cash_flow_to_liabilities", 130.0 / 290.0),
            ("capex_to_operating_cash_flow", 30.0 / 130.0),
            ("retention_ratio", (100.0 - 15.0) / 100.0),
            # S9c: year-over-year growth ratios
            ("revenue_growth_yoy", (400.0 - 320.0) / 320.0),
            ("net_income_growth_yoy", (100.0 - 80.0) / 80.0),
            ("operating_income_growth_yoy", (120.0 - 100.0) / 100.0),
            ("operating_cash_flow_growth_yoy", (130.0 - 104.0) / 104.0),
            ("assets_growth_yoy", (350.0 - 300.0) / 300.0),
            # S9d: average-balance returns + book-equity growth
            ("average_return_on_assets", 100.0 / ((350.0 + 300.0) / 2)),
            ("average_return_on_equity", 100.0 / ((60.0 + 50.0) / 2)),
            ("operating_cash_flow_to_average_assets", 130.0 / ((350.0 + 300.0) / 2)),
            ("equity_growth_yoy", (60.0 - 50.0) / 50.0),
            # S10a: liquidity / working-capital ratios (consolidated XBRL instants)
            ("current_ratio", 200.0 / 100.0),
            ("quick_ratio", (200.0 - 30.0) / 100.0),
            ("cash_ratio", 40.0 / 100.0),
            ("working_capital", 100.0),
            ("working_capital_to_assets", (200.0 - 100.0) / 350.0),
            # S10b: debt / solvency ratios (long-term debt vs equity/assets, net of cash)
            ("long_term_debt_to_equity", 90.0 / 60.0),
            ("long_term_debt_to_assets", 90.0 / 350.0),
            ("net_debt", 90.0 - 40.0),
            ("net_debt_to_assets", (90.0 - 40.0) / 350.0),
        ],
    )
    def test_ratio_values(self, code, expected):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows[code].value == pytest.approx(expected)

    def test_efficiency_ratios_have_efficiency_category(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows["asset_turnover"].ratio_category == "efficiency"
        assert rows["equity_turnover"].ratio_category == "efficiency"
        # equity_turnover denominator is equity -> not meaningful when negative
        neg = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(equity=-10.0)])))
        assert neg["equity_turnover"].is_meaningful is False

    def test_growth_ratio_kind_and_lineage(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        g = rows["revenue_growth_yoy"]
        assert g.ratio_kind == "growth"
        assert g.ratio_category == "growth"
        assert g.numerator_value == 400.0   # current TTM
        assert g.denominator_value == 320.0  # prior-year TTM
        # available_at = max(current_av 2026-05-01, prior_av 2025-05-01)
        assert g.available_at == _ts("2026-05-01")

    def test_growth_dropped_when_prior_missing(self):
        row = _wide_row(rev_prior=None, rev_prior_av=None)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        assert "revenue_growth_yoy" not in rows
        # other growth ratios with priors present still emit
        assert rows["net_income_growth_yoy"].value == pytest.approx(0.25)

    def test_growth_negative_prior_not_meaningful(self):
        row = _wide_row(ni_prior=-40.0)  # swung from loss to profit
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        g = rows["net_income_growth_yoy"]
        assert g.is_meaningful is False  # sign of % change is ambiguous off a negative base

    def test_quick_ratio_treats_missing_inventory_as_zero(self):
        # firms that report no InventoryNet (software/aerospace) -> quick == current
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(inventory=None, inventory_av=None)])))
        assert rows["quick_ratio"].value == pytest.approx(200.0 / 100.0)
        assert rows["current_ratio"].value == pytest.approx(200.0 / 100.0)

    def test_liquidity_dropped_without_current_liabilities(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(current_liabilities=None, current_liabilities_av=None)])))
        for code in ("current_ratio", "quick_ratio", "cash_ratio", "working_capital"):
            assert code not in rows

    def test_average_balance_returns_need_both_balances(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        # denominator is the mean of ending and prior-year balance
        assert rows["average_return_on_equity"].denominator_value == pytest.approx(55.0)
        # drop when the prior-year balance is unavailable
        gone = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(assets_prior=None, assets_prior_av=None)])))
        assert "average_return_on_assets" not in gone
        assert "operating_cash_flow_to_average_assets" not in gone

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


class TestAttachPriorYear:
    def _base(self, period_end, rev, av):
        return {
            "security_id": "S1", "symbol": "AAPL", "period_end": period_end,
            "rev": rev, "rev_av": av,
            "ni": rev / 4, "ni_av": av, "oi": rev / 3, "oi_av": av,
            "ocf": rev / 3, "ocf_av": av, "assets": rev * 2, "assets_av": av,
        }

    def test_pairs_row_to_prior_year(self):
        cur = self._base(dt.date(2026, 3, 28), 400.0, _ts("2026-05-01"))
        prior = self._base(dt.date(2025, 3, 29), 320.0, _ts("2025-05-01"))
        out = _attach_prior_year(pd.DataFrame([prior, cur]))
        by_end = {r.period_end: r for r in out.itertuples(index=False)}
        # 2026 row gets the 2025 values as *_prior
        assert by_end[dt.date(2026, 3, 28)].rev_prior == 320.0
        assert by_end[dt.date(2026, 3, 28)].rev_prior_av == _ts("2025-05-01")
        # 2025 row has no prior in the frame
        assert pd.isna(by_end[dt.date(2025, 3, 29)].rev_prior)

    def test_no_match_outside_one_year_window(self):
        cur = self._base(dt.date(2026, 3, 28), 400.0, _ts("2026-05-01"))
        far = self._base(dt.date(2024, 1, 1), 200.0, _ts("2024-02-01"))  # ~2yr prior
        out = _attach_prior_year(pd.DataFrame([far, cur]))
        by_end = {r.period_end: r for r in out.itertuples(index=False)}
        assert pd.isna(by_end[dt.date(2026, 3, 28)].rev_prior)


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

    def test_reads_liquidity_metrics_from_xbrl_table(self, tmp_store):
        sid, sym, end = _seed_minimal(tmp_store)
        av = dt.datetime(2026, 5, 1, 22, 0)

        def _xm(metric, value):
            tmp_store.con.execute(
                """
                INSERT INTO fundamental_xbrl_metric (
                    metric_id, source, security_id, symbol, cik, canonical_metric, concept,
                    taxonomy, period_type, period_end, value, is_latest_revision, as_of_date, available_at
                ) VALUES (?, 'x', ?, ?, '320193', ?, ?, 'us-gaap', 'instant', ?, ?, true, ?, ?)
                """,
                [f"{metric}|{end}", sid, sym, metric, metric, end, value, end, av],
            )

        _xm("current_assets", 200.0)
        _xm("current_liabilities", 100.0)
        _xm("cash_and_equivalents", 40.0)
        _xm("inventory", 30.0)
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        df = tmp_store.con.execute(
            "SELECT ratio_code, value FROM fundamental_ratios WHERE ratio_category='liquidity'"
        ).df()
        codes = dict(zip(df["ratio_code"], df["value"]))
        assert codes["current_ratio"] == pytest.approx(2.0)
        assert codes["quick_ratio"] == pytest.approx(1.7)
        assert codes["cash_ratio"] == pytest.approx(0.4)

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
