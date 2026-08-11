from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.valuation_multiples import (
    VALUATION_FORMULA_DEFS,
    MarketCapDataset,
    MarketCapOptions,
    ValuationMultiplesDataset,
    ValuationMultiplesOptions,
    compute_market_cap_rows,
    compute_valuation_multiple_rows,
    refresh_market_cap,
    refresh_valuation_multiples,
    valuation_multiples_overlap_coverage,
)
from atx_db.warehouse import insert_frame


def _price(
    security_id: str,
    symbol: str,
    trade_date: dt.date,
    close: float,
    available_at: dt.datetime,
    *,
    source: str = "bulk_bars_2015plus",
    run_id: str = "price-run",
) -> dict[str, object]:
    return {
        "source": source,
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": trade_date,
        "close": close,
        "available_at": available_at,
        "run_id": run_id,
    }


def _share(
    security_id: str,
    symbol: str,
    share_count_type: str,
    effective_date: dt.date,
    available_at: dt.datetime,
    share_count: float,
    *,
    share_history_id: str,
    as_of_date: dt.date | None = None,
    source: str = "SEC XBRL share counts",
    run_id: str = "share-run",
    revision_sequence: int = 1,
    is_latest_revision: bool = True,
) -> dict[str, object]:
    as_of_date = as_of_date or effective_date
    return {
        "share_history_id": share_history_id,
        "source": source,
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "share_count_type": share_count_type,
        "taxonomy": "us-gaap",
        "concept": share_count_type,
        "unit": "shares",
        "period_type": "instant",
        "period_start": pd.NaT,
        "period_end": effective_date,
        "effective_date": effective_date,
        "as_of_date": as_of_date,
        "available_at": available_at,
        "fiscal_year": 2020,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{share_history_id}",
        "revision_sequence": revision_sequence,
        "revision_count": 1,
        "is_latest_revision": is_latest_revision,
        "share_count": share_count,
        "source_url": "fixture",
        "run_id": run_id,
    }


def _seed_market_cap_inputs(tmp_store) -> None:
    prices = pd.DataFrame(
        [
            _price("SEC-A", "AAA", dt.date(2020, 1, 2), 10.0, dt.datetime(2020, 1, 2, 22)),
            _price("SEC-B", "BBB", dt.date(2020, 1, 2), 20.0, dt.datetime(2020, 1, 2, 22)),
            _price("SEC-C", "CCC", dt.date(2020, 1, 2), 30.0, dt.datetime(2020, 1, 2, 22)),
        ]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-A",
                "AAA",
                "shares_diluted_avg",
                dt.date(2020, 1, 1),
                dt.datetime(2020, 1, 2, 9),
                999.0,
                share_history_id="a-diluted",
            ),
            _share(
                "SEC-A",
                "AAA",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                100.0,
                share_history_id="a-instant",
            ),
            _share(
                "SEC-B",
                "BBB",
                "shares_diluted_avg",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                200.0,
                share_history_id="b-diluted",
            ),
            _share(
                "SEC-C",
                "CCC",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                300.0,
                share_history_id="c-visible",
            ),
            _share(
                "SEC-C",
                "CCC",
                "shares_outstanding",
                dt.date(2020, 1, 1),
                dt.datetime(2020, 1, 3, 9),
                9999.0,
                share_history_id="c-lookahead",
                revision_sequence=2,
            ),
        ]
    )
    insert_frame(tmp_store, prices, "equity_daily_bars", "market_cap_price_seed")
    insert_frame(tmp_store, shares, "shares_outstanding_history", "market_cap_share_seed")


def _valuation_wide_row(**overrides) -> dict[str, object]:
    base = {
        "market_cap_id": "mc-a",
        "market_cap_source": "derived_market_cap_v1",
        "security_id": "SEC-A",
        "symbol": "AAA",
        "trade_date": dt.date(2020, 1, 2),
        "price": 10.0,
        "market_cap": 1000.0,
        "market_cap_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "price_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "market_cap_input_lineage_json": '{"market_cap": "fixture"}',
        "period_start": dt.date(2019, 1, 1),
        "period_end": dt.date(2019, 12, 31),
        "fiscal_year": 2019,
        "fiscal_period": "FY",
        "fundamental_available_at": pd.Timestamp("2020-02-06 10:00:00"),
        "fundamental_sort_key": "fund-a",
        "rev": 400.0,
        "rev_av": pd.Timestamp("2020-02-01 10:00:00"),
        "rev_id": "ttm-rev",
        "rev_source": "stmt",
        "ni": 100.0,
        "ni_av": pd.Timestamp("2020-02-03 10:00:00"),
        "ni_id": "ttm-ni",
        "ni_source": "stmt",
        "oi": 120.0,
        "oi_av": pd.Timestamp("2020-02-04 10:00:00"),
        "oi_id": "ttm-oi",
        "oi_source": "stmt",
        "ocf": 130.0,
        "ocf_av": pd.Timestamp("2020-02-01 10:00:00"),
        "ocf_id": "ttm-ocf",
        "ocf_source": "stmt",
        "capex": -30.0,
        "capex_av": pd.Timestamp("2020-02-01 10:00:00"),
        "capex_id": "ttm-capex",
        "capex_source": "stmt",
        "div": -10.0,
        "div_av": pd.Timestamp("2020-02-02 10:00:00"),
        "div_id": "ttm-div",
        "div_source": "stmt",
        "repurch": -50.0,
        "repurch_av": pd.Timestamp("2020-02-02 10:00:00"),
        "repurch_id": "ttm-repurch",
        "repurch_source": "stmt",
        "assets": 350.0,
        "assets_av": pd.Timestamp("2020-02-02 10:00:00"),
        "assets_id": "stmt-assets",
        "assets_source": "stmt",
        "equity": 50.0,
        "equity_av": pd.Timestamp("2020-02-02 10:00:00"),
        "equity_id": "stmt-equity",
        "equity_source": "stmt",
        "long_term_debt": 200.0,
        "long_term_debt_av": pd.Timestamp("2020-02-05 10:00:00"),
        "long_term_debt_id": "x-debt",
        "long_term_debt_source": "xbrl",
        "cash_and_equivalents": 50.0,
        "cash_and_equivalents_av": pd.Timestamp("2020-02-06 10:00:00"),
        "cash_and_equivalents_id": "x-cash",
        "cash_and_equivalents_source": "xbrl",
        "depreciation_amortization": 30.0,
        "depreciation_amortization_av": pd.Timestamp("2020-02-04 11:00:00"),
        "depreciation_amortization_id": "x-da",
        "depreciation_amortization_source": "xbrl",
    }
    base.update(overrides)
    return base


def _valuation_rows_by_code(frame: pd.DataFrame) -> dict[str, pd.Series]:
    return {row.formula_code: row for row in frame.itertuples(index=False)}


def _ttm_row(
    security_id: str,
    symbol: str,
    metric: str,
    value: float,
    *,
    end: dt.date = dt.date(2019, 12, 31),
    av: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> dict[str, object]:
    return {
        "ttm_point_id": f"ttm-{security_id}-{metric}",
        "ttm_revision_group_id": f"ttm-rg-{security_id}-{metric}",
        "anchor_statement_point_id": f"anchor-{security_id}-{metric}",
        "source": "fixture_ttm",
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "statement_type": "income_statement",
        "statement_section": metric,
        "canonical_metric": metric,
        "canonical_label": metric.replace("_", " ").title(),
        "unit": "USD",
        "unit_type": "monetary",
        "ttm_start_date": dt.date(end.year, 1, 1),
        "ttm_end_date": end,
        "as_of_date": end,
        "available_at": av,
        "fiscal_year": end.year,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{security_id}-{metric}",
        "quarter_count": 4,
        "coverage_days": 365,
        "min_input_available_at": av,
        "max_input_available_at": av,
        "input_statement_point_ids_json": "[]",
        "input_accessions_json": "[]",
        "input_period_ends_json": "[]",
        "ttm_value": value,
        "previous_ttm_value": None,
        "ttm_value_delta": None,
        "ttm_value_delta_percent": None,
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "calculation_method": "fixture",
        "source_loaded_at": av,
    }


def _stmt_row(
    security_id: str,
    symbol: str,
    metric: str,
    value: float,
    *,
    end: dt.date = dt.date(2019, 12, 31),
    av: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> dict[str, object]:
    return {
        "statement_point_id": f"stmt-{security_id}-{metric}",
        "fact_revision_id": f"fact-{security_id}-{metric}",
        "revision_group_id": f"rg-{security_id}-{metric}",
        "source": "fixture_statement",
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "statement_type": "balance_sheet",
        "statement_section": "equity",
        "canonical_metric": metric,
        "canonical_label": metric.replace("_", " ").title(),
        "taxonomy": "us-gaap",
        "concept": metric,
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "instant",
        "normal_balance": "credit",
        "period_start": None,
        "period_end": end,
        "as_of_date": end,
        "available_at": av,
        "fiscal_year": end.year,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{security_id}-{metric}",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "raw_value": value,
        "value": value,
        "previous_raw_value": None,
        "previous_value": None,
        "value_delta": None,
        "value_delta_percent": None,
        "run_id": "stmt-run",
        "source_url": "fixture",
        "source_loaded_at": av,
    }


def _xbrl_row(
    security_id: str,
    symbol: str,
    metric: str,
    value: float,
    *,
    period_type: str,
    end: dt.date = dt.date(2019, 12, 31),
    av: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> dict[str, object]:
    return {
        "metric_id": f"xbrl-{security_id}-{metric}",
        "source": "fixture_xbrl",
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "canonical_metric": metric,
        "concept": metric,
        "taxonomy": "us-gaap",
        "unit": "USD",
        "period_type": period_type,
        "period_start": dt.date(end.year, 1, 1) if period_type == "duration" else None,
        "period_end": end,
        "fiscal_year": end.year,
        "fiscal_period": "FY",
        "accession_number": f"acc-{security_id}-{metric}",
        "value": value,
        "raw_value": str(value),
        "revision_seq": 1,
        "is_latest_revision": True,
        "as_of_date": end,
        "available_at": av,
        "run_id": "xbrl-run",
    }


def _seed_valuation_fundamentals(
    tmp_store,
    security_id: str,
    symbol: str,
    *,
    revenue: float = 400.0,
    net_income: float = 100.0,
    operating_income: float = 120.0,
    operating_cash_flow: float = 130.0,
    capex: float = -30.0,
    dividends: float = -10.0,
    repurchases: float = -50.0,
    assets: float = 350.0,
    equity: float = 50.0,
    debt: float = 200.0,
    cash: float = 50.0,
    depreciation: float = 30.0,
    av: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> None:
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _ttm_row(security_id, symbol, "revenue", revenue, av=av),
                _ttm_row(security_id, symbol, "net_income", net_income, av=av),
                _ttm_row(security_id, symbol, "operating_income", operating_income, av=av),
                _ttm_row(security_id, symbol, "operating_cash_flow", operating_cash_flow, av=av),
                _ttm_row(security_id, symbol, "capital_expenditures", capex, av=av),
                _ttm_row(security_id, symbol, "dividends_paid", dividends, av=av),
                _ttm_row(security_id, symbol, "share_repurchases", repurchases, av=av),
            ]
        ),
        "fundamental_ttm_points",
        f"valuation_ttm_seed_{symbol}",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _stmt_row(security_id, symbol, "assets", assets, av=av),
                _stmt_row(security_id, symbol, "stockholders_equity", equity, av=av),
            ]
        ),
        "fundamental_statement_points",
        f"valuation_stmt_seed_{symbol}",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _xbrl_row(security_id, symbol, "long_term_debt", debt, period_type="instant", av=av),
                _xbrl_row(security_id, symbol, "cash_and_equivalents", cash, period_type="instant", av=av),
                _xbrl_row(
                    security_id,
                    symbol,
                    "depreciation_amortization",
                    depreciation,
                    period_type="duration",
                    av=av,
                ),
            ]
        ),
        "fundamental_xbrl_metric",
        f"valuation_xbrl_seed_{symbol}",
    )


def test_compute_market_cap_uses_raw_close_times_pit_shares() -> None:
    rows = compute_market_cap_rows(
        pd.DataFrame(
            [
                _price("SEC-A", "AAA", dt.date(2020, 1, 2), 10.0, dt.datetime(2020, 1, 2, 22)),
            ]
        ),
        pd.DataFrame(
            [
                _share(
                    "SEC-A",
                    "AAA",
                    "shares_outstanding",
                    dt.date(2019, 12, 31),
                    dt.datetime(2020, 1, 2, 10),
                    123.0,
                    share_history_id="a-instant",
                )
            ]
        ),
        run_id="market-run",
    )

    assert len(rows) == 1
    row = rows.iloc[0]
    assert row["close"] == 10.0
    assert row["share_count"] == 123.0
    assert row["market_cap"] == 1230.0
    assert row["available_at"] == pd.Timestamp(dt.datetime(2020, 1, 2, 22))
    assert row["run_id"] == "market-run"


def test_compute_market_cap_rejects_zero_price_and_zero_shares() -> None:
    prices = pd.DataFrame(
        [
            _price("GOOD", "GOOD", dt.date(2020, 1, 2), 10.0, dt.datetime(2020, 1, 2, 22)),
            _price("ZERO-P", "ZERO-P", dt.date(2020, 1, 2), 0.0, dt.datetime(2020, 1, 2, 22)),
            _price("ZERO-S", "ZERO-S", dt.date(2020, 1, 2), 10.0, dt.datetime(2020, 1, 2, 22)),
        ]
    )
    shares = pd.DataFrame(
        [
            _share(
                security_id,
                security_id,
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                share_count,
                share_history_id=f"{security_id}-shares",
            )
            for security_id, share_count in (
                ("GOOD", 100.0),
                ("ZERO-P", 100.0),
                ("ZERO-S", 0.0),
            )
        ]
    )

    rows = compute_market_cap_rows(prices, shares)

    assert rows["security_id"].tolist() == ["GOOD"]


def test_compute_market_cap_prefers_instant_shares_over_diluted_fallback() -> None:
    prices = pd.DataFrame(
        [_price("SEC-A", "AAA", dt.date(2020, 1, 2), 10.0, dt.datetime(2020, 1, 2, 22))]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-A",
                "AAA",
                "shares_diluted_avg",
                dt.date(2020, 1, 1),
                dt.datetime(2020, 1, 2, 9),
                999.0,
                share_history_id="diluted",
            ),
            _share(
                "SEC-A",
                "AAA",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                100.0,
                share_history_id="instant",
            ),
        ]
    )

    row = compute_market_cap_rows(prices, shares).iloc[0]

    assert row["share_count_type_used"] == "shares_outstanding"
    assert row["share_count"] == 100.0
    assert row["market_cap"] == 1000.0


def test_compute_market_cap_falls_back_to_diluted_when_instant_absent() -> None:
    prices = pd.DataFrame(
        [_price("SEC-B", "BBB", dt.date(2020, 1, 2), 20.0, dt.datetime(2020, 1, 2, 22))]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-B",
                "BBB",
                "shares_diluted_avg",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                200.0,
                share_history_id="diluted",
            )
        ]
    )

    row = compute_market_cap_rows(prices, shares).iloc[0]

    assert row["share_count_type_used"] == "shares_diluted_avg"
    assert row["market_cap"] == 4000.0


def test_compute_market_cap_allows_later_filed_applicable_share_count() -> None:
    prices = pd.DataFrame(
        [_price("SEC-C", "CCC", dt.date(2020, 1, 2), 30.0, dt.datetime(2020, 1, 2, 22))]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-C",
                "CCC",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                300.0,
                share_history_id="visible",
            ),
            _share(
                "SEC-C",
                "CCC",
                "shares_outstanding",
                dt.date(2020, 1, 1),
                dt.datetime(2020, 1, 3, 9),
                9999.0,
                share_history_id="lookahead",
                revision_sequence=2,
            ),
        ]
    )

    row = compute_market_cap_rows(prices, shares).iloc[0]

    assert row["share_history_id"] == "lookahead"
    assert row["market_cap"] == 299970.0
    assert row["available_at"] == pd.Timestamp(dt.datetime(2020, 1, 3, 9))


def test_compute_market_cap_keeps_superseded_applicable_share_revision() -> None:
    prices = pd.DataFrame(
        [_price("SEC-D", "DDD", dt.date(2020, 1, 2), 40.0, dt.datetime(2020, 1, 2, 22))]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-D",
                "DDD",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                400.0,
                share_history_id="older-visible",
                is_latest_revision=False,
            ),
            _share(
                "SEC-D",
                "DDD",
                "shares_outstanding",
                dt.date(2020, 3, 31),
                dt.datetime(2020, 4, 1, 10),
                9999.0,
                share_history_id="future-current",
                revision_sequence=2,
                is_latest_revision=True,
            ),
        ]
    )

    row = compute_market_cap_rows(prices, shares).iloc[0]

    assert row["share_history_id"] == "older-visible"
    assert row["market_cap"] == 16000.0


def test_refresh_market_cap_is_idempotent_and_asof_visible(tmp_store) -> None:
    from atx_db.asof import market_cap_asof

    _seed_market_cap_inputs(tmp_store)

    assert refresh_market_cap(tmp_store, MarketCapOptions(run_id="run-1")) == 3
    assert refresh_market_cap(tmp_store, MarketCapOptions(run_id="run-2")) == 3

    rows = tmp_store.con.execute(
        """
        SELECT symbol, share_count_type_used, market_cap, available_at, run_id
        FROM market_cap
        ORDER BY symbol
        """
    ).fetchall()
    assert rows == [
        ("AAA", "shares_outstanding", 1000.0, dt.datetime(2020, 1, 2, 22), "run-2"),
        ("BBB", "shares_diluted_avg", 4000.0, dt.datetime(2020, 1, 2, 22), "run-2"),
        ("CCC", "shares_outstanding", 299970.0, dt.datetime(2020, 1, 3, 9), "run-2"),
    ]

    early = market_cap_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 1, 2, 21, 59),
        store=tmp_store,
    )
    late = market_cap_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 1, 2, 22),
        store=tmp_store,
        symbols=["AAA"],
    )
    assert early.empty
    assert late[["symbol", "market_cap"]].to_dict("records") == [{"symbol": "AAA", "market_cap": 1000.0}]


def test_refresh_market_cap_scoped_symbol_preserves_other_rows(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)

    assert refresh_market_cap(tmp_store, MarketCapOptions(run_id="initial")) == 3
    assert refresh_market_cap(tmp_store, MarketCapOptions(symbols=("AAA",), run_id="scoped")) == 1

    rows = tmp_store.con.execute(
        """
        SELECT symbol, market_cap, run_id
        FROM market_cap
        ORDER BY symbol
        """
    ).fetchall()

    assert rows == [
        ("AAA", 1000.0, "scoped"),
        ("BBB", 4000.0, "initial"),
        ("CCC", 299970.0, "initial"),
    ]


def test_refresh_market_cap_keeps_superseded_applicable_share_revision(tmp_store) -> None:
    prices = pd.DataFrame(
        [_price("SEC-D", "DDD", dt.date(2020, 1, 2), 40.0, dt.datetime(2020, 1, 2, 22))]
    )
    shares = pd.DataFrame(
        [
            _share(
                "SEC-D",
                "DDD",
                "shares_outstanding",
                dt.date(2019, 12, 31),
                dt.datetime(2020, 1, 2, 10),
                400.0,
                share_history_id="older-visible",
                is_latest_revision=False,
            ),
            _share(
                "SEC-D",
                "DDD",
                "shares_outstanding",
                dt.date(2020, 3, 31),
                dt.datetime(2020, 4, 1, 10),
                9999.0,
                share_history_id="future-current",
                revision_sequence=2,
                is_latest_revision=True,
            ),
        ]
    )
    insert_frame(tmp_store, prices, "equity_daily_bars", "market_cap_superseded_price_seed")
    insert_frame(tmp_store, shares, "shares_outstanding_history", "market_cap_superseded_share_seed")

    assert refresh_market_cap(tmp_store, MarketCapOptions(run_id="sql-path")) == 1

    row = tmp_store.con.execute(
        """
        SELECT share_history_id, market_cap, available_at
        FROM market_cap
        WHERE symbol = 'DDD'
        """
    ).fetchone()
    assert row == ("older-visible", 16000.0, dt.datetime(2020, 1, 2, 22))


def test_market_cap_dataset_records_quality(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)

    result = MarketCapDataset().load(tmp_store, MarketCapOptions(run_id="dataset-run"))

    assert result.rows_loaded == 3
    assert tmp_store.con.execute(
        """
        SELECT status, observed_value
        FROM data_quality_checks
        WHERE dataset_id = 'market_cap'
          AND check_name = 'rows_materialized'
        """
    ).fetchall() == [("passed", 3.0)]


def test_market_cap_migration_and_catalog_are_present(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'market_cap'
            """
        ).fetchall()
    }

    assert {
        "market_cap_id",
        "security_id",
        "trade_date",
        "close",
        "share_count",
        "share_count_type_used",
        "market_cap",
        "available_at",
        "input_lineage_json",
    }.issubset(columns)
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0084'"
    ).fetchone()[0] == "market_cap_schema_catalog"
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0085'"
    ).fetchone()[0] == "market_cap_indexes"
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'market_cap'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE table_name = 'market_cap'
          AND field_name IN ('market_cap_id', 'market_cap', 'input_lineage_json')
        """
    ).fetchone()[0] == 3


def test_compute_valuation_multiple_rows_emits_all_sixteen_formulas() -> None:
    rows = compute_valuation_multiple_rows(pd.DataFrame([_valuation_wide_row()]), run_id="valuation-run")
    by_code = _valuation_rows_by_code(rows)

    assert set(by_code) == {
        "price_to_earnings",
        "price_to_book",
        "price_to_sales",
        "enterprise_value",
        "ev_to_ebitda",
        "ev_to_sales",
        "fcf_yield",
        "earnings_yield",
        "dividend_yield",
        "price_to_cash_flow",
        "price_to_free_cash_flow",
        "ev_to_ebit",
        "ev_to_fcf",
        "ev_to_assets",
        "buyback_yield",
        "shareholder_yield",
    }
    expected = {
        "price_to_earnings": 10.0,
        "price_to_book": 20.0,
        "price_to_sales": 2.5,
        "enterprise_value": 1150.0,
        "ev_to_ebitda": 1150.0 / 150.0,
        "ev_to_sales": 1150.0 / 400.0,
        "fcf_yield": 0.10,
        "earnings_yield": 0.10,
        "dividend_yield": 0.01,
        "price_to_cash_flow": 1000.0 / 130.0,
        "price_to_free_cash_flow": 10.0,
        "ev_to_ebit": 1150.0 / 120.0,
        "ev_to_fcf": 11.5,
        "ev_to_assets": 1150.0 / 350.0,
        "buyback_yield": 0.05,
        "shareholder_yield": 0.06,
    }
    for code, value in expected.items():
        assert by_code[code].value == pytest.approx(value)
        assert by_code[code].is_meaningful
        assert by_code[code].run_id == "valuation-run"

    assert by_code["enterprise_value"].numerator_value == pytest.approx(1200.0)
    assert by_code["enterprise_value"].denominator_value == pytest.approx(50.0)
    assert by_code["ev_to_ebitda"].enterprise_value == pytest.approx(1150.0)
    assert by_code["ev_to_ebitda"].denominator_value == pytest.approx(150.0)
    assert by_code["ev_to_fcf"].enterprise_value == pytest.approx(1150.0)
    assert by_code["ev_to_fcf"].denominator_value == pytest.approx(100.0)
    assert by_code["buyback_yield"].numerator_value == pytest.approx(50.0)
    assert by_code["shareholder_yield"].numerator_value == pytest.approx(60.0)

    ev_lineage = json.loads(by_code["ev_to_fcf"].input_lineage_json)["inputs"]["enterprise_value"]
    assert ev_lineage["available_at"] == "2020-02-06 10:00:00"
    assert set(ev_lineage["components"]) == {"market_cap", "long_term_debt", "cash_and_equivalents"}


def test_valuation_available_at_is_formula_specific_max() -> None:
    rows = compute_valuation_multiple_rows(pd.DataFrame([_valuation_wide_row()]))
    by_code = _valuation_rows_by_code(rows)

    assert by_code["price_to_earnings"].available_at == pd.Timestamp("2020-02-03 10:00:00")
    assert by_code["enterprise_value"].available_at == pd.Timestamp("2020-02-06 10:00:00")
    assert by_code["ev_to_fcf"].available_at == pd.Timestamp("2020-02-06 10:00:00")
    assert by_code["dividend_yield"].available_at == pd.Timestamp("2020-02-02 10:00:00")


def test_valuation_negative_denominator_emits_not_meaningful_row() -> None:
    rows = compute_valuation_multiple_rows(pd.DataFrame([_valuation_wide_row(ni=-25.0)]))
    pe = _valuation_rows_by_code(rows)["price_to_earnings"]

    assert pe.value == pytest.approx(-40.0)
    assert pe.denominator_value == -25.0
    assert not pe.is_meaningful


def test_compute_valuation_multiple_rows_uses_latest_applicable_fundamental_period() -> None:
    older = _valuation_wide_row(period_end=dt.date(2019, 9, 30), ni=50.0, fundamental_sort_key="older")
    latest = _valuation_wide_row(period_end=dt.date(2019, 12, 31), ni=100.0, fundamental_sort_key="latest")
    future = _valuation_wide_row(period_end=dt.date(2020, 3, 31), ni=999.0, fundamental_sort_key="future")

    rows = compute_valuation_multiple_rows(pd.DataFrame([older, latest, future]))
    pe = _valuation_rows_by_code(rows)["price_to_earnings"]

    assert pe.period_end == dt.date(2019, 12, 31)
    assert pe.value == pytest.approx(10.0)


def test_refresh_valuation_multiples_is_idempotent_and_scoped(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA")
    _seed_valuation_fundamentals(tmp_store, "SEC-B", "BBB", revenue=800.0, net_income=200.0)

    assert refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="run-1")) == 32
    assert refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="run-2")) == 32

    counts = tmp_store.con.execute(
        """
        SELECT symbol, count(*), min(run_id), max(run_id)
        FROM valuation_multiples
        GROUP BY symbol
        ORDER BY symbol
        """
    ).fetchall()
    assert counts == [("AAA", 16, "run-2", "run-2"), ("BBB", 16, "run-2", "run-2")]

    assert refresh_valuation_multiples(
        tmp_store,
        ValuationMultiplesOptions(symbols=("AAA",), run_id="scoped"),
    ) == 16
    counts = tmp_store.con.execute(
        """
        SELECT symbol, count(*), min(run_id), max(run_id)
        FROM valuation_multiples
        GROUP BY symbol
        ORDER BY symbol
        """
    ).fetchall()
    assert counts == [("AAA", 16, "scoped", "scoped"), ("BBB", 16, "run-2", "run-2")]


def test_refresh_valuation_multiples_blank_symbol_scope_preserves_rows(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA")
    _seed_valuation_fundamentals(tmp_store, "SEC-B", "BBB", revenue=800.0, net_income=200.0)

    assert refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="initial")) == 32
    before = tmp_store.con.execute(
        """
        SELECT symbol, formula_code, value, run_id
        FROM valuation_multiples
        ORDER BY symbol, formula_code
        """
    ).fetchall()

    assert refresh_valuation_multiples(
        tmp_store,
        ValuationMultiplesOptions(symbols=(" ",), run_id="blank-symbol"),
    ) == 0
    after = tmp_store.con.execute(
        """
        SELECT symbol, formula_code, value, run_id
        FROM valuation_multiples
        ORDER BY symbol, formula_code
        """
    ).fetchall()

    assert len(before) == 32
    assert after == before


def test_valuation_multiples_asof_visibility(tmp_store) -> None:
    from atx_db.asof import valuation_multiples_asof

    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA", av=dt.datetime(2020, 2, 1, 10))
    refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="valuation-run"))

    early = valuation_multiples_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 1, 31, 23, 59),
        store=tmp_store,
        symbols=["AAA"],
    )
    late = valuation_multiples_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 2, 1, 10),
        store=tmp_store,
        symbols=["AAA"],
        formula_codes=["price_to_earnings"],
    )

    assert early.empty
    assert late[["symbol", "formula_code", "value"]].to_dict("records") == [
        {"symbol": "AAA", "formula_code": "price_to_earnings", "value": 10.0}
    ]


def test_valuation_multiples_dataset_records_quality(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA")

    result = ValuationMultiplesDataset().load(tmp_store, ValuationMultiplesOptions(run_id="dataset-run"))

    assert result.rows_loaded == 16
    assert tmp_store.con.execute(
        """
        SELECT status, observed_value
        FROM data_quality_checks
        WHERE dataset_id = 'valuation_multiples'
          AND check_name = 'rows_materialized'
        """
    ).fetchall() == [("passed", 16.0)]


def test_valuation_multiples_dataset_records_overlap_coverage_quality(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA")
    _seed_valuation_fundamentals(tmp_store, "SEC-D", "DDD")

    result = ValuationMultiplesDataset().load(tmp_store, ValuationMultiplesOptions(run_id="coverage-run"))

    assert result.rows_loaded == 16
    status, observed, details_json = tmp_store.con.execute(
        """
        SELECT status, observed_value, details_json
        FROM data_quality_checks
        WHERE dataset_id = 'valuation_multiples'
          AND check_name = 'overlap_coverage'
        """
    ).fetchone()
    details = json.loads(details_json)

    assert status == "warning"
    assert observed == pytest.approx(0.5)
    assert details["numerator_security_count"] == 1
    assert details["denominator_security_count"] == 2
    assert details["coverage_ratio"] == pytest.approx(0.5)
    assert details["min_valuation_trade_date"] == "2020-01-02"
    assert details["max_valuation_trade_date"] == "2020-01-02"
    assert details["min_valuation_period_end"] == "2019-12-31"
    assert details["as_of_ts"] == "2020-02-01T10:00:00"
    assert details["stale_price_fundamental_gap_days"] == 5

    slice_row = tmp_store.con.execute(
        """
        SELECT numerator_security_count, denominator_security_count, coverage_ratio, valuation_row_count, run_id
        FROM valuation_overlap_slice
        WHERE source = 'derived_valuation_multiples_v1'
        """
    ).fetchone()
    assert slice_row == (1, 2, pytest.approx(0.5), 16, "coverage-run")


def test_valuation_overlap_coverage_respects_available_at(tmp_store) -> None:
    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA", av=dt.datetime(2020, 2, 1, 10))
    _seed_valuation_fundamentals(tmp_store, "SEC-D", "DDD", av=dt.datetime(2020, 3, 1, 10))
    refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="coverage-run"))

    early = valuation_multiples_overlap_coverage(
        tmp_store,
        ValuationMultiplesOptions(),
        as_of_ts=dt.datetime(2020, 2, 15, 12),
    )
    late = valuation_multiples_overlap_coverage(
        tmp_store,
        ValuationMultiplesOptions(),
        as_of_ts=dt.datetime(2020, 3, 1, 10),
    )

    assert early["numerator_security_count"] == 1
    assert early["denominator_security_count"] == 1
    assert early["coverage_ratio"] == pytest.approx(1.0)
    assert late["numerator_security_count"] == 1
    assert late["denominator_security_count"] == 2
    assert late["coverage_ratio"] == pytest.approx(0.5)


def test_valuation_multiples_dataset_flags_stale_price_fundamental_gap(tmp_store) -> None:
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _price(
                    "SEC-Z",
                    "ZZZ",
                    dt.date(2020, 2, 15),
                    10.0,
                    dt.datetime(2020, 2, 15, 22),
                )
            ]
        ),
        "equity_daily_bars",
        "stale_price_seed",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _share(
                    "SEC-Z",
                    "ZZZ",
                    "shares_outstanding",
                    dt.date(2019, 12, 31),
                    dt.datetime(2020, 1, 1, 10),
                    100.0,
                    share_history_id="z-share",
                )
            ]
        ),
        "shares_outstanding_history",
        "stale_share_seed",
    )
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-Z", "ZZZ")

    result = ValuationMultiplesDataset().load(
        tmp_store,
        ValuationMultiplesOptions(
            run_id="stale-run",
            stale_price_fundamental_gap_days=5,
        ),
    )

    assert result.rows_loaded == 16
    status, observed, details_json = tmp_store.con.execute(
        """
        SELECT status, observed_value, details_json
        FROM data_quality_checks
        WHERE dataset_id = 'valuation_multiples'
          AND check_name = 'stale_price_fundamental_gap_days'
        """
    ).fetchone()
    details = json.loads(details_json)

    assert status == "warning"
    assert observed == 16.0
    assert details["stale_valuation_row_count"] == 16
    assert details["max_price_fundamental_gap_days"] == 46
    assert details["stale_price_fundamental_gap_days"] == 5
    assert {row["formula_code"] for row in details["rows"]} == {
        "price_to_earnings",
        "price_to_book",
        "price_to_sales",
        "enterprise_value",
        "ev_to_ebitda",
        "ev_to_sales",
        "fcf_yield",
        "earnings_yield",
        "dividend_yield",
        "price_to_cash_flow",
        "price_to_free_cash_flow",
        "ev_to_ebit",
        "ev_to_fcf",
        "ev_to_assets",
        "buyback_yield",
        "shareholder_yield",
    }


def test_valuation_multiples_warehouse_quality_specs_pass_clean_fixture(tmp_store) -> None:
    from atx_db.quality import run_warehouse_quality_checks

    _seed_market_cap_inputs(tmp_store)
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-A", "AAA")
    refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="quality-run"))

    quality_checks = (
        "duplicate_valuation_multiple_natural_keys",
        "bad_valuation_multiple_rows",
        "non_finite_valuation_multiple_values",
        "valuation_multiple_arithmetic_consistency",
        "valuation_multiple_non_positive_denominator_meaningfulness",
        "stale_price_fundamental_gap_days",
    )
    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=quality_checks,
        )
    }

    for check_name in quality_checks:
        assert results[check_name].status == "passed", check_name


def test_valuation_multiples_warehouse_quality_stale_gap_fires(tmp_store) -> None:
    from atx_db.quality import run_warehouse_quality_checks

    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _price(
                    "SEC-Z",
                    "ZZZ",
                    dt.date(2020, 2, 15),
                    10.0,
                    dt.datetime(2020, 2, 15, 22),
                )
            ]
        ),
        "equity_daily_bars",
        "quality_stale_price_seed",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _share(
                    "SEC-Z",
                    "ZZZ",
                    "shares_outstanding",
                    dt.date(2019, 12, 31),
                    dt.datetime(2020, 1, 1, 10),
                    100.0,
                    share_history_id="quality-z-share",
                )
            ]
        ),
        "shares_outstanding_history",
        "quality_stale_share_seed",
    )
    refresh_market_cap(tmp_store, MarketCapOptions(run_id="market-run"))
    _seed_valuation_fundamentals(tmp_store, "SEC-Z", "ZZZ")
    refresh_valuation_multiples(tmp_store, ValuationMultiplesOptions(run_id="quality-run"))

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            valuation_stale_gap_days=5,
            check_names=("stale_price_fundamental_gap_days",),
        )
    }
    stale = results["stale_price_fundamental_gap_days"]

    assert stale.status == "warning"
    assert stale.observed_value == 16.0
    assert stale.details["rows"][0]["gap_days"] == 46


def test_valuation_multiples_migration_catalog_and_formula_seed_are_present(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'valuation_multiples'
            """
        ).fetchall()
    }

    assert {
        "valuation_multiple_id",
        "formula_code",
        "value",
        "market_cap",
        "enterprise_value",
        "input_lineage_json",
    }.issubset(columns)
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0086'"
    ).fetchone()[0] == "valuation_multiples_schema_catalog"
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0087'"
    ).fetchone()[0] == "valuation_multiples_indexes"
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0124'"
    ).fetchone()[0] == "valuation_overlap_slice_schema_catalog"
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0127'"
    ).fetchone()[0] == "pf2_s9_indexes_report"
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'valuation_multiples'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'valuation_overlap_slice'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE table_name = 'valuation_multiples'
          AND field_name IN ('valuation_multiple_id', 'formula_code', 'input_lineage_json')
        """
    ).fetchone()[0] == 3

    from atx_db.formula_library import read_formula_registry_seed

    valuation_rows = {
        row.formula_code: row
        for row in read_formula_registry_seed()
        if row.family == "valuation"
    }
    assert set(valuation_rows) == {
        "price_to_earnings",
        "price_to_book",
        "price_to_sales",
        "enterprise_value",
        "ev_to_ebitda",
        "ev_to_sales",
        "fcf_yield",
        "earnings_yield",
        "dividend_yield",
        "price_to_cash_flow",
        "price_to_free_cash_flow",
        "ev_to_ebit",
        "ev_to_fcf",
        "ev_to_assets",
        "buyback_yield",
        "shareholder_yield",
    }
    assert valuation_rows["enterprise_value"].kind == "difference"
    assert valuation_rows["enterprise_value"].expression == "sum:market_cap,long_term_debt|key:cash_and_equivalents"
    assert json.loads(valuation_rows["price_to_earnings"].inputs) == ["market_cap", "ni"]


def test_valuation_formula_seed_inputs_match_implementation_defs() -> None:
    from atx_db.formula_library import read_formula_registry_seed

    seed_inputs = {
        row.formula_code: json.loads(row.inputs)
        for row in read_formula_registry_seed()
        if row.family == "valuation"
    }
    expected_inputs = {
        definition.code: list(definition.input_keys)
        for definition in VALUATION_FORMULA_DEFS
    }

    assert seed_inputs == expected_inputs
