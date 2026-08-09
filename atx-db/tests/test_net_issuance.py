from __future__ import annotations

import datetime as dt
import math

import pytest

from atx_db.net_issuance import (
    FACTOR_ID,
    NetIssuanceOptions,
    load_net_issuance_inputs,
    refresh_net_issuance_values,
)


def _insert_share_observation(
    store,
    *,
    security_id: str,
    symbol: str,
    effective_date: dt.date,
    available_at: dt.datetime,
    share_count: float,
) -> None:
    year = effective_date.year
    store.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, symbol, cik, share_count_type,
            taxonomy, concept, unit, period_type, period_end, effective_date,
            as_of_date, available_at, form, accession_number, revision_sequence,
            revision_count, is_latest_revision, share_count, source_url
        ) VALUES (?, 'test', ?, ?, ?, 'shares_outstanding', 'dei',
                  'EntityCommonStockSharesOutstanding', 'shares', 'instant', ?, ?, ?, ?,
                  '10-K', ?, 1, 1, true, ?, 'https://example.test')
        """,
        [
            f"share-{security_id}-{year}",
            security_id,
            symbol,
            f"CIK-{security_id}",
            effective_date,
            effective_date,
            effective_date,
            available_at,
            f"{security_id}-{year}",
            share_count,
        ],
    )


def _seed_split_fixture(store) -> None:
    prior_date = dt.date(2023, 12, 31)
    current_date = dt.date(2024, 12, 31)
    for security_id, symbol, current_shares in (
        ("SPLIT", "SPLT", 200.0),
        ("ISSUER", "ISSU", 110.0),
    ):
        _insert_share_observation(
            store,
            security_id=security_id,
            symbol=symbol,
            effective_date=prior_date,
            available_at=dt.datetime(2024, 2, 1, 12),
            share_count=100.0,
        )
        _insert_share_observation(
            store,
            security_id=security_id,
            symbol=symbol,
            effective_date=current_date,
            available_at=dt.datetime(2025, 2, 1, 12),
            share_count=current_shares,
        )

    dates = [dt.date(2023, 12, 29), dt.date(2024, 6, 3)]
    dates.extend(dt.date(2025, 2, day) for day in range(1, 29))
    for security_id, symbol in (("SPLIT", "SPLT"), ("ISSUER", "ISSU")):
        for trade_date in dates:
            is_split = security_id == "SPLIT" and trade_date == dt.date(2024, 6, 3)
            close = 50.0 if security_id == "SPLIT" and trade_date >= dt.date(2024, 6, 3) else 100.0
            store.con.execute(
                """
                INSERT INTO equity_daily_bars (
                    source, security_id, symbol, trade_date, close, volume,
                    split_factor, available_at
                ) VALUES ('test', ?, ?, ?, ?, 1000000, ?, ?)
                """,
                [
                    security_id,
                    symbol,
                    trade_date,
                    close,
                    0.5 if is_split else 1.0,
                    dt.datetime.combine(trade_date, dt.time(22)),
                ],
            )


def test_net_issuance_is_split_adjusted_and_oriented_toward_repurchases(tmp_store) -> None:
    _seed_split_fixture(tmp_store)
    options = NetIssuanceOptions(
        start_date=dt.date(2025, 2, 1),
        end_date=dt.date(2025, 2, 28),
        minimum_market_cap_usd=0,
        minimum_adv21_usd=0,
        minimum_names_per_date=2,
    )

    inputs = load_net_issuance_inputs(tmp_store, options).set_index("security_id")

    assert inputs.loc["SPLIT", "net_share_issuance"] == pytest.approx(0.0)
    assert inputs.loc["ISSUER", "net_share_issuance"] == pytest.approx(math.log(1.1))
    assert inputs.loc["SPLIT", "market_cap_usd"] == pytest.approx(10_000.0)

    assert refresh_net_issuance_values(tmp_store, options) == 2
    assert refresh_net_issuance_values(tmp_store, options) == 2
    factor = tmp_store.con.execute(
        """
        SELECT security_id, raw_value, value, input_lineage_json
        FROM fundamental_factor_values
        WHERE factor_id = ?
        ORDER BY security_id
        """,
        [FACTOR_ID],
    ).df().set_index("security_id")
    assert factor.loc["SPLIT", "raw_value"] == pytest.approx(0.0)
    assert factor.loc["ISSUER", "raw_value"] == pytest.approx(-math.log(1.1))
    assert factor.loc["SPLIT", "value"] > factor.loc["ISSUER", "value"]
    assert "split_index" in factor.loc["SPLIT", "input_lineage_json"]


def test_net_issuance_factor_is_governed_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        """
        SELECT family, direction, is_point_in_time_safe, source
        FROM factor_definition
        WHERE factor_id = ?
        """,
        [FACTOR_ID],
    ).fetchone()
    assert row == (
        "fundamental_financing",
        1,
        True,
        "atx-db PIT net share issuance v1",
    )
