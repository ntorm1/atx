from __future__ import annotations

import datetime as dt

import pandas as pd

from db.asof import universe_membership_asof
from db.universe import (
    GovernedUniverseMembershipDataset,
    UniverseMembershipOptions,
    compute_universe_membership_intervals,
)
from db.warehouse import insert_frame


def _dates() -> list[dt.date]:
    return [dt.date(2020, 1, 2) + dt.timedelta(days=i) for i in range(4)]


def _daily_decisions() -> pd.DataFrame:
    rows = []
    for day in _dates():
        rows.append(
            {
                "security_id": "COMMON",
                "symbol": "COM",
                "as_of_date": day,
                "close": 20.0,
                "avg_dollar_volume": 20_000_000.0,
                "history_days": 1,
                "is_common_equity": True,
                "is_active_listing": True,
                "available_at": pd.Timestamp(day) + pd.Timedelta(hours=22),
            }
        )
    rows.append(
        {
            "security_id": "COMMON",
            "symbol": "COM",
            "as_of_date": dt.date(2020, 1, 6),
            "close": 4.0,
            "avg_dollar_volume": 20_000_000.0,
            "history_days": 1,
            "is_common_equity": True,
            "is_active_listing": True,
            "available_at": pd.Timestamp("2020-01-06 22:00:00"),
        }
    )
    rows.append(
        {
            "security_id": "PREF",
            "symbol": "PRF",
            "as_of_date": dt.date(2020, 1, 2),
            "close": 20.0,
            "avg_dollar_volume": 20_000_000.0,
            "history_days": 1,
            "is_common_equity": False,
            "is_active_listing": True,
            "available_at": pd.Timestamp("2020-01-02 22:00:00"),
        }
    )
    return pd.DataFrame(rows)


def test_compute_universe_membership_intervals_is_deterministic_and_keeps_exclusions():
    options = UniverseMembershipOptions(
        universe_id="test_common",
        lookback_days=1,
        min_history_days=1,
        min_price=5.0,
        min_dollar_volume=10_000_000.0,
        run_id="r1",
    )

    first = compute_universe_membership_intervals(_daily_decisions(), options)
    second = compute_universe_membership_intervals(_daily_decisions(), options)

    pd.testing.assert_frame_equal(first, second)
    common = first[first["security_id"] == "COMMON"].reset_index(drop=True)
    assert common[["valid_from", "valid_to", "is_member", "reason"]].to_dict("records") == [
        {
            "valid_from": dt.date(2020, 1, 2),
            "valid_to": dt.date(2020, 1, 5),
            "is_member": True,
            "reason": "member",
        },
        {
            "valid_from": dt.date(2020, 1, 6),
            "valid_to": dt.date(2020, 1, 6),
            "is_member": False,
            "reason": "liquidity_screen_fail",
        },
    ]
    pref = first[first["security_id"] == "PREF"].iloc[0]
    assert bool(pref["is_member"]) is False
    assert pref["reason"] == "not_common_equity"


def _bar_rows() -> pd.DataFrame:
    rows = []
    for day in _dates():
        for security_id, symbol in (
            ("COMMON", "COM"),
            ("PREF", "PRF"),
            ("DELIST", "DEL"),
        ):
            rows.append(
                {
                    "source": "fixture",
                    "security_id": security_id,
                    "vendor_security_id": security_id,
                    "symbol": symbol,
                    "trade_date": day,
                    "open": 20.0,
                    "high": 21.0,
                    "low": 19.0,
                    "close": 20.0,
                    "adjusted_close": 20.0,
                    "volume": 1_000_000,
                    "vwap": None,
                    "dividend_amount": None,
                    "split_factor": 1.0,
                    "is_adjusted": False,
                    "as_of_date": day,
                    "available_at": pd.Timestamp(day) + pd.Timedelta(hours=22),
                    "run_id": "bars",
                    "is_latest_revision": True,
                }
            )
    return pd.DataFrame(rows)


def _insert_security(store, security_id: str, symbol: str, asset_class: str = "EQUITY") -> None:
    store.con.execute(
        """
        INSERT INTO securities (
            security_id, primary_symbol, name, asset_class, country,
            currency, active, first_seen_date, source
        )
        VALUES (?, ?, ?, ?, 'US', 'USD', true, DATE '2020-01-02', 'fixture')
        """,
        [security_id, symbol, f"{symbol} Common Stock", asset_class],
    )


def _insert_listing(
    store,
    *,
    listing_status_id: str,
    security_id: str,
    symbol: str,
    status: str,
    valid_from: dt.date,
    valid_to: dt.date | None,
) -> None:
    store.con.execute(
        """
        INSERT INTO listing_status_intervals (
            listing_status_id, security_id, symbol, listing_venue_code,
            listing_venue_name, listing_exchange_code, status, valid_from,
            valid_to, as_of_date, available_at, source, evidence_source,
            evidence_source_table, method, source_loaded_at
        )
        VALUES (?, ?, ?, 'Q', 'Nasdaq', 'Q', ?, ?, ?, ?, ?, 'fixture',
                'fixture', 'fixture', 'fixture', ?)
        """,
        [
            listing_status_id,
            security_id,
            symbol,
            status,
            valid_from,
            valid_to,
            valid_from,
            pd.Timestamp(valid_from) + pd.Timedelta(hours=22),
            pd.Timestamp(valid_from) + pd.Timedelta(hours=22),
        ],
    )


def _governed_universe_options() -> UniverseMembershipOptions:
    return UniverseMembershipOptions(
        universe_id="test_common",
        start_date=dt.date(2020, 1, 2),
        end_date=dt.date(2020, 1, 5),
        lookback_days=1,
        min_history_days=1,
        min_price=5.0,
        min_dollar_volume=10_000_000.0,
        run_id="run-1",
    )


def _load_governed_universe_slice(tmp_store):
    _insert_security(tmp_store, "COMMON", "COM")
    _insert_security(tmp_store, "PREF", "PRF", asset_class="PREFERRED")
    _insert_security(tmp_store, "DELIST", "DEL")
    _insert_listing(
        tmp_store,
        listing_status_id="del-active",
        security_id="DELIST",
        symbol="DEL",
        status="active",
        valid_from=dt.date(2020, 1, 2),
        valid_to=dt.date(2020, 1, 3),
    )
    _insert_listing(
        tmp_store,
        listing_status_id="del-inactive",
        security_id="DELIST",
        symbol="DEL",
        status="inactive",
        valid_from=dt.date(2020, 1, 4),
        valid_to=None,
    )
    insert_frame(tmp_store, _bar_rows(), "equity_daily_bars", "governed_universe_bars")

    return GovernedUniverseMembershipDataset().run(tmp_store, _governed_universe_options())


def test_governed_universe_dataset_writes_interval_members_and_exclusions(tmp_store):
    result = _load_governed_universe_slice(tmp_store)

    assert result.rows_loaded == 4
    rows = tmp_store.con.execute(
        """
        SELECT security_id, valid_from, valid_to, is_member, reason
        FROM universe_membership
        WHERE universe_id = 'test_common'
        ORDER BY security_id, valid_from
        """
    ).fetchall()
    assert rows == [
        ("COMMON", dt.date(2020, 1, 2), dt.date(2020, 1, 5), True, "member"),
        ("DELIST", dt.date(2020, 1, 2), dt.date(2020, 1, 3), True, "member"),
        ("DELIST", dt.date(2020, 1, 4), dt.date(2020, 1, 5), False, "inactive_listing"),
        ("PREF", dt.date(2020, 1, 2), dt.date(2020, 1, 5), False, "not_common_equity"),
    ]

    # Idempotent replacement for the same slice: no duplicate intervals.
    second = GovernedUniverseMembershipDataset().run(tmp_store, _governed_universe_options())
    assert second.rows_loaded == 4
    assert tmp_store.con.execute(
        """
        SELECT count(*), count(DISTINCT universe_id || ':' || security_id || ':' || valid_from || ':' || source)
        FROM universe_membership
        WHERE universe_id = 'test_common'
        """
    ).fetchone() == (4, 4)


def test_universe_membership_asof_gates_availability_and_validity(tmp_store):
    _load_governed_universe_slice(tmp_store)

    early = universe_membership_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 1, 2, 21, 59),
        store=tmp_store,
        universe_id="test_common",
    )
    assert early.empty

    jan3 = universe_membership_asof(
        dt.date(2020, 1, 3),
        as_of_ts=dt.datetime(2020, 1, 3, 23),
        store=tmp_store,
        universe_id="test_common",
    )
    assert jan3[["security_id", "symbol"]].to_dict("records") == [
        {"security_id": "COMMON", "symbol": "COM"},
        {"security_id": "DELIST", "symbol": "DEL"},
    ]

    jan4 = universe_membership_asof(
        dt.date(2020, 1, 4),
        as_of_ts=dt.datetime(2020, 1, 4, 23),
        store=tmp_store,
        universe_id="test_common",
    )
    assert jan4["security_id"].tolist() == ["COMMON"]


def test_price_fundamental_overlap_view_counts_visible_universe_security_days(tmp_store):
    _load_governed_universe_slice(tmp_store)
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                {
                    "source": "fixture",
                    "security_id": "COMMON",
                    "symbol": "COM",
                    "metric": "revenue",
                    "period_start": dt.date(2019, 1, 1),
                    "period_end": dt.date(2019, 12, 31),
                    "as_of_date": dt.date(2020, 1, 2),
                    "value": 100.0,
                    "available_at": pd.Timestamp("2020-01-02 21:00:00"),
                    "run_id": "fundamentals",
                }
            ]
        ),
        "fundamental_points",
        "overlap_fundamental_points",
    )

    row = tmp_store.con.execute(
        """
        SELECT
            universe_id,
            overlap_month,
            universe_price_days,
            price_fundamental_days,
            universe_priced_security_count,
            overlapped_security_count,
            overlap_ratio
        FROM v_price_fundamental_overlap
        WHERE universe_id = 'test_common'
        """
    ).fetchone()

    assert row[:6] == (
        "test_common",
        dt.date(2020, 1, 1),
        6,
        4,
        2,
        1,
    )
    assert round(row[6], 6) == round(4 / 6, 6)


def test_universe_membership_migration_catalogs_contract_surface(tmp_store):
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT table_name FROM duckdb_tables() WHERE table_name = 'universe_membership'"
        ).fetchall()
    }
    assert tables == {"universe_membership"}
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'universe_membership'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM schema_contract WHERE table_name = 'universe_membership'"
    ).fetchone()[0] >= 10
