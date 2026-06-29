from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


SECURITY_ID = "SEC-TEST-DELIST"


def _date_value(value) -> dt.date:
    return value.date() if hasattr(value, "date") else value


def _seed_security(tmp_store) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id,
            issuer_id,
            primary_symbol,
            name,
            source
        )
        VALUES (?, ?, ?, ?, ?)
        """,
        [SECURITY_ID, SECURITY_ID, "DLS", "Delist Test Corp.", "test"],
    )


def _insert_listing_status(
    tmp_store,
    *,
    listing_status_id: str,
    status: str,
    valid_from: dt.date,
    valid_to: dt.date | None = None,
    as_of_date: dt.date | None = None,
    available_at: dt.datetime | None = None,
    last_evidence_as_of_date: dt.date | None = None,
    last_evidence_at: dt.datetime | None = None,
    method: str = "trading_system_action_checkpoint",
) -> None:
    as_of_date = as_of_date or valid_from
    available_at = available_at or dt.datetime.combine(as_of_date, dt.time(22, 0))
    last_evidence_as_of_date = last_evidence_as_of_date or as_of_date
    last_evidence_at = last_evidence_at or available_at
    tmp_store.con.execute(
        """
        INSERT INTO listing_status_intervals (
            listing_status_id,
            security_id,
            symbol,
            listing_venue_code,
            listing_venue_name,
            listing_exchange_code,
            status,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            last_evidence_as_of_date,
            last_evidence_at,
            source,
            evidence_source,
            evidence_source_table,
            source_event_id,
            source_snapshot_directory,
            source_url,
            method,
            details_json,
            run_id
        )
        VALUES (
            ?, ?, 'DLS', 'Q', 'NASDAQ Global Select Market', 'NASDAQ',
            ?, ?, ?, ?, ?, ?, ?,
            'test_listing_status',
            'nasdaq_trading_system_adds_deletes',
            'nasdaq_listing_events',
            'event-1',
            NULL,
            'https://example.test/listing-events',
            ?,
            '{}',
            'listing-run'
        )
        """,
        [
            listing_status_id,
            SECURITY_ID,
            status,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            last_evidence_as_of_date,
            last_evidence_at,
            method,
        ],
    )


def test_delisting_events_default_to_unobserved_return_and_are_pit_visible(tmp_store):
    from db.asof import delisting_events_asof
    from db.delisting import refresh_delisting_events

    _seed_security(tmp_store)
    _insert_listing_status(
        tmp_store,
        listing_status_id="inactive-status-1",
        status="inactive",
        valid_from=dt.date(2024, 2, 1),
        as_of_date=dt.date(2024, 2, 1),
        available_at=dt.datetime(2024, 2, 2, 12, 0),
    )

    rows = refresh_delisting_events(tmp_store)
    assert rows == 1

    event = tmp_store.con.execute(
        """
        SELECT
            symbol,
            delist_code,
            delisting_return,
            delisting_return_type,
            is_return_imputed,
            return_policy,
            return_confidence,
            evidence_confidence
        FROM delisting_events
        """
    ).fetchone()
    assert event == (
        "DLS",
        "NASDAQ_DELETE",
        None,
        "UNOBSERVED_PUBLIC_PROXY",
        False,
        "none",
        "none",
        "high",
    )

    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    before_available = delisting_events_asof(
        dt.date(2024, 2, 1),
        as_of_ts=dt.datetime(2024, 2, 1, 23, 0),
        db_path=db_path,
        symbols=("DLS",),
    )
    after_available = delisting_events_asof(
        dt.date(2024, 2, 2),
        as_of_ts=dt.datetime(2024, 2, 2, 13, 0),
        db_path=db_path,
        symbols=("DLS",),
    )

    assert before_available.empty
    assert len(after_available) == 1
    assert _date_value(after_available.iloc[0]["delist_date"]) == dt.date(2024, 2, 1)


def test_delisting_events_apply_imputation_only_when_requested(tmp_store):
    from db.delisting import DelistingEventOptions, refresh_delisting_events

    _seed_security(tmp_store)
    _insert_listing_status(
        tmp_store,
        listing_status_id="inactive-status-2",
        status="inactive",
        valid_from=dt.date(2024, 3, 4),
    )

    refresh_delisting_events(
        tmp_store,
        DelistingEventOptions(apply_shumway_warther_imputation=True),
    )

    row = tmp_store.con.execute(
        """
        SELECT delisting_return, is_return_imputed, return_policy, return_confidence
        FROM delisting_events
        """
    ).fetchone()
    assert row[0] == pytest.approx(-0.30)
    assert row[1:] == (
        True,
        "optional_shumway_warther_unresolved_delete_minus_30pct",
        "low",
    )


def test_delisting_event_dataset_run_returns_load_result(tmp_store):
    from db.delisting import DelistingEventDataset, DelistingEventOptions

    _seed_security(tmp_store)
    _insert_listing_status(
        tmp_store,
        listing_status_id="inactive-status-dataset",
        status="inactive",
        valid_from=dt.date(2024, 3, 5),
    )

    result = DelistingEventDataset().run(tmp_store, DelistingEventOptions())

    assert result.dataset_id == "delisting_events"
    assert result.rows_loaded == 1
    assert result.run_id is not None


def test_snapshot_absence_delisting_proxy_is_opt_in_and_low_confidence(tmp_store):
    from db.delisting import DelistingEventOptions, refresh_delisting_events

    _seed_security(tmp_store)
    _insert_listing_status(
        tmp_store,
        listing_status_id="active-ended-status-1",
        status="active",
        valid_from=dt.date(2024, 1, 1),
        valid_to=dt.date(2024, 1, 5),
        as_of_date=dt.date(2024, 1, 1),
        available_at=dt.datetime(2024, 1, 1, 22, 0),
        last_evidence_as_of_date=dt.date(2024, 1, 4),
        last_evidence_at=dt.datetime(2024, 1, 4, 22, 0),
        method="snapshot_presence_consecutive_days",
    )

    assert refresh_delisting_events(tmp_store) == 0
    assert refresh_delisting_events(
        tmp_store,
        DelistingEventOptions(include_snapshot_absence=True),
    ) == 1

    row = tmp_store.con.execute(
        """
        SELECT delist_code, delisting_return, inferred_from_absence, evidence_confidence
        FROM delisting_events
        """
    ).fetchone()
    assert row == ("SNAPSHOT_ABSENCE", None, True, "low")


def test_observed_delisting_return_observations_enrich_asof_without_lookahead(tmp_store, tmp_path):
    from db.asof import delisting_events_asof, delisting_return_observations_asof
    from db.delisting import (
        DelistingReturnObservationOptions,
        load_delisting_return_observations,
        refresh_delisting_events,
    )

    _seed_security(tmp_store)
    _insert_listing_status(
        tmp_store,
        listing_status_id="inactive-status-observed",
        status="inactive",
        valid_from=dt.date(2024, 4, 1),
        as_of_date=dt.date(2024, 4, 1),
        available_at=dt.datetime(2024, 4, 2, 12, 0),
    )
    refresh_delisting_events(tmp_store)

    csv_path = tmp_path / "crsp_dsedelist_sample.csv"
    csv_path.write_text(
        "\n".join(
            [
                "PERMNO,security_id,TICKER,DLSTDT,DLSTCD,DLRET,DLRETX,DLPRC,DLAMT,available_at",
                "12345,SEC-TEST-DELIST,DLS,2024-04-01,233,0.041667,0.040000,48.0,50.0,2024-04-03T12:00:00",
            ]
        ),
        encoding="utf-8",
    )
    loaded = load_delisting_return_observations(
        tmp_store,
        DelistingReturnObservationOptions(
            source_file=csv_path,
            provider="CRSP_SAMPLE",
        ),
    )
    assert loaded == 1

    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    before_observation = delisting_events_asof(
        dt.date(2024, 4, 2),
        as_of_ts=dt.datetime(2024, 4, 2, 13, 0),
        db_path=db_path,
        symbols=("DLS",),
    )
    after_observation = delisting_events_asof(
        dt.date(2024, 4, 3),
        as_of_ts=dt.datetime(2024, 4, 3, 13, 0),
        db_path=db_path,
        symbols=("DLS",),
    )
    observations = delisting_return_observations_asof(
        dt.date(2024, 4, 3),
        as_of_ts=dt.datetime(2024, 4, 3, 13, 0),
        db_path=db_path,
        symbols=("DLS",),
        providers=("CRSP_SAMPLE",),
    )

    assert len(before_observation) == 1
    assert before_observation.iloc[0]["delisting_return"] is None or pd.isna(
        before_observation.iloc[0]["delisting_return"]
    )
    assert len(after_observation) == 1
    enriched = after_observation.iloc[0]
    assert enriched["delisting_return"] == pytest.approx(0.041667)
    assert enriched["delisting_return_type"] == "OBSERVED_SOURCE"
    assert enriched["is_return_imputed"] == False
    assert enriched["return_policy"] == "observed_source"
    assert enriched["return_observation_provider"] == "CRSP_SAMPLE"
    assert len(observations) == 1
    assert observations.iloc[0]["crsp_dlstcd"] == 233
