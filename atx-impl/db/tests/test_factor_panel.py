from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from db.factor_panel import assemble_factor_panel_long, pivot_factor_panel_wide


def _factor_row(
    surface: str,
    security_id: str,
    factor_id: str,
    value: float,
    *,
    as_of_date: str,
    available_at: str,
    source_loaded_at: str = "2024-02-01 22:00:00",
) -> dict[str, object]:
    row = {
        "factor_value_id": f"{surface}-{security_id}-{factor_id}",
        "factor_id": factor_id,
        "factor_name": factor_id.replace("_", " ").title(),
        "family": "fixture_family",
        "security_id": security_id,
        "symbol": security_id.replace("SEC-", ""),
        "as_of_date": pd.Timestamp(as_of_date).date(),
        "raw_value": value,
        "value": value,
        "available_at": pd.Timestamp(available_at),
        "input_ids_json": "[]",
        "input_lineage_json": json.dumps([{"surface": surface, "available_at": available_at}]),
        "is_latest_revision": True,
        "run_id": "panel-fixture",
        "source": "pytest",
        "source_loaded_at": pd.Timestamp(source_loaded_at),
    }
    if surface == "cross":
        row["domain"] = "price_liquidity"
        row["source_row_id"] = f"source-{security_id}-{factor_id}"
    return row


def _membership(
    security_id: str,
    *,
    is_member: bool = True,
    valid_from: str = "2024-01-01",
    valid_to: str | None = None,
    available_at: str = "2024-01-01 09:30:00",
) -> dict[str, object]:
    return {
        "universe_id": "us_common_equity_liquid_v1",
        "security_id": security_id,
        "symbol": security_id.replace("SEC-", ""),
        "valid_from": pd.Timestamp(valid_from).date(),
        "valid_to": pd.Timestamp(valid_to).date() if valid_to else None,
        "as_of_date": pd.Timestamp(valid_from).date(),
        "is_member": is_member,
        "reason": "member" if is_member else "liquidity_screen_fail",
        "rules_json": "{}",
        "decision_count": 1,
        "available_at": pd.Timestamp(available_at),
        "source": "pytest",
        "run_id": "universe-fixture",
        "is_latest_revision": True,
        "source_loaded_at": pd.Timestamp("2024-01-01 09:31:00"),
    }


def test_assemble_factor_panel_long_uses_decision_dates_and_universe_membership() -> None:
    fundamental = pd.DataFrame(
        [
            _factor_row(
                "fundamental",
                "SEC-A",
                "profitability_gross_profitability",
                0.8,
                as_of_date="2023-12-31",
                available_at="2024-02-01 10:00:00",
            ),
            _factor_row(
                "fundamental",
                "SEC-B",
                "profitability_gross_profitability",
                0.1,
                as_of_date="2023-12-31",
                available_at="2024-02-01 10:00:00",
            ),
        ]
    )
    cross_domain = pd.DataFrame(
        [
            _factor_row(
                "cross",
                "SEC-A",
                "price_momentum_21d",
                1.0,
                as_of_date="2024-02-01",
                available_at="2024-02-01 21:00:00",
            ),
            _factor_row(
                "cross",
                "SEC-C",
                "price_momentum_21d",
                0.5,
                as_of_date="2024-02-01",
                available_at="2024-02-02 09:00:00",
            ),
        ]
    )
    membership = pd.DataFrame(
        [
            _membership("SEC-A"),
            _membership("SEC-B", is_member=False),
            _membership("SEC-C"),
        ]
    )

    panel = assemble_factor_panel_long(
        fundamental,
        cross_domain,
        universe_membership=membership,
        as_of_date=dt.date(2024, 2, 1),
    )

    assert list(panel["security_id"].unique()) == ["SEC-A"]
    assert set(panel["factor_id"]) == {"profitability_gross_profitability", "price_momentum_21d"}
    assert set(panel["as_of_date"]) == {dt.date(2024, 2, 1)}
    assert all(pd.to_datetime(panel["available_at"]).dt.date <= panel["as_of_date"])

    wide = pivot_factor_panel_wide(panel)
    assert list(wide.columns) == [
        "security_id",
        "as_of_date",
        "price_momentum_21d",
        "profitability_gross_profitability",
    ]
    assert wide.loc[0, "price_momentum_21d"] == 1.0
    assert wide.loc[0, "profitability_gross_profitability"] == 0.8


def test_factor_panel_views_resolve_long_and_wide_with_pit_universe_filter(tmp_store) -> None:
    fundamental = _factor_row(
        "fundamental",
        "SEC-A",
        "profitability_gross_profitability",
        0.8,
        as_of_date="2023-12-31",
        available_at="2024-02-01 10:00:00",
    )
    cross = _factor_row(
        "cross",
        "SEC-A",
        "price_momentum_21d",
        1.0,
        as_of_date="2024-02-01",
        available_at="2024-02-01 21:00:00",
    )
    non_member = _factor_row(
        "cross",
        "SEC-B",
        "price_momentum_21d",
        0.2,
        as_of_date="2024-02-01",
        available_at="2024-02-01 21:00:00",
    )
    future_visible = _factor_row(
        "cross",
        "SEC-C",
        "price_momentum_21d",
        0.5,
        as_of_date="2024-02-01",
        available_at="2024-02-02 09:00:00",
    )

    tmp_store.con.execute(
        """
        INSERT INTO fundamental_factor_values (
            factor_value_id, factor_id, factor_name, family, security_id, symbol,
            as_of_date, raw_value, value, available_at, input_ids_json,
            input_lineage_json, is_latest_revision, run_id, source, source_loaded_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            fundamental["factor_value_id"],
            fundamental["factor_id"],
            fundamental["factor_name"],
            fundamental["family"],
            fundamental["security_id"],
            fundamental["symbol"],
            fundamental["as_of_date"],
            fundamental["raw_value"],
            fundamental["value"],
            fundamental["available_at"],
            fundamental["input_ids_json"],
            fundamental["input_lineage_json"],
            fundamental["is_latest_revision"],
            fundamental["run_id"],
            fundamental["source"],
            fundamental["source_loaded_at"],
        ],
    )
    for row in (cross, non_member, future_visible):
        tmp_store.con.execute(
            """
            INSERT INTO cross_domain_factor_values (
                factor_value_id, factor_id, factor_name, domain, family, security_id, symbol,
                as_of_date, raw_value, value, available_at, source_row_id, input_ids_json,
                input_lineage_json, is_latest_revision, run_id, source, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                row["factor_value_id"],
                row["factor_id"],
                row["factor_name"],
                row["domain"],
                row["family"],
                row["security_id"],
                row["symbol"],
                row["as_of_date"],
                row["raw_value"],
                row["value"],
                row["available_at"],
                row["source_row_id"],
                row["input_ids_json"],
                row["input_lineage_json"],
                row["is_latest_revision"],
                row["run_id"],
                row["source"],
                row["source_loaded_at"],
            ],
        )
    for row in (_membership("SEC-A"), _membership("SEC-B", is_member=False), _membership("SEC-C")):
        tmp_store.con.execute(
            """
            INSERT INTO universe_membership (
                universe_id, security_id, symbol, valid_from, valid_to, as_of_date,
                is_member, reason, rules_json, decision_count, available_at, source,
                run_id, is_latest_revision, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                row["universe_id"],
                row["security_id"],
                row["symbol"],
                row["valid_from"],
                row["valid_to"],
                row["as_of_date"],
                row["is_member"],
                row["reason"],
                row["rules_json"],
                row["decision_count"],
                row["available_at"],
                row["source"],
                row["run_id"],
                row["is_latest_revision"],
                row["source_loaded_at"],
            ],
        )

    long_rows = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_id, value, CAST(available_at AS DATE) AS available_date
        FROM v_factor_panel
        WHERE as_of_date = DATE '2024-02-01'
        ORDER BY security_id, factor_id
        """
    ).fetchall()

    assert long_rows == [
        ("SEC-A", dt.date(2024, 2, 1), "price_momentum_21d", 1.0, dt.date(2024, 2, 1)),
        ("SEC-A", dt.date(2024, 2, 1), "profitability_gross_profitability", 0.8, dt.date(2024, 2, 1)),
    ]
    all_rows = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_id, CAST(available_at AS DATE) AS available_date
        FROM v_factor_panel
        ORDER BY security_id, as_of_date, factor_id
        """
    ).fetchall()
    assert ("SEC-C", dt.date(2024, 2, 2), "price_momentum_21d", dt.date(2024, 2, 2)) in all_rows
    assert all(available_date <= as_of_date for _, as_of_date, _, available_date in all_rows)
    assert not any(row[0] == "SEC-B" for row in all_rows)

    wide_row = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_values_json, factor_count
        FROM v_factor_panel_wide
        WHERE security_id = 'SEC-A'
          AND as_of_date = DATE '2024-02-01'
        """
    ).fetchone()
    assert wide_row[0:2] == ("SEC-A", dt.date(2024, 2, 1))
    assert json.loads(wide_row[2]) == {
        "price_momentum_21d": 1.0,
        "profitability_gross_profitability": 0.8,
    }
    assert wide_row[3] == 2

    catalog_counts = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'factor_panel'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'v_factor_panel'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'v_factor_panel_wide')
        """
    ).fetchone()
    assert catalog_counts == (1, 1, 1)
