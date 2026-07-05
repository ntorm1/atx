from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


# ---------------------------------------------------------------------------
# (a) Pure transform tests -- no DuckDB. compute_lei_alias_rows takes a parsed
# GLEIF Level-1 frame plus an already-resolved entity_id -> [security_id]
# lookup and emits a long security_identifier_history alias frame (id_type='LEI'),
# one row per matched security_id under that entity.
# ---------------------------------------------------------------------------


def _gleif_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "lei": "HWUPKR0MPOU8FGXBT394",
                "legal_name": "APPLE INC",
                "cik": "0000320193",
            },
            {
                "lei": "549300OVFHYPPCFHF059",
                "legal_name": "MICROSOFT CORP",
                "cik": "0000789019",
            },
        ]
    )


def _security_lookup_frame() -> pd.DataFrame:
    # entity_id -> security_id, the shape produced by resolving cik -> entity_id
    # -> securities currently under that entity.
    return pd.DataFrame(
        [
            {"entity_id": "CIK-0000320193", "security_id": "SEC-CIK-0000320193"},
            {"entity_id": "CIK-0000789019", "security_id": "SEC-CIK-0000789019"},
        ]
    )


def test_compute_lei_alias_rows_emits_lei_rows_for_matched_entities():
    from db.identifiers_lei import compute_lei_alias_rows

    as_of = dt.date(2026, 6, 1)
    available_at = dt.datetime(2026, 6, 1, 22, 0, 0)
    out = compute_lei_alias_rows(
        _gleif_frame(),
        _security_lookup_frame(),
        source="GLEIF",
        run_id="run-abc",
        as_of_date=as_of,
        available_at=available_at,
    )

    assert list(out.columns) >= [
        "security_id",
        "id_type",
        "id_value",
        "valid_from",
        "valid_to",
        "as_of_date",
        "available_at",
        "source",
        "run_id",
    ]
    assert (out["id_type"] == "LEI").all()

    rows = out.sort_values("security_id").reset_index(drop=True)
    assert list(rows["security_id"]) == ["SEC-CIK-0000320193", "SEC-CIK-0000789019"]
    assert list(rows["id_value"]) == ["HWUPKR0MPOU8FGXBT394", "549300OVFHYPPCFHF059"]
    assert (rows["source"] == "GLEIF").all()
    assert (rows["run_id"] == "run-abc").all()
    assert (rows["valid_from"] == as_of).all()
    assert rows["valid_to"].isna().all()
    assert (rows["available_at"] == available_at).all()


def test_compute_lei_alias_rows_fans_out_to_multiple_securities_under_one_entity():
    """An entity_id can have more than one security_id (share classes); the LEI is
    an entity-level identifier and must attach to every security under that entity."""
    from db.identifiers_lei import compute_lei_alias_rows

    lookup = pd.DataFrame(
        [
            {"entity_id": "CIK-0000320193", "security_id": "SEC-CIK-0000320193"},
            {"entity_id": "CIK-0000320193", "security_id": "SEC-CIK-0000320193-B"},
        ]
    )
    out = compute_lei_alias_rows(
        _gleif_frame(),
        lookup,
        source="GLEIF",
        run_id=None,
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert set(out["security_id"]) == {"SEC-CIK-0000320193", "SEC-CIK-0000320193-B"}
    assert set(out["id_value"]) == {"HWUPKR0MPOU8FGXBT394"}


def test_compute_lei_alias_rows_empty_lookup_yields_empty_frame():
    from db.identifiers_lei import compute_lei_alias_rows

    out = compute_lei_alias_rows(
        _gleif_frame(),
        pd.DataFrame(columns=["entity_id", "security_id"]),
        source="GLEIF",
        run_id=None,
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert out.empty


def test_compute_lei_alias_rows_deterministic_same_inputs_same_rows():
    from db.identifiers_lei import compute_lei_alias_rows

    kwargs = dict(
        source="GLEIF",
        run_id="run-abc",
        as_of_date=dt.date(2026, 6, 1),
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    first = compute_lei_alias_rows(_gleif_frame(), _security_lookup_frame(), **kwargs)
    second = compute_lei_alias_rows(_gleif_frame(), _security_lookup_frame(), **kwargs)
    pd.testing.assert_frame_equal(
        first.sort_values("security_id").reset_index(drop=True),
        second.sort_values("security_id").reset_index(drop=True),
    )


# ---------------------------------------------------------------------------
# cik <-> lei crosswalk derivation -- deterministic, offline, from the Golden
# Copy's RegistrationAuthority fields when the RA scheme is SEC EDGAR CIK.
# ---------------------------------------------------------------------------


def test_derive_cik_lei_crosswalk_from_sec_registration_authority():
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, derive_cik_lei_crosswalk

    raw = pd.DataFrame(
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
            {
                # Non-SEC RA scheme -- no usable cik crosswalk, must be dropped.
                "LEI": "5299000J2069MJHVWA37",
                "Entity.LegalName": "SOME UK ENTITY",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": "RA000585",
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "01234567",
            },
        ]
    )
    out = derive_cik_lei_crosswalk(raw)
    assert list(out.columns) >= ["lei", "cik", "legal_name"]
    assert len(out) == 1
    row = out.iloc[0]
    assert row["lei"] == "HWUPKR0MPOU8FGXBT394"
    assert row["cik"] == "0000320193"
    assert row["legal_name"] == "APPLE INC"


def test_derive_cik_lei_crosswalk_pads_short_cik_and_drops_blank():
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, derive_cik_lei_crosswalk

    raw = pd.DataFrame(
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "320193",
            },
            {
                "LEI": "NOENTITYID00000000000",
                "Entity.LegalName": "BLANK RA ID",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "",
            },
        ]
    )
    out = derive_cik_lei_crosswalk(raw)
    assert len(out) == 1
    assert out.iloc[0]["cik"] == "0000320193"


def test_derive_cik_lei_crosswalk_deterministic():
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, derive_cik_lei_crosswalk

    raw = pd.DataFrame(
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
        ]
    )
    first = derive_cik_lei_crosswalk(raw)
    second = derive_cik_lei_crosswalk(raw)
    pd.testing.assert_frame_equal(first.reset_index(drop=True), second.reset_index(drop=True))


# ---------------------------------------------------------------------------
# Injectable Level-1 (LEI record) file parsing -- CSV only, GLEIF Golden Copy shape.
# ---------------------------------------------------------------------------


def test_parse_gleif_file_reads_csv_and_derives_cik():
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, parse_gleif_file

    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "gleif_level1.csv"
        path.write_text(
            "LEI,Entity.LegalName,Entity.RegistrationAuthority.RegistrationAuthorityID,"
            "Entity.RegistrationAuthority.RegistrationAuthorityEntityID\n"
            f"HWUPKR0MPOU8FGXBT394,APPLE INC,{SEC_REGISTRATION_AUTHORITY_ID},0000320193\n"
            "5299000J2069MJHVWA37,SOME UK ENTITY,RA000585,01234567\n",
            encoding="utf-8",
        )
        frame = parse_gleif_file(path)
        assert set(frame["lei"]) == {"HWUPKR0MPOU8FGXBT394", "5299000J2069MJHVWA37"}
        apple = frame.loc[frame["lei"] == "HWUPKR0MPOU8FGXBT394"].iloc[0]
        assert apple["cik"] == "0000320193"
        uk = frame.loc[frame["lei"] == "5299000J2069MJHVWA37"].iloc[0]
        assert pd.isna(uk["cik"]) or uk["cik"] is None


def test_parse_gleif_file_rejects_unknown_extension(tmp_path):
    from db.identifiers_lei import parse_gleif_file

    path = tmp_path / "gleif.txt"
    path.write_text("nope", encoding="utf-8")
    with pytest.raises(ValueError):
        parse_gleif_file(path)


# ---------------------------------------------------------------------------
# GLEIF Level-2 (relationship) records -> entity->entity parent edges.
# ---------------------------------------------------------------------------


def _level2_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "child_lei": "5493001KJTIIGC8Y1R12",
                "parent_lei": "HWUPKR0MPOU8FGXBT394",
                "relationship_type": "IS_DIRECTLY_CONSOLIDATED_BY",
                "valid_from": "2019-01-01",
                "valid_to": None,
            },
            {
                "child_lei": "5493001KJTIIGC8Y1R12",
                "parent_lei": "549300OVFHYPPCFHF059",
                "relationship_type": "IS_ULTIMATELY_CONSOLIDATED_BY",
                "valid_from": "2019-01-01",
                "valid_to": None,
            },
        ]
    )


def _lei_entity_lookup_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {"lei": "5493001KJTIIGC8Y1R12", "entity_id": "CIK-0000111111"},
            {"lei": "HWUPKR0MPOU8FGXBT394", "entity_id": "CIK-0000320193"},
            {"lei": "549300OVFHYPPCFHF059", "entity_id": "CIK-0000789019"},
        ]
    )


def test_compute_entity_parent_edges_maps_lei_pairs_to_entity_ids():
    from db.identifiers_lei import compute_entity_parent_edges

    available_at = dt.datetime(2026, 6, 1, 22, 0, 0)
    out = compute_entity_parent_edges(
        _level2_frame(),
        _lei_entity_lookup_frame(),
        source="GLEIF",
        run_id="run-abc",
        available_at=available_at,
    )
    assert list(out.columns) >= [
        "child_entity_id",
        "parent_entity_id",
        "relationship_type",
        "valid_from",
        "valid_to",
        "as_of_date",
        "available_at",
        "source",
        "run_id",
    ]
    direct = out[out["relationship_type"] == "IS_DIRECTLY_CONSOLIDATED_BY"].iloc[0]
    assert direct["child_entity_id"] == "CIK-0000111111"
    assert direct["parent_entity_id"] == "CIK-0000320193"
    assert direct["valid_from"] == dt.date(2019, 1, 1)
    assert pd.isna(direct["valid_to"])

    ultimate = out[out["relationship_type"] == "IS_ULTIMATELY_CONSOLIDATED_BY"].iloc[0]
    assert ultimate["child_entity_id"] == "CIK-0000111111"
    assert ultimate["parent_entity_id"] == "CIK-0000789019"


def test_compute_entity_parent_edges_drops_unresolvable_lei():
    """A relationship referencing a LEI with no known entity_id (not yet loaded
    via Level-1) cannot yet be turned into an entity edge -- drop, don't crash."""
    from db.identifiers_lei import compute_entity_parent_edges

    lookup = pd.DataFrame([{"lei": "5493001KJTIIGC8Y1R12", "entity_id": "CIK-0000111111"}])
    out = compute_entity_parent_edges(
        _level2_frame(),
        lookup,
        source="GLEIF",
        run_id=None,
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert out.empty


def test_compute_entity_parent_edges_empty_level2_yields_empty_frame():
    from db.identifiers_lei import compute_entity_parent_edges

    out = compute_entity_parent_edges(
        pd.DataFrame(columns=["child_lei", "parent_lei", "relationship_type", "valid_from", "valid_to"]),
        _lei_entity_lookup_frame(),
        source="GLEIF",
        run_id=None,
        available_at=dt.datetime(2026, 6, 1, 22, 0, 0),
    )
    assert out.empty


def test_parse_gleif_level2_file_reads_csv():
    from db.identifiers_lei import parse_gleif_level2_file

    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "gleif_level2.csv"
        path.write_text(
            "Relationship.StartNode.NodeID,Relationship.EndNode.NodeID,"
            "Relationship.RelationshipType,Relationship.RelationshipPeriods.StartDate,"
            "Relationship.RelationshipPeriods.EndDate\n"
            "5493001KJTIIGC8Y1R12,HWUPKR0MPOU8FGXBT394,IS_DIRECTLY_CONSOLIDATED_BY,2019-01-01,\n",
            encoding="utf-8",
        )
        frame = parse_gleif_level2_file(path)
        assert list(frame["child_lei"]) == ["5493001KJTIIGC8Y1R12"]
        assert list(frame["parent_lei"]) == ["HWUPKR0MPOU8FGXBT394"]
        assert list(frame["relationship_type"]) == ["IS_DIRECTLY_CONSOLIDATED_BY"]
        assert list(frame["valid_from"]) == ["2019-01-01"]


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
            ('SEC-CIK-0000789019', 'CIK-0000789019', 'CIK-0000789019', 'MSFT', 'Microsoft Corp.', 'EQUITY', 'US', 'USD', true, DATE '2021-02-03', NULL, 'fixture')
        """
    )
    store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES
            ('SEC-CIK-0000320193', 'ENTITY_ID', 'CIK-0000320193', DATE '2019-05-04', NULL, DATE '2019-05-04', TIMESTAMP '2019-05-05 12:00:00', 'fixture', NULL),
            ('SEC-CIK-0000789019', 'ENTITY_ID', 'CIK-0000789019', DATE '2021-02-03', NULL, DATE '2021-02-03', TIMESTAMP '2021-02-04 12:00:00', 'fixture', NULL)
        """
    )


def _write_gleif_file(tmp_path, rows: list[dict]) -> "Path":
    import csv

    path = tmp_path / "gleif_level1.csv"
    fieldnames = [
        "LEI",
        "Entity.LegalName",
        "Entity.RegistrationAuthority.RegistrationAuthorityID",
        "Entity.RegistrationAuthority.RegistrationAuthorityEntityID",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return path


def _write_gleif_level2_file(tmp_path, rows: list[dict]) -> "Path":
    import csv

    path = tmp_path / "gleif_level2.csv"
    fieldnames = [
        "Relationship.StartNode.NodeID",
        "Relationship.EndNode.NodeID",
        "Relationship.RelationshipType",
        "Relationship.RelationshipPeriods.StartDate",
        "Relationship.RelationshipPeriods.EndDate",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return path


def test_lei_alias_dataset_load_attaches_lei_to_entity_securities(tmp_store):
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, LeiAliasDataset, LeiLoadOptions

    _seed_securities(tmp_store)
    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
            {
                "LEI": "549300OVFHYPPCFHF059",
                "Entity.LegalName": "MICROSOFT CORP",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000789019",
            },
        ],
    )

    dataset = LeiAliasDataset()
    result = dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))

    assert result.rows_loaded > 0

    lei_rows = tmp_store.con.execute(
        """
        SELECT security_id, id_value, source
        FROM security_identifier_history
        WHERE id_type = 'LEI'
        ORDER BY security_id
        """
    ).fetchall()
    assert lei_rows == [
        ("SEC-CIK-0000320193", "HWUPKR0MPOU8FGXBT394", "GLEIF"),
        ("SEC-CIK-0000789019", "549300OVFHYPPCFHF059", "GLEIF"),
    ]


def test_lei_alias_dataset_unmatched_entity_does_not_crash(tmp_store):
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, LeiAliasDataset, LeiLoadOptions

    _seed_securities(tmp_store)
    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                "LEI": "NOMATCH00000000000000",
                "Entity.LegalName": "UNKNOWN CO",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "9999999999",
            },
        ],
    )

    dataset = LeiAliasDataset()
    result = dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))
    assert result.rows_loaded == 0

    lei_count = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'LEI'"
    ).fetchone()[0]
    assert lei_count == 0


def test_lei_alias_dataset_load_is_idempotent(tmp_store):
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, LeiAliasDataset, LeiLoadOptions

    _seed_securities(tmp_store)
    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
        ],
    )

    dataset = LeiAliasDataset()
    dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))
    dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))

    lei_count = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE id_type = 'LEI'"
    ).fetchone()[0]
    assert lei_count == 1


def test_lei_alias_dataset_optionally_loads_level2_parent_edges(tmp_store):
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, LeiAliasDataset, LeiLoadOptions

    _seed_securities(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES ('SEC-CIK-0000111111', 'CIK-0000111111', 'CIK-0000111111', 'SUBS', 'Apple Subsidiary Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-01-01', NULL, 'fixture')
        """
    )
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id, id_type, id_value, valid_from, valid_to,
            as_of_date, available_at, source, run_id
        )
        VALUES ('SEC-CIK-0000111111', 'ENTITY_ID', 'CIK-0000111111', DATE '2019-01-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-02 12:00:00', 'fixture', NULL)
        """
    )

    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                "LEI": "5493001KJTIIGC8Y1R12",
                "Entity.LegalName": "APPLE SUBSIDIARY INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000111111",
            },
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
        ],
    )
    level2_file = _write_gleif_level2_file(
        tmp_store.path.parent,
        [
            {
                "Relationship.StartNode.NodeID": "5493001KJTIIGC8Y1R12",
                "Relationship.EndNode.NodeID": "HWUPKR0MPOU8FGXBT394",
                "Relationship.RelationshipType": "IS_DIRECTLY_CONSOLIDATED_BY",
                "Relationship.RelationshipPeriods.StartDate": "2019-01-01",
                "Relationship.RelationshipPeriods.EndDate": "",
            }
        ],
    )

    dataset = LeiAliasDataset()
    result = dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file, lei_level2_file=level2_file))

    assert result.details["parent_edge_rows"] == 1
    edges = tmp_store.con.execute(
        "SELECT child_entity_id, parent_entity_id, relationship_type FROM entity_parent_edges"
    ).fetchall()
    assert edges == [("CIK-0000111111", "CIK-0000320193", "IS_DIRECTLY_CONSOLIDATED_BY")]


def test_lei_alias_dataset_no_network_call_offline_only(tmp_store, monkeypatch):
    """Guard: the dataset must never construct a requests session / hit the network."""
    import db.identifiers_lei as identifiers_lei

    _seed_securities(tmp_store)
    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": identifiers_lei.SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
        ],
    )

    if hasattr(identifiers_lei, "requests"):
        def _boom(*_args, **_kwargs):
            raise AssertionError("identifiers_lei must not perform network requests")

        monkeypatch.setattr(identifiers_lei.requests, "get", _boom, raising=False)
        monkeypatch.setattr(identifiers_lei.requests, "post", _boom, raising=False)

    from db.identifiers_lei import LeiAliasDataset, LeiLoadOptions

    dataset = LeiAliasDataset()
    result = dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))
    assert result.rows_loaded > 0


def test_lei_alias_dataset_duplicate_cik_routes_to_resolution_ledger_not_merge(tmp_store):
    """GLEIF Golden Copy is NOT a safe 1:1 cik<->lei key: a lapsed-then-reissued
    registration (or LOU duplicate/transfer artifact) can carry two distinct LEI
    records for the same SEC EDGAR CIK. The loader must NOT fan those out into two
    simultaneously-current LEI aliases on the same security -- it must route the
    ambiguous cik into identifier_resolution_candidates/decisions instead, mirroring
    identifiers_figi.py's conflict-routing for ambiguous CUSIPs."""
    from db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, LeiAliasDataset, LeiLoadOptions

    _seed_securities(tmp_store)
    lei_file = _write_gleif_file(
        tmp_store.path.parent,
        [
            {
                # Same CIK, two distinct LEIs -- ambiguous, must not both be merged.
                "LEI": "HWUPKR0MPOU8FGXBT394",
                "Entity.LegalName": "APPLE INC",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
            {
                "LEI": "DUPLICATELEI000000001",
                "Entity.LegalName": "APPLE INC (REISSUED)",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000320193",
            },
            {
                # Unambiguous CIK -- must still be merged normally.
                "LEI": "549300OVFHYPPCFHF059",
                "Entity.LegalName": "MICROSOFT CORP",
                "Entity.RegistrationAuthority.RegistrationAuthorityID": SEC_REGISTRATION_AUTHORITY_ID,
                "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "0000789019",
            },
        ],
    )

    dataset = LeiAliasDataset()
    dataset.run(tmp_store, LeiLoadOptions(lei_file=lei_file))

    # No duplicate/ambiguous current LEI alias written for the Apple security.
    lei_rows = tmp_store.con.execute(
        """
        SELECT security_id, id_value
        FROM security_identifier_history
        WHERE id_type = 'LEI' AND security_id = 'SEC-CIK-0000320193' AND valid_to IS NULL
        """
    ).fetchall()
    assert lei_rows == []

    # The unambiguous cik->lei match still merges normally.
    msft_rows = tmp_store.con.execute(
        "SELECT security_id, id_value FROM security_identifier_history WHERE id_type = 'LEI' AND security_id = 'SEC-CIK-0000789019'"
    ).fetchall()
    assert msft_rows == [("SEC-CIK-0000789019", "549300OVFHYPPCFHF059")]

    candidates = tmp_store.con.execute(
        """
        SELECT source_key_type, source_key_value, target_id_type, target_id_value, match_method, confidence, candidate_status
        FROM identifier_resolution_candidates
        WHERE source_key_value = '0000320193'
        ORDER BY target_id_value
        """
    ).fetchall()
    assert len(candidates) == 2
    for row in candidates:
        assert row[0] == "CIK"
        assert row[1] == "0000320193"
        assert row[2] == "LEI"
        assert row[3] in {"HWUPKR0MPOU8FGXBT394", "DUPLICATELEI000000001"}
        assert row[4] == "gleif_cik_lei"
        assert row[5] < 1.0
        assert row[6] == "conflict"

    decisions = tmp_store.con.execute(
        """
        SELECT decision_status, source_key_value
        FROM identifier_resolution_decisions
        WHERE source_key_value = '0000320193'
        """
    ).fetchall()
    assert len(decisions) == 2
    for status, _key in decisions:
        assert status == "needs_review"


# ---------------------------------------------------------------------------
# Migration 0081 (entity_parent_edges_schema_catalog) field_catalog coverage.
# entity_parent_edges is a brand-new table, not additive columns, so every one
# of its base columns must be seeded -- not just the 3 "identity" columns.
# PF3-S2 later adds/catalogs is_latest_revision to close the PIT gap.
# ---------------------------------------------------------------------------


def test_migration_0081_entity_parent_edges_field_catalog_full_coverage(tmp_store):
    table_columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'entity_parent_edges'
            """
        ).fetchall()
    }
    assert table_columns == {
        "child_entity_id",
        "parent_entity_id",
        "relationship_type",
        "valid_from",
        "valid_to",
        "as_of_date",
        "available_at",
        "source",
        "run_id",
        "source_loaded_at",
        "is_latest_revision",
    }

    cataloged_columns = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'entity_parent_edges'"
        ).fetchall()
    }
    assert cataloged_columns == table_columns
