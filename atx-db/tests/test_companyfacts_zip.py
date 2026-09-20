"""S45: offline SEC companyfacts.zip bulk fetcher for the fundamentals loader.

SEC publishes the entire XBRL company-facts universe as a single nightly bulk archive
(`companyfacts.zip`, ~1.4 GB) at
``https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip`` containing one
``CIK##########.json`` per filer — byte-identical JSON to the per-CIK
``data.sec.gov/.../companyfacts/CIK*.json`` endpoint. Backfilling from a locally-downloaded
zip replaces N throttled network round trips with one download plus N lazy member reads,
and removes the per-CIK 404/throttle/retry burden. Download is a one-time operator step;
these tests run purely against a tiny fixture zip built on disk — no network.
"""
from __future__ import annotations

import json
import zipfile
from dataclasses import replace

import pytest

from atx_db.fundamentals import (
    SecCompanyFactsDataset,
    SecCompanyFactsOptions,
    _make_companyfacts_zip_fetcher,
    resolve_companyfacts_targets,
)


def _companyfacts_payload(cik: str = "0000320193") -> dict:
    return {
        "cik": int(cik),
        "entityName": "Apple Inc.",
        "facts": {
            "us-gaap": {
                "Assets": {
                    "label": "Assets",
                    "units": {
                        "USD": [
                            {
                                "end": "2023-12-31",
                                "val": 100.0,
                                "accn": "0000320193-24-000001",
                                "fy": 2023,
                                "fp": "FY",
                                "form": "10-K",
                                "filed": "2024-02-15",
                            }
                        ]
                    },
                }
            }
        },
    }


def _write_companyfacts_zip(path, ciks=("0000320193",)):
    with zipfile.ZipFile(path, "w") as zf:
        for cik in ciks:
            zf.writestr(f"CIK{cik}.json", json.dumps(_companyfacts_payload(cik)))
    return path


class TestCompanyFactsZipFetcher:
    def test_returns_payload_for_present_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        payload = fetch("320193")
        assert payload is not None
        assert "us-gaap" in payload["facts"]

    def test_returns_none_for_missing_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch("999999") is None

    def test_returns_none_for_non_numeric_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch("not-a-cik") is None

    def test_zero_pads_bare_integer_cik(self, tmp_path):
        # Member names are zero-padded to CIK10; a bare integer must still resolve.
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch(320193) is not None


class TestCompanyFactsZipLoad:
    def test_load_reads_from_zip_without_network(self, tmp_store, tmp_path, monkeypatch):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")

        # Any network use must blow up — proves the zip path is fully offline.
        def _boom(ua):
            raise AssertionError("sec_session must not be called when companyfacts_zip is set")

        monkeypatch.setattr("atx_db.fundamentals.sec_session", _boom)
        monkeypatch.setattr(
            "atx_db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [("AAPL", "0000320193", "SEC-CIK-0000320193")],
        )
        res = SecCompanyFactsDataset().run(
            tmp_store,
            SecCompanyFactsOptions(companyfacts_zip=zpath, concepts=("Assets",)),
        )
        assert res.rows_loaded >= 1
        n = tmp_store.con.execute(
            "SELECT count(*) FROM sec_company_facts WHERE cik = '0000320193'"
        ).fetchone()[0]
        assert n >= 1

    def test_missing_zip_member_is_skipped_not_fatal(self, tmp_store, tmp_path, monkeypatch):
        # A CIK absent from the bulk zip must be recorded as a failed target and skipped
        # (skip_failed_targets defaults on for zip loads via the option), not abort the run.
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip", ciks=("0000320193",))
        monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda ua: None)
        monkeypatch.setattr(
            "atx_db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [
                ("AAPL", "0000320193", "SEC-CIK-0000320193"),
                ("GHOST", "0009999999", "SEC-CIK-0009999999"),
            ],
        )
        res = SecCompanyFactsDataset().run(
            tmp_store,
            SecCompanyFactsOptions(
                companyfacts_zip=zpath, concepts=("Assets",), skip_failed_targets=True
            ),
        )
        assert res.details["loaded_targets"] == 1
        assert res.details["failed_target_count"] == 1


def test_archive_targets_are_exact_sorted_deduplicated_and_paged(tmp_path):
    path = tmp_path / "members.zip"
    with zipfile.ZipFile(path, "w") as archive:
        for name in ("CIK0000000009.json", "CIK0000000001.json", "nested/CIK0000000002.json",
                     "CIK2.json", "cik0000000003.json", "CIK0000000004.json.bak"):
            archive.writestr(name, "{}")
        with pytest.warns(UserWarning, match="Duplicate name"):
            archive.writestr("CIK0000000001.json", "{}")
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path)
    # No store access is permitted for archive discovery.
    assert resolve_companyfacts_targets(None, options) == [
        ("CIK0000000001", "0000000001", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001"),
        ("CIK0000000009", "0000000009", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000009"),
    ]
    assert resolve_companyfacts_targets(None, replace(options, symbol_offset=1, symbol_limit=1))[0][1] == "0000000009"
    with pytest.raises(ValueError, match="positive"):
        resolve_companyfacts_targets(None, replace(options, symbol_limit=0))


def test_archive_requires_local_zip_before_network(monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("archive mode opened a network session")

    monkeypatch.setattr("atx_db.fundamentals.sec_session", forbidden)
    with pytest.raises(ValueError, match="local companyfacts_zip"):
        SecCompanyFactsDataset().load(None, SecCompanyFactsOptions(symbol_source="archive_members"))


def test_archive_load_accounts_failures_empty_unresolved_and_replacement(tmp_store, tmp_path, monkeypatch):
    path = tmp_path / "companyfacts.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps(_companyfacts_payload("1")))
        archive.writestr("CIK0000000002.json", "{invalid json")
        unsupported = _companyfacts_payload("3")
        unsupported["facts"]["ifrs-full"] = unsupported["facts"].pop("us-gaap")
        archive.writestr("CIK0000000003.json", json.dumps(unsupported))
        filtered = _companyfacts_payload("4")
        filtered["facts"]["us-gaap"]["Other"] = filtered["facts"]["us-gaap"].pop("Assets")
        archive.writestr("CIK0000000004.json", json.dumps(filtered))
        archive.writestr("CIK0000000005.json", json.dumps({"cik": 5, "facts": []}))
        archive.writestr("CIK0000000006.json", json.dumps(_companyfacts_payload("99")))
        archive.writestr("readme.txt", "ignored")
    # Alias/current source metadata must neither duplicate targets nor resolve old facts.
    tmp_store.con.execute("INSERT INTO sec_company_tickers (cik,ticker,title,security_id) VALUES "
                          "('0000000001','AAA','Alpha','CURRENT'), ('0000000001','AAA.A','Alpha','CURRENT')")
    monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda *_: pytest.fail("HTTP fallback"))
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets",), refresh_derived_surfaces=False)
    result = SecCompanyFactsDataset().load(tmp_store, options)
    detail = result.details
    assert result.rows_loaded == 1
    assert detail["archive_member_count"] == 7
    assert detail["archive_cik_member_count"] == detail["valid_target_count"] == detail["target_count"] == 6
    assert detail["completed_targets"] == 3
    assert detail["loaded_targets"] == 1
    assert detail["empty_target_count"] == 2
    assert detail["empty_target_reasons"] == {"unsupported_or_empty_taxonomy": 1, "allowlist_empty": 1}
    assert detail["failed_target_count"] == 3
    assert detail["failure_types"] == {"JSONDecodeError": 1, "ValueError": 2}
    assert detail["unresolved_security_targets"] == detail["unresolved_security_fact_rows"] == 1
    assert detail["unresolved_entity_fact_rows"] == detail["unresolved_cik_candidate_rows"] == 1
    assert detail["previously_completed_targets"] is None
    assert len(detail["archive_sha256"]) == len(detail["allowlist_sha256"]) == 64
    assert tmp_store.con.execute("SELECT security_id,entity_id FROM sec_company_facts").fetchall() == [
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001", None)]
    assert tmp_store.con.execute("SELECT security_id,symbol FROM fundamental_points").fetchall() == [
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001", None)]
    assert tmp_store.con.execute("SELECT count(*) FROM securities WHERE security_id='SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001'").fetchone()[0] == 0
    assert tmp_store.con.execute("SELECT candidate_status FROM identifier_resolution_candidates "
                                 "WHERE source_dataset_id='sec_company_facts'").fetchall() == [("proposed",)]
    assert tmp_store.con.execute("SELECT count(*) FROM raw_source_files WHERE status='error'").fetchone()[0] == 3

    # Append-missing skips any existing facts, without asserting fingerprint completion.
    skipped = SecCompanyFactsDataset().load(tmp_store, replace(options, skip_loaded_targets=True))
    assert skipped.details["skipped_existing_targets"] == 1
    assert skipped.details["target_count"] == 5
    assert skipped.details["previously_completed_targets"] is None
    assert skipped.details["load_policy"] == "append_missing"

    # New allowlist: replacement clears the old CIK facts/points on a successful empty parse.
    empty = SecCompanyFactsDataset().load(tmp_store, replace(options, concepts=("NeverReported",), symbol_limit=1))
    assert empty.details["empty_target_count"] == 1
    assert empty.details["allowlist_sha256"] != detail["allowlist_sha256"]
    assert tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()[0] == 0
    assert tmp_store.con.execute("SELECT count(*) FROM fundamental_points").fetchone()[0] == 0
    assert tmp_store.con.execute("SELECT count(*) FROM identifier_resolution_candidates "
                                 "WHERE source_dataset_id='sec_company_facts'").fetchone()[0] == 0


def test_empty_archive_and_all_existing_have_different_outcomes(tmp_store, tmp_path):
    path = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets",), refresh_derived_surfaces=False)
    dataset = SecCompanyFactsDataset()
    first = dataset.load(tmp_store, options)
    second = dataset.load(tmp_store, options)
    assert first.rows_loaded == second.rows_loaded == 1
    assert tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()[0] == 1
    skipped = dataset.load(tmp_store, replace(options, skip_loaded_targets=True))
    assert skipped.details["outcome"] == "all_existing_skipped"
    assert skipped.details["skipped_existing_targets"] == 1
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("not-a-cik.json", "{}")
    empty = dataset.load(tmp_store, options)
    assert empty.details["outcome"] == "no_valid_targets"
    assert empty.details["valid_target_count"] == 0


def test_replacement_does_not_delete_another_cik_with_shared_filing_keys(tmp_store, tmp_path):
    path = _write_companyfacts_zip(tmp_path / "companyfacts.zip", ciks=("0000000001", "0000000002"))
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets",), refresh_derived_surfaces=False)
    dataset = SecCompanyFactsDataset()
    # The helper deliberately gives both CIKs the same accession/concept/unit/period.
    dataset.load(tmp_store, options)
    for _ in range(2):
        dataset.load(tmp_store, replace(options, symbol_limit=1))
        assert tmp_store.con.execute("SELECT cik FROM sec_company_facts ORDER BY cik").fetchall() == [
            ("0000000001",), ("0000000002",)]
        assert tmp_store.con.execute("SELECT security_id FROM fundamental_points ORDER BY security_id").fetchall() == [
            ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001",),
            ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000002",)]


def test_replacement_clears_old_resolved_points_with_null_accession(tmp_store, tmp_path):
    path = tmp_path / "companyfacts.zip"
    payload = _companyfacts_payload("1")
    payload["facts"]["us-gaap"]["Assets"]["units"]["USD"][0]["accn"] = None
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps(payload))
    tmp_store.con.execute("""INSERT INTO security_identifier_history
        (security_id,id_type,id_value,valid_from,as_of_date,available_at,source) VALUES
        ('RESOLVED','CIK','0000000001','2020-01-01','2020-01-01','2020-01-01','fixture')""")
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets",), refresh_derived_surfaces=False)
    dataset = SecCompanyFactsDataset()
    dataset.load(tmp_store, options)
    assert tmp_store.con.execute("SELECT security_id FROM fundamental_points").fetchall() == [("RESOLVED",)]
    tmp_store.con.execute("UPDATE security_identifier_history SET available_at=TIMESTAMP '2026-01-01'")
    for _ in range(2):
        dataset.load(tmp_store, options)
        assert tmp_store.con.execute("SELECT security_id,accession_number FROM fundamental_points").fetchall() == [
            ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001", None)]


def test_replacement_normalizes_old_raw_points_and_candidate_scope(tmp_store, tmp_path):
    path = _write_companyfacts_zip(tmp_path / "companyfacts.zip", ciks=("0000000001", "0000000002"))
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets",), refresh_derived_surfaces=False)
    dataset = SecCompanyFactsDataset()
    dataset.load(tmp_store, options)
    # Reproduce a prior loader: unpadded raw CIK, PIT raw ID, legacy passthrough point ID.
    tmp_store.con.execute("UPDATE sec_company_facts SET cik='1',security_id='HISTORICAL' WHERE cik='0000000001'")
    tmp_store.con.execute("UPDATE fundamental_points SET security_id='SEC-CIK-0000000001' "
                          "WHERE security_id='SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001'")
    tmp_store.con.execute("UPDATE identifier_resolution_candidates SET source_key_value='1' "
                          "WHERE source_dataset_id='sec_company_facts' AND source_key_value='0000000001'")
    assert resolve_companyfacts_targets(tmp_store, replace(options, skip_loaded_targets=True)) == []
    for _ in range(2):
        dataset.load(tmp_store, replace(options, symbol_limit=1))
        assert tmp_store.con.execute("SELECT cik FROM sec_company_facts ORDER BY cik").fetchall() == [
            ("0000000001",), ("0000000002",)]
        assert tmp_store.con.execute("SELECT security_id FROM fundamental_points ORDER BY security_id").fetchall() == [
            ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000001",),
            ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000002",)]
        assert tmp_store.con.execute("SELECT source_key_value FROM identifier_resolution_candidates "
                                     "WHERE source_dataset_id='sec_company_facts' ORDER BY source_key_value").fetchall() == [
            ("0000000001",), ("0000000002",)]


@pytest.mark.parametrize("broken", [{}, {"units": {"USD": {}}}, {"units": []}, {"units": {"USD": [None]}}])
@pytest.mark.parametrize("mixed", [False, True])
def test_malformed_selected_structure_preserves_prior_issuer(tmp_store, tmp_path, broken, mixed):
    path = _write_companyfacts_zip(tmp_path / "companyfacts.zip", ciks=("0000000001",))
    options = SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                    concepts=("Assets", "NetIncomeLoss"), refresh_derived_surfaces=False)
    dataset = SecCompanyFactsDataset()
    dataset.load(tmp_store, options)
    tables = ("sec_company_facts", "fundamental_points", "identifier_resolution_candidates")
    before = {table: tmp_store.con.execute(f"SELECT * FROM {table}").fetchall() for table in tables}
    payload = _companyfacts_payload("1")
    payload["facts"]["us-gaap"]["NetIncomeLoss" if mixed else "Assets"] = broken
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps(payload))
    result = dataset.load(tmp_store, options)
    assert result.details["failed_target_count"] == 1
    assert result.details["completed_targets"] == result.details["empty_target_count"] == 0
    assert result.rows_loaded == 0
    for table in tables:
        assert tmp_store.con.execute(f"SELECT * FROM {table}").fetchall() == before[table]
