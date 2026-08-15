from __future__ import annotations

import datetime as dt

from atx_db.api.catalog import RESTATEMENTS_SCHEMA, get_schema


def _insert_standardized_revision(
    store,
    *,
    standardized_id: str,
    revision_group_id: str,
    revision_sequence: int,
    revision_count: int,
    value: float,
    previous_value: float | None,
    is_value_changed: bool,
    is_latest_revision: bool,
    update_type: str,
    accession: str,
    available_at: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik,
            item_id, canonical_code, basis, period_start, period_end,
            fiscal_year, fiscal_period, value, unit_type, source_accession,
            filed_date, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, is_latest_revision,
            run_id, source_loaded_at, updated_at, unit, revision_group_id,
            revision_sequence, revision_count, is_value_changed, previous_value,
            value_delta, value_delta_percent, update_type, valid_to
        ) VALUES (
            ?, 'sec_company_facts', 'sec_company_facts', 'SEC-CIK-0000000001',
            'ALPH', '0000000001', 1224, 'atx.test.item', 'annual',
            DATE '2023-01-01', DATE '2023-12-31', 2023, 'FY', ?, 'currency', ?,
            DATE '2024-02-01', DATE '2023-12-31', ?, '[]', '[]', 'rule-1',
            'single', ?, 'restatement-test', now(), now(), 'USD', ?, ?, ?, ?, ?, ?,
            ?, ?, NULL
        )
        """,
        [
            standardized_id,
            value,
            accession,
            available_at,
            is_latest_revision,
            revision_group_id,
            revision_sequence,
            revision_count,
            is_value_changed,
            previous_value,
            None if previous_value is None else value - previous_value,
            None if previous_value in (None, 0) else (value - previous_value) / abs(previous_value),
            update_type,
        ],
    )


def _seed_restatement_chain(store) -> None:
    _insert_standardized_revision(
        store,
        standardized_id="std-1",
        revision_group_id="chain-1",
        revision_sequence=1,
        revision_count=3,
        value=100.0,
        previous_value=None,
        is_value_changed=False,
        is_latest_revision=False,
        update_type="original",
        accession="0000000001-24-000001",
        available_at=dt.datetime(2024, 2, 1, 16, 30),
    )
    _insert_standardized_revision(
        store,
        standardized_id="std-2",
        revision_group_id="chain-1",
        revision_sequence=2,
        revision_count=3,
        value=110.0,
        previous_value=100.0,
        is_value_changed=True,
        is_latest_revision=False,
        update_type="restated",
        accession="0000000001-25-000001",
        available_at=dt.datetime(2025, 2, 1, 16, 30),
    )
    # A later revision that repeats the value is not a restatement event.
    _insert_standardized_revision(
        store,
        standardized_id="std-3",
        revision_group_id="chain-1",
        revision_sequence=3,
        revision_count=3,
        value=110.0,
        previous_value=110.0,
        is_value_changed=False,
        is_latest_revision=True,
        update_type="restated",
        accession="0000000001-26-000001",
        available_at=dt.datetime(2026, 2, 1, 16, 30),
    )
    # A single-revision chain never emits an event.
    _insert_standardized_revision(
        store,
        standardized_id="std-4",
        revision_group_id="chain-2",
        revision_sequence=1,
        revision_count=1,
        value=50.0,
        previous_value=None,
        is_value_changed=False,
        is_latest_revision=True,
        update_type="original",
        accession="0000000001-24-000002",
        available_at=dt.datetime(2024, 2, 1, 16, 30),
    )


def test_restatement_events_view_emits_only_value_changing_revisions(tmp_store) -> None:
    _seed_restatement_chain(tmp_store)

    rows = tmp_store.con.execute(
        """
        SELECT restatement_event_id, revision_group_id, revision_sequence,
               restated_value, previous_value, first_reported_value,
               value_delta, cumulative_delta, restating_accession,
               previous_accession, update_type
        FROM v_fundamental_restatement_events
        ORDER BY revision_group_id, revision_sequence
        """
    ).fetchall()

    assert len(rows) == 1
    event = rows[0]
    assert event[0] == "std-2"
    assert event[1] == "chain-1"
    assert event[2] == 2
    assert event[3] == 110.0
    assert event[4] == 100.0
    assert event[5] == 100.0
    assert event[6] == 10.0
    assert event[7] == 10.0
    assert event[8] == "0000000001-25-000001"
    assert event[9] == "0000000001-24-000001"
    assert event[10] == "restated"


def test_restatement_events_view_carries_pit_columns(tmp_store) -> None:
    _seed_restatement_chain(tmp_store)

    row = tmp_store.con.execute(
        """
        SELECT available_at, previous_available_at, as_of_date,
               source_loaded_at, run_id
        FROM v_fundamental_restatement_events
        """
    ).fetchone()

    assert row[0] == dt.datetime(2025, 2, 1, 16, 30)
    assert row[1] == dt.datetime(2024, 2, 1, 16, 30)
    assert row[2] == dt.date(2023, 12, 31)
    assert row[3] is not None
    assert row[4] == "restatement-test"


def test_restatements_schema_is_registered_and_matches_view(tmp_store) -> None:
    schema = get_schema("ATX.US.FUNDAMENTALS", "restatements")
    assert schema is RESTATEMENTS_SCHEMA
    assert schema.source_table == "v_fundamental_restatement_events"
    # Every declared source column must exist on the view.
    view_columns = {
        row[1]
        for row in tmp_store.con.execute(
            "PRAGMA table_info('v_fundamental_restatement_events')"
        ).fetchall()
    }
    missing = [
        field.source_column
        for field in schema.fields
        if field.source_column not in view_columns
    ]
    assert missing == []
    # Events are natural-key unique per revision, so PIT vintage selection
    # never collapses distinct restatement events.
    assert schema.natural_key == ("revision_group_id", "revision_sequence")


def test_restatements_schema_is_catalogued_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        """
        SELECT schema_version, source_table, schema_sha256
        FROM api_schema_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='restatements'
        """
    ).fetchone()
    assert row is not None
    assert row[1] == "v_fundamental_restatement_events"
    assert row[2] is not None and len(row[2]) == 64
