from __future__ import annotations

import datetime as dt
import subprocess
import sys
from pathlib import Path

import pandas as pd

from db.asof import fundamental_ratios_asof_month, pit_snapshot_asof
from db.fundamental_ratios import FundamentalRatiosOptions, refresh_fundamental_ratios
from db.pit_snapshot import PitSnapshotOptions, compute_pit_snapshot_rows, refresh_pit_snapshot


SID = "SEC-CIK-0000320193"
SYMBOL = "AAPL"
CIK = "0000320193"
PERIOD_END = dt.date(2025, 12, 31)
PERIOD_START = dt.date(2025, 1, 1)


def _candidate(
    metric: str,
    value: float,
    *,
    accession: str,
    filed_date: dt.date,
    available_at: dt.datetime,
    item_id: int | None = None,
    basis: str = "annual",
) -> dict:
    return {
        "upstream_source": "fixture",
        "security_id": SID,
        "symbol": SYMBOL,
        "cik": CIK,
        "canonical_metric": metric,
        "item_id": item_id,
        "basis": basis,
        "period_start": PERIOD_START if basis != "instant" else None,
        "period_end": PERIOD_END,
        "value": value,
        "unit": "USD",
        "unit_type": "monetary",
        "source_accession": accession,
        "filed_date": filed_date,
        "available_at": available_at,
    }


def _insert_statement(
    store,
    *,
    metric: str,
    value: float,
    accession: str,
    filed_date: dt.date,
    available_at: dt.datetime,
    latest: bool,
    item_id: int | None = None,
) -> None:
    cols = {
        "statement_point_id": f"{SID}|{metric}|{PERIOD_END}|{accession}",
        "fact_revision_id": f"fr-{metric}-{accession}",
        "revision_group_id": f"rg-{metric}",
        "source": "fixture_statement",
        "security_id": SID,
        "symbol": SYMBOL,
        "cik": CIK,
        "statement_type": "income_statement",
        "statement_section": "profitability",
        "canonical_metric": metric,
        "canonical_label": metric,
        "taxonomy": "us-gaap",
        "concept": metric,
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "duration",
        "normal_balance": "credit",
        "period_start": PERIOD_START,
        "period_end": PERIOD_END,
        "as_of_date": filed_date,
        "available_at": available_at,
        "fiscal_year": 2025,
        "fiscal_period": "FY",
        "accession_number": accession,
        "source_accession": accession,
        "filed_date": filed_date,
        "revision_sequence": 1 if latest else 0,
        "revision_count": 2,
        "is_latest_revision": latest,
        "is_value_changed": not latest,
        "value": value,
        "item_id": item_id,
        "source_url": f"https://example.invalid/{accession}",
        "source_loaded_at": available_at,
        "updated_at": available_at,
    }
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_statement_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_ttm(
    store,
    *,
    metric: str,
    value: float,
    accession: str,
    filed_date: dt.date,
    available_at: dt.datetime,
    latest: bool,
) -> None:
    cols = {
        "ttm_point_id": f"{SID}|{metric}|{PERIOD_END}|{accession}",
        "ttm_revision_group_id": f"ttm-rg-{metric}",
        "anchor_statement_point_id": f"anchor-{metric}-{accession}",
        "source": "fixture_ttm",
        "security_id": SID,
        "symbol": SYMBOL,
        "cik": CIK,
        "statement_type": "income_statement",
        "statement_section": "profitability",
        "canonical_metric": metric,
        "canonical_label": metric,
        "unit": "USD",
        "unit_type": "monetary",
        "ttm_start_date": PERIOD_START,
        "ttm_end_date": PERIOD_END,
        "as_of_date": filed_date,
        "available_at": available_at,
        "fiscal_year": 2025,
        "fiscal_period": "FY",
        "accession_number": accession,
        "quarter_count": 4,
        "coverage_days": 365,
        "input_statement_point_ids_json": "[]",
        "input_accessions_json": "[]",
        "input_period_ends_json": "[]",
        "ttm_value": value,
        "revision_sequence": 1 if latest else 0,
        "revision_count": 2,
        "is_latest_revision": latest,
        "is_value_changed": not latest,
        "calculation_method": "sum_4q",
        "source_loaded_at": available_at,
        "updated_at": available_at,
    }
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_ttm_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _seed_statement_restatement(store) -> None:
    _insert_statement(
        store,
        metric="revenue",
        value=100.0,
        accession="0001-original",
        filed_date=dt.date(2026, 2, 15),
        available_at=dt.datetime(2026, 2, 15, 21, 30),
        latest=False,
        item_id=1001,
    )
    _insert_statement(
        store,
        metric="revenue",
        value=120.0,
        accession="0002-restated",
        filed_date=dt.date(2026, 4, 10),
        available_at=dt.datetime(2026, 4, 10, 21, 30),
        latest=True,
        item_id=1001,
    )


def _seed_ttm_restatement(store) -> None:
    for metric, original, restated in (
        ("revenue", 100.0, 120.0),
        ("net_income", 10.0, 15.0),
    ):
        _insert_ttm(
            store,
            metric=metric,
            value=original,
            accession="0001-original",
            filed_date=dt.date(2026, 2, 15),
            available_at=dt.datetime(2026, 2, 15, 21, 30),
            latest=False,
        )
        _insert_ttm(
            store,
            metric=metric,
            value=restated,
            accession="0002-restated",
            filed_date=dt.date(2026, 4, 10),
            available_at=dt.datetime(2026, 4, 10, 21, 30),
            latest=True,
        )


def test_compute_pit_snapshot_rows_selects_visible_month_vintage():
    inputs = pd.DataFrame(
        [
            _candidate(
                "revenue",
                100.0,
                accession="0001-original",
                filed_date=dt.date(2026, 2, 15),
                available_at=dt.datetime(2026, 2, 15, 21, 30),
                item_id=1001,
            ),
            _candidate(
                "revenue",
                120.0,
                accession="0002-restated",
                filed_date=dt.date(2026, 4, 10),
                available_at=dt.datetime(2026, 4, 10, 21, 30),
                item_id=1001,
            ),
            _candidate(
                "net_income",
                9.0,
                accession="0003-late",
                filed_date=dt.date(2026, 3, 5),
                available_at=dt.datetime(2026, 3, 5, 21, 30),
                item_id=1002,
            ),
        ]
    )

    feb = compute_pit_snapshot_rows(inputs, snapshot_month=dt.date(2026, 2, 1))
    assert list(feb["canonical_metric"]) == ["revenue"]
    assert feb.iloc[0]["value"] == 100.0
    assert feb.iloc[0]["source_accession"] == "0001-original"
    assert feb.iloc[0]["vintage_class"] == "as_first_reported"

    april = compute_pit_snapshot_rows(inputs, snapshot_month=dt.date(2026, 4, 1))
    revenue = april.loc[april["canonical_metric"] == "revenue"].iloc[0]
    assert revenue["value"] == 120.0
    assert revenue["source_accession"] == "0002-restated"
    assert revenue["vintage_class"] == "most_recently_restated"


def test_migrations_0107_0109_catalog_pit_snapshot_surface(tmp_store):
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT version FROM schema_migrations WHERE version IN ('0107', '0108', '0109')"
        ).fetchall()
    }
    assert versions == {"0107", "0108", "0109"}

    ratio_cols = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'fundamental_ratios'"
        ).fetchall()
    }
    snapshot_cols = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'fundamental_pit_snapshot'"
        ).fetchall()
    }

    assert "vintage_class" in ratio_cols
    assert {"snapshot_month", "vintage_class", "source_accession", "available_at"} <= snapshot_cols
    assert tmp_store.con.execute(
        "SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'fundamental_pit_snapshot'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE table_name = 'fundamental_ratios'
          AND field_name = 'vintage_class'
        """
    ).fetchone()[0] == 1
    indexes = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT index_name FROM duckdb_indexes() WHERE table_name = 'fundamental_pit_snapshot'"
        ).fetchall()
    }
    assert "idx_fundamental_pit_snapshot_month" in indexes


def test_refresh_pit_snapshot_materializes_month_boundaries(tmp_store):
    _seed_statement_restatement(tmp_store)

    feb_rows = refresh_pit_snapshot(tmp_store, PitSnapshotOptions(snapshot_month=dt.date(2026, 2, 1)))
    april_rows = refresh_pit_snapshot(tmp_store, PitSnapshotOptions(snapshot_month=dt.date(2026, 4, 1)))

    assert feb_rows == 1
    assert april_rows == 1
    feb = pit_snapshot_asof(dt.date(2026, 2, 28), store=tmp_store, symbols=[SYMBOL], metrics=["revenue"])
    april = pit_snapshot_asof(dt.date(2026, 4, 30), store=tmp_store, symbols=[SYMBOL], metrics=["revenue"])

    assert feb.iloc[0]["value"] == 100.0
    assert feb.iloc[0]["source_accession"] == "0001-original"
    assert april.iloc[0]["value"] == 120.0
    assert april.iloc[0]["source_accession"] == "0002-restated"
    assert april.iloc[0]["vintage_class"] == "most_recently_restated"


def test_ratio_history_materializes_and_reads_month_vintages(tmp_store):
    _seed_ttm_restatement(tmp_store)

    rows = refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions(vintage_mode="history"))
    assert rows > 0

    history = tmp_store.con.execute(
        """
        SELECT source_accession, value, vintage_class, is_latest_revision
        FROM fundamental_ratios
        WHERE ratio_code = 'net_profit_margin'
        ORDER BY filed_date
        """
    ).df()
    assert list(history["source_accession"]) == ["0001-original", "0002-restated"]
    assert list(history["vintage_class"]) == ["as_first_reported", "most_recently_restated"]
    assert list(history["is_latest_revision"]) == [False, True]

    feb = fundamental_ratios_asof_month(
        dt.date(2026, 2, 1),
        store=tmp_store,
        symbols=[SYMBOL],
        ratio_codes=["net_profit_margin"],
    )
    april = fundamental_ratios_asof_month(
        dt.date(2026, 4, 1),
        store=tmp_store,
        symbols=[SYMBOL],
        ratio_codes=["net_profit_margin"],
    )

    assert feb.iloc[0]["source_accession"] == "0001-original"
    assert feb.iloc[0]["value"] == 0.1
    assert april.iloc[0]["source_accession"] == "0002-restated"
    assert april.iloc[0]["value"] == 0.125


def test_query_asof_cli_reads_pit_snapshot_view(tmp_store):
    _seed_statement_restatement(tmp_store)
    refresh_pit_snapshot(tmp_store, PitSnapshotOptions(snapshot_month=dt.date(2026, 4, 1)))

    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    root = Path(__file__).resolve().parents[2]
    result = subprocess.run(
        [
            sys.executable,
            str(root / "scripts" / "query_asof.py"),
            "--db-path",
            str(db_path),
            "--as-of-date",
            "2026-04-30",
            "--view",
            "pit-snapshot",
            "--symbols",
            SYMBOL,
            "--metrics",
            "revenue",
        ],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    )
    assert SYMBOL in result.stdout
    assert "0002-restated" in result.stdout
