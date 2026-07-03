from __future__ import annotations

import datetime as dt

import pandas as pd

from db.valuation_multiples import (
    MarketCapDataset,
    MarketCapOptions,
    compute_market_cap_rows,
    refresh_market_cap,
)
from db.warehouse import insert_frame


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
    from db.asof import market_cap_asof

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
