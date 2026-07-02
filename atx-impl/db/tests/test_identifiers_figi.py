from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest


# ---------------------------------------------------------------------------
# (a) Pure transform tests -- no DuckDB. compute_figi_alias_rows takes an
# OpenFIGI mapping frame plus an already-resolved cusip -> security_id lookup
# and emits a long alias frame (FIGI + TICKER rows, internal_cusip carried).
# ---------------------------------------------------------------------------


def _mapping_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {"cusip": "037833100", "figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"},
            {"cusip": "594918104", "figi": "BBG000BPH459", "ticker": "MSFT", "name": "MICROSOFT CORP"},
        ]
    )


def _resolution_frame() -> pd.DataFrame:
    # One unambiguous security_id per cusip -- the shape produced by resolving
    # cusip against the existing security_identifier_history CUSIP aliases.
    return pd.DataFrame(
        [
            {"cusip": "037833100", "security_id": "SEC-CIK-0000320193"},
            {"cusip": "594918104", "security_id": "SEC-CIK-0000789019"},
        ]
    )


def test_compute_figi_alias_rows_emits_figi_and_ticker_rows_with_internal_cusip():
    from db.identifiers_figi import compute_figi_alias_rows

    as_of = dt.date(2026, 6, 1)
    available_at = dt.datetime(2026, 6, 1, 22, 0, 0)
    out = compute_figi_alias_rows(
        _mapping_frame(),
        _resolution_frame(),
        source="OpenFIGI",
        run_id="run-abc",
        as_of_date=as_of,
        available_at=available_at,
    )

    assert list(out.columns) >= [
        "security_id",
        "id_type",
        "id_value",
        "internal_cusip",
        "valid_from",
        "valid_to",
        "as_of_date",
        "available_at",
        "source",
        "run_id",
    ]

    figi_rows = out[out["id_type"] == "FIGI"].sort_values("security_id").reset_index(drop=True)
    assert list(figi_rows["security_id"]) == ["SEC-CIK-0000320193", "SEC-CIK-0000789019"]
    assert list(figi_rows["id_value"]) == ["BBG000B9XRY4", "BBG000BPH459"]
    # Source CUSIP is carried on the row via the internal-only column, never as id_value/id_type.
    assert list(figi_rows["internal_cusip"]) == ["037833100", "594918104"]
    assert (figi_rows["source"] == "OpenFIGI").all()
    assert (figi_rows["run_id"] == "run-abc").all()
    assert (figi_rows["valid_from"] == as_of).all()
    assert figi_rows["valid_to"].isna().all()
    assert (figi_rows["available_at"] == available_at).all()

    ticker_rows = out[out["id_type"] == "TICKER"].sort_values("security_id").reset_index(drop=True)
    assert list(ticker_rows["security_id"]) == ["SEC-CIK-0000320193", "SEC-CIK-0000789019"]
    assert list(ticker_rows["id_value"]) == ["AAPL", "MSFT"]
    assert list(ticker_rows["internal_cusip"]) == ["037833100", "594918104"]

    # No id_type/id_value anywhere in the frame carries the raw CUSIP -- it only
    # ever appears in the internal_cusip column.
    assert "CUSIP" not in set(out["id_type"])
    assert "037833100" not in set(out["id_value"])
    assert "594918104" not in set(out["id_value"])


def test_compute_figi_alias_rows_empty_resolution_yields_empty_frame():
    from db.identifiers_figi import compute_figi_alias_rows

    out = compute_figi_alias_rows(
        _mapping_frame(),
        pd.DataFrame(columns=["cusip", "security_id"]),
        source="OpenFIGI",
        run_id=None,
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert out.empty


def test_compute_figi_alias_rows_skips_rows_missing_figi():
    from db.identifiers_figi import compute_figi_alias_rows

    mapping = pd.DataFrame(
        [
            {"cusip": "037833100", "figi": None, "ticker": "AAPL", "name": "APPLE INC"},
            {"cusip": "594918104", "figi": "BBG000BPH459", "ticker": "MSFT", "name": "MICROSOFT CORP"},
        ]
    )
    out = compute_figi_alias_rows(
        mapping,
        _resolution_frame(),
        source="OpenFIGI",
        run_id=None,
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert set(out.loc[out["id_type"] == "FIGI", "security_id"]) == {"SEC-CIK-0000789019"}


def test_compute_figi_alias_rows_deterministic_same_inputs_same_rows():
    from db.identifiers_figi import compute_figi_alias_rows

    kwargs = dict(
        source="OpenFIGI",
        run_id="run-abc",
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    first = compute_figi_alias_rows(_mapping_frame(), _resolution_frame(), **kwargs)
    second = compute_figi_alias_rows(_mapping_frame(), _resolution_frame(), **kwargs)
    pd.testing.assert_frame_equal(
        first.sort_values(["id_type", "security_id"]).reset_index(drop=True),
        second.sort_values(["id_type", "security_id"]).reset_index(drop=True),
    )


# ---------------------------------------------------------------------------
# Injectable file parsing -- CSV and OpenFIGI-JSON-shaped bulk dump.
# ---------------------------------------------------------------------------


def test_parse_openfigi_file_reads_csv(tmp_path):
    from db.identifiers_figi import parse_openfigi_file

    path = tmp_path / "figi.csv"
    path.write_text(
        "cusip,figi,ticker,name\n"
        "037833100,BBG000B9XRY4,AAPL,APPLE INC\n"
        "594918104,BBG000BPH459,MSFT,MICROSOFT CORP\n",
        encoding="utf-8",
    )
    frame = parse_openfigi_file(path)
    assert set(frame["cusip"]) == {"037833100", "594918104"}
    assert set(frame["figi"]) == {"BBG000B9XRY4", "BBG000BPH459"}


def test_parse_openfigi_file_reads_mapping_v3_json_shape(tmp_path):
    """POST /v3/mapping returns one {"data": [...]} or {"error": ...} per request row,
    positionally aligned to the request idValue list."""
    from db.identifiers_figi import parse_openfigi_file

    path = tmp_path / "figi.json"
    payload = {
        "requests": [
            {"idType": "ID_CUSIP", "idValue": "037833100"},
            {"idType": "ID_CUSIP", "idValue": "999999999"},
        ],
        "responses": [
            {"data": [{"figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"}]},
            {"error": "No identifier found."},
        ],
    }
    path.write_text(json.dumps(payload), encoding="utf-8")
    frame = parse_openfigi_file(path)
    assert list(frame["cusip"]) == ["037833100"]
    assert list(frame["figi"]) == ["BBG000B9XRY4"]


def test_parse_openfigi_file_reads_flat_json_array(tmp_path):
    from db.identifiers_figi import parse_openfigi_file

    path = tmp_path / "figi_flat.json"
    payload = [
        {"cusip": "037833100", "figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"},
        {"cusip": "594918104", "figi": "BBG000BPH459", "ticker": "MSFT", "name": "MICROSOFT CORP"},
    ]
    path.write_text(json.dumps(payload), encoding="utf-8")
    frame = parse_openfigi_file(path)
    assert set(frame["cusip"]) == {"037833100", "594918104"}


def test_parse_openfigi_file_rejects_unknown_extension(tmp_path):
    from db.identifiers_figi import parse_openfigi_file

    path = tmp_path / "figi.txt"
    path.write_text("nope", encoding="utf-8")
    with pytest.raises(ValueError):
        parse_openfigi_file(path)


# ---------------------------------------------------------------------------
# (b) DuckDB integration via tmp_store.
# ---------------------------------------------------------------------------


def _seed_securities(store) -> None:
    store.con.execute(
        """
        INSERT INTO securities (
            security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES
            ('SEC-CIK-0000320193', 'CIK-0000320193', 'CIK-0000320193', 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-05-04', NULL, 'fixture'),
            ('SEC-CIK-0000789019', 'CIK-0000789019', 'CIK-0000789019', 'MSFT', 'Microsoft Corp.', 'EQUITY', 'US', 'USD', true, DATE '2021-02-03', NULL, 'fixture'),
            ('SEC-CIK-0000000099', 'CIK-0000000099', 'CIK-0000000099', 'DUP', 'Duplicate Holder Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-01-01', NULL, 'fixture')
        """
    )
    store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES
            ('SEC-CIK-0000320193', 'CUSIP', '037833100', DATE '2019-05-04', NULL, DATE '2019-05-04', TIMESTAMP '2019-05-05 12:00:00', 'fixture', NULL),
            ('SEC-CIK-0000789019', 'CUSIP', '594918104', DATE '2021-02-03', NULL, DATE '2021-02-03', TIMESTAMP '2021-02-04 12:00:00', 'fixture', NULL),
            -- conflicting CUSIP: two distinct securities both claim this CUSIP
            ('SEC-CIK-0000000099', 'CUSIP', '111111108', DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL),
            ('SEC-CIK-0000320193', 'CUSIP', '111111108', DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL)
        """
    )


def _write_figi_file(tmp_path, rows: list[dict]) -> "Path":
    import csv

    path = tmp_path / "figi_mapping.csv"
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["cusip", "figi", "ticker", "name"])
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return path


def test_figi_alias_dataset_load_attaches_figi_and_keeps_cusip_internal_only(tmp_store):
    from db.identifiers_figi import FigiAliasDataset, FigiLoadOptions

    _seed_securities(tmp_store)
    figi_file = _write_figi_file(
        tmp_store.path.parent,
        [
            {"cusip": "037833100", "figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"},
            {"cusip": "594918104", "figi": "BBG000BPH459", "ticker": "MSFT", "name": "MICROSOFT CORP"},
        ],
    )

    dataset = FigiAliasDataset()
    result = dataset.run(tmp_store, FigiLoadOptions(figi_file=figi_file))

    assert result.rows_loaded > 0

    figi_rows = tmp_store.con.execute(
        """
        SELECT security_id, id_value, internal_cusip, source
        FROM security_identifier_history
        WHERE id_type = 'FIGI'
        ORDER BY security_id
        """
    ).fetchall()
    assert figi_rows == [
        ("SEC-CIK-0000320193", "BBG000B9XRY4", "037833100", "OpenFIGI"),
        ("SEC-CIK-0000789019", "BBG000BPH459", "594918104", "OpenFIGI"),
    ]

    # CUSIP never lands as an id_type/id_value pair written by this loader.
    figi_source_cusip_rows = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'CUSIP' AND source = 'OpenFIGI'"
    ).fetchone()[0]
    assert figi_source_cusip_rows == 0


def test_figi_alias_dataset_conflict_writes_candidate_and_decision_not_merge(tmp_store):
    from db.identifiers_figi import FigiAliasDataset, FigiLoadOptions

    _seed_securities(tmp_store)
    figi_file = _write_figi_file(
        tmp_store.path.parent,
        [
            # This CUSIP resolves to two distinct securities in the seed fixture above --
            # ambiguous, must NOT be blindly merged into a FIGI alias row.
            {"cusip": "111111108", "figi": "BBG000CONFLICT", "ticker": "DUP", "name": "Duplicate Holder Inc."},
        ],
    )

    dataset = FigiAliasDataset()
    dataset.run(tmp_store, FigiLoadOptions(figi_file=figi_file))

    figi_rows = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'FIGI' AND id_value = 'BBG000CONFLICT'"
    ).fetchone()[0]
    assert figi_rows == 0

    candidates = tmp_store.con.execute(
        """
        SELECT source_key_type, source_key_value, target_id_type, target_id_value, match_method, confidence
        FROM identifier_resolution_candidates
        WHERE source_key_value = '111111108'
        """
    ).fetchall()
    assert len(candidates) == 2
    for row in candidates:
        assert row[0] == "CUSIP"
        assert row[1] == "111111108"
        assert row[2] == "FIGI"
        assert row[3] == "BBG000CONFLICT"
        assert row[4] == "openfigi_cusip_mapping"
        assert row[5] < 1.0

    decisions = tmp_store.con.execute(
        """
        SELECT decision_status, source_key_value
        FROM identifier_resolution_decisions
        WHERE source_key_value = '111111108'
        """
    ).fetchall()
    assert len(decisions) == 2
    assert all(status == "needs_review" for status, _ in decisions)


def test_figi_alias_dataset_unmatched_cusip_writes_candidate_not_error(tmp_store):
    """A cusip with no existing CUSIP alias in the spine at all (never seen before)
    must not crash the loader and must not silently vanish -- it is recorded as a
    'proposed' resolution candidate (no target_security_id known yet) rather than
    merged or dropped."""
    from db.identifiers_figi import FigiAliasDataset, FigiLoadOptions

    _seed_securities(tmp_store)
    figi_file = _write_figi_file(
        tmp_store.path.parent,
        [
            {"cusip": "000000000", "figi": "BBG000UNKNOWN", "ticker": "ZZZ", "name": "Unknown Co"},
        ],
    )

    dataset = FigiAliasDataset()
    result = dataset.run(tmp_store, FigiLoadOptions(figi_file=figi_file))

    assert result.rows_loaded == 0
    candidate = tmp_store.con.execute(
        """
        SELECT source_security_id, candidate_status
        FROM identifier_resolution_candidates
        WHERE source_key_value = '000000000'
        """
    ).fetchone()
    assert candidate == (None, "proposed")

    figi_count = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'FIGI'"
    ).fetchone()[0]
    assert figi_count == 0

    figi_count = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'FIGI'"
    ).fetchone()[0]
    assert figi_count == 0


def test_figi_alias_dataset_no_network_call_offline_only(tmp_store, monkeypatch):
    """Guard: the dataset must never construct a requests session / hit the network."""
    import db.identifiers_figi as identifiers_figi

    _seed_securities(tmp_store)
    figi_file = _write_figi_file(
        tmp_store.path.parent,
        [{"cusip": "037833100", "figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"}],
    )

    if hasattr(identifiers_figi, "requests"):
        def _boom(*_args, **_kwargs):
            raise AssertionError("identifiers_figi must not perform network requests")

        monkeypatch.setattr(identifiers_figi.requests, "get", _boom, raising=False)
        monkeypatch.setattr(identifiers_figi.requests, "post", _boom, raising=False)

    from db.identifiers_figi import FigiAliasDataset, FigiLoadOptions

    dataset = FigiAliasDataset()
    result = dataset.run(tmp_store, FigiLoadOptions(figi_file=figi_file))
    assert result.rows_loaded > 0
