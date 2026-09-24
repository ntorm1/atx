"""Offline CF5 failure/resume and connection-lifetime regression fixtures."""
from __future__ import annotations

import datetime as dt
import json
import zipfile
from dataclasses import replace

import pytest

import atx_db.fundamentals as fundamentals
from atx_db._companyfacts_resume import reopen_companyfacts_store
from atx_db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions

_UA = "atx-db/0.2 atx-research@example.com"
_AS_OF = dt.date(2026, 9, 20)


@pytest.fixture(autouse=True)
def _offline(monkeypatch):
    monkeypatch.setattr(fundamentals, "sec_session", lambda *_: pytest.fail("offline archive opened HTTP"))


def _payload(cik):
    return {"cik": cik, "entityName": "Fixture issuer", "facts": {"us-gaap": {"Assets": {
        "units": {"USD": [{"end": "2023-12-31", "val": 100 + cik,
                            "accn": f"{cik:010d}-24-000001", "fy": 2023, "fp": "FY",
                            "form": "10-K", "filed": "2024-02-15"}]},
    }}}}


def _archive(path, members):
    with zipfile.ZipFile(path, "w") as archive:
        for cik, payload in members.items():
            archive.writestr(f"CIK{cik:010d}.json", payload if isinstance(payload, bytes) else json.dumps(payload))
    return path


def _options(path):
    return SecCompanyFactsOptions(
        symbols=(), symbol_source="archive_members", companyfacts_zip=path,
        concepts=("Assets",), refresh_derived_surfaces=False, as_of_date=_AS_OF,
        user_agent=_UA,
    )


class _FailBeforeSix(SecCompanyFactsDataset):
    def _replace_facts(self, store, facts, points, security_id, *, cik, **kwargs):
        if cik == "0000000006":
            raise RuntimeError("injected issuer failure")
        return super()._replace_facts(store, facts, points, security_id, cik=cik, **kwargs)


class _FailOnCik(SecCompanyFactsDataset):
    def __init__(self, failed_cik):
        self.failed_cik = f"{failed_cik:010d}"

    def _replace_facts(self, store, facts, points, security_id, *, cik, **kwargs):
        if cik == self.failed_cik:
            raise RuntimeError("injected issuer failure")
        return super()._replace_facts(store, facts, points, security_id, cik=cik, **kwargs)


def _failed_prefix(store, tmp_path, *, mixed=False):
    members = {1: _payload(1), 2: _payload(2)}
    if mixed:
        members.update({3: {"cik": 3, "facts": {"cef": {}}}, 4: b"{}", 5: b"{invalid"})
    members[6] = _payload(6)
    options = _options(_archive(tmp_path / "companyfacts.zip", members))
    with pytest.raises(RuntimeError, match="injected issuer failure"):
        _FailBeforeSix().run(store, options)
    prior_id, status = store.con.execute(
        "SELECT run_id,status FROM dataset_runs WHERE dataset_id='sec_company_facts' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()
    assert status == "failed"
    return options, prior_id


def _snapshot(store):
    return {table: store.con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall()
            for table in ("sec_company_facts", "fundamental_points", "identifier_resolution_candidates",
                          "raw_source_files")}


def test_verified_resume_skips_loaded_recovers_candidates_and_retries_observations(tmp_store, tmp_path, monkeypatch):
    options, prior_id = _failed_prefix(tmp_store, tmp_path, mixed=True)
    before = tmp_store.con.execute("SELECT * FROM sec_company_facts ORDER BY cik").fetchall()
    # Model archive3: the facts committed, but the end-of-load candidate flush did not run.
    tmp_store.con.execute("DELETE FROM identifier_resolution_candidates")
    reads = []
    original = fundamentals._CompanyFactsZipFetcher.__call__

    def observed(fetcher, cik):
        reads.append(cik)
        return original(fetcher, cik)

    monkeypatch.setattr(fundamentals._CompanyFactsZipFetcher, "__call__", observed)
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert reads == [f"{cik:010d}" for cik in (3, 4, 5, 6)]
    assert result.rows_loaded == 1
    details = result.details
    assert details["previously_completed_targets"] == details["resumed_loaded_targets"] == 2
    assert details["resume_verified_rows"] == 2
    assert details["recovered_unresolved_candidate_rows"] == 2
    assert details["loaded_targets"] == 3
    assert details["completed_targets"] == 4
    assert details["empty_target_count"] == details["unavailable_target_count"] == details["failed_target_count"] == 1
    assert details["unresolved_cik_candidate_rows"] == 3
    assert details["completed_targets"] + details["unavailable_target_count"] + details["failed_target_count"] == 6
    assert tmp_store.con.execute("SELECT * FROM sec_company_facts WHERE cik<'0000000006' ORDER BY cik").fetchall() == before
    recovered = tmp_store.con.execute(
        "SELECT source_key_value,source_security_id,details_json FROM identifier_resolution_candidates ORDER BY source_key_value"
    ).fetchall()
    assert len(recovered) == 3
    for cik, security_id, raw in recovered:
        assert security_id == f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{cik}"
        assert json.loads(raw)["fact_available_at"] == "2024-02-15 22:00:00"
    owners = tmp_store.con.execute("SELECT cik,run_id FROM sec_company_facts ORDER BY cik").fetchall()
    assert owners == [("0000000001", prior_id), ("0000000002", prior_id), ("0000000006", result.run_id)]
    assert tmp_store.con.execute("SELECT status FROM dataset_runs WHERE run_id=?", [result.run_id]).fetchone() == (
        "succeeded",
    )
    marker = tmp_store.con.execute(
        "SELECT observed_value,details_json FROM data_quality_checks WHERE check_name='source_completeness'"
    ).fetchone()
    assert marker[0] == 1
    assert json.loads(marker[1])["run_id"] == result.run_id
    completed_facts = tmp_store.con.execute("SELECT * FROM sec_company_facts ORDER BY cik").fetchall()
    latest = result
    # A source-incomplete normal return must authorize its own completed tail.
    # Repeat again after its error receipt has been overwritten, proving the
    # ancestor's durable marker does not depend on retaining that mutable error.
    for _ in range(2):
        reads.clear()
        latest = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=latest.run_id))
        assert reads == [f"{cik:010d}" for cik in (3, 4, 5)]
        assert latest.rows_loaded == 0
        assert latest.details["previously_completed_targets"] == 3
        assert latest.details["loaded_targets"] == 3
        assert latest.details["empty_target_count"] == latest.details["unavailable_target_count"] == 1
        assert latest.details["failed_target_count"] == 1
        assert tmp_store.con.execute("SELECT * FROM sec_company_facts ORDER BY cik").fetchall() == completed_facts


@pytest.mark.parametrize("receipt_change", ["deleted", "overwritten", "malformed"])
def test_missing_receipt_replays_committed_issuer_instead_of_skipping(tmp_store, tmp_path, receipt_change):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    source_url = fundamentals._companyfacts_zip_member_url("1")
    if receipt_change == "deleted":
        tmp_store.con.execute("DELETE FROM raw_source_files WHERE source_url=?", [source_url])
    else:
        metadata = "{" if receipt_change == "malformed" else json.dumps({"run_id": "other-attempt"})
        tmp_store.con.execute("UPDATE raw_source_files SET metadata_json=? WHERE source_url=?", [metadata, source_url])
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert result.rows_loaded == 2
    assert result.details["previously_completed_targets"] == 1
    assert tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()[0] == 3
    assert tmp_store.con.execute("SELECT count(*) FROM fundamental_points").fetchone()[0] == 3


@pytest.mark.parametrize("damage", ["fact", "point", "point_value", "point_symbol", "receipt_rows", "source_digest",
                                    "point_foreign_owner_extra", "point_null_owner_extra"])
def test_contradictory_completion_proof_fails_before_issuer_mutation(tmp_store, tmp_path, damage):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    if damage == "fact":
        tmp_store.con.execute("DELETE FROM sec_company_facts WHERE cik='0000000001'")
    elif damage == "point":
        tmp_store.con.execute("DELETE FROM fundamental_points WHERE accession_number='0000000001-24-000001'")
    elif damage == "point_value":
        tmp_store.con.execute("UPDATE fundamental_points SET value=value+1 WHERE accession_number='0000000001-24-000001'")
    elif damage == "point_symbol":
        tmp_store.con.execute("UPDATE fundamental_points SET symbol='CURRENT' WHERE accession_number='0000000001-24-000001'")
    elif damage in ("point_foreign_owner_extra", "point_null_owner_extra"):
        owner = "foreign-attempt" if damage == "point_foreign_owner_extra" else None
        tmp_store.con.execute(
            """INSERT INTO fundamental_points
               SELECT * REPLACE (cast(? AS VARCHAR) AS run_id) FROM fundamental_points
               WHERE accession_number='0000000001-24-000001'""", [owner],
        )
    elif damage == "source_digest":
        tmp_store.con.execute("UPDATE raw_source_files SET sha256='different-archive'")
    else:
        url = fundamentals._companyfacts_zip_member_url("1")
        raw = tmp_store.con.execute("SELECT metadata_json FROM raw_source_files WHERE source_url=?", [url]).fetchone()[0]
        metadata = json.loads(raw)
        metadata["rows"] = 999
        tmp_store.con.execute("UPDATE raw_source_files SET metadata_json=? WHERE source_url=?", [json.dumps(metadata), url])
    before = _snapshot(tmp_store)
    with pytest.raises(ValueError, match="companyfacts resume"):
        SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert _snapshot(tmp_store) == before


@pytest.mark.parametrize("damage", ["missing", "wrong_run", "zero_count", "wrong_time", "malformed"])
def test_source_incomplete_success_requires_its_durable_positive_marker(tmp_store, tmp_path, damage):
    options, prior_id = _failed_prefix(tmp_store, tmp_path, mixed=True)
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    if damage == "missing":
        tmp_store.con.execute("DELETE FROM data_quality_checks WHERE check_name='source_completeness'")
    elif damage == "wrong_run":
        tmp_store.con.execute("UPDATE data_quality_checks SET details_json=? WHERE check_name='source_completeness'",
                              [json.dumps({"run_id": prior_id, "failed_target_count": 1})])
    elif damage == "zero_count":
        tmp_store.con.execute("UPDATE data_quality_checks SET observed_value=0 WHERE check_name='source_completeness'")
    elif damage == "wrong_time":
        tmp_store.con.execute("UPDATE data_quality_checks SET checked_at=TIMESTAMP '2000-01-01' "
                              "WHERE check_name='source_completeness'")
    else:
        tmp_store.con.execute("UPDATE data_quality_checks SET details_json='{' WHERE check_name='source_completeness'")
    before = _snapshot(tmp_store)
    with pytest.raises(ValueError, match="durable source-incomplete evidence"):
        SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=result.run_id))
    assert _snapshot(tmp_store) == before


def test_ordinary_successful_run_does_not_authorize_resume(tmp_store, tmp_path):
    options = _options(_archive(tmp_path / "companyfacts.zip", {1: _payload(1)}))
    result = SecCompanyFactsDataset().run(tmp_store, options)
    before = _snapshot(tmp_store)
    with pytest.raises(ValueError, match="durable source-incomplete evidence"):
        SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=result.run_id))
    assert _snapshot(tmp_store) == before


def test_shared_security_proof_accepts_matching_foreign_owner_pairs(tmp_store, tmp_path):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    tmp_store.con.execute("UPDATE sec_company_facts SET security_id='SHARED'")
    tmp_store.con.execute("UPDATE fundamental_points SET security_id='SHARED'")
    tmp_store.con.execute(
        """INSERT INTO sec_company_facts SELECT * REPLACE (
            '0000000099' AS cik, 'foreign-attempt' AS run_id,
            '0000000099-24-000001' AS accession_number,
            'foreign-source-fixture' AS source_url)
           FROM sec_company_facts WHERE cik='0000000001'"""
    )
    tmp_store.con.execute(
        """INSERT INTO fundamental_points SELECT * REPLACE (
            'foreign-attempt' AS run_id, '0000000099-24-000001' AS accession_number,
            'LEGACY' AS symbol)
           FROM fundamental_points WHERE accession_number='0000000001-24-000001'"""
    )
    foreign = tmp_store.con.execute("SELECT * FROM fundamental_points WHERE run_id='foreign-attempt'").fetchall()
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert result.details["previously_completed_targets"] == 2
    assert result.rows_loaded == 1
    assert tmp_store.con.execute("SELECT * FROM fundamental_points WHERE run_id='foreign-attempt'").fetchall() == foreign


@pytest.mark.parametrize("change", ["allowlist", "as_of", "offset", "limit", "append", "archive"])
def test_resume_rejects_incompatible_source_and_scope(tmp_store, tmp_path, change):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    options = replace(options, resume_from_run_id=prior_id)
    if change == "allowlist":
        options = replace(options, concepts=("Revenues",))
    elif change == "as_of":
        options = replace(options, as_of_date=dt.date(2026, 9, 21))
    elif change == "offset":
        options = replace(options, symbol_offset=1)
    elif change == "limit":
        options = replace(options, symbol_limit=1)
    elif change == "append":
        options = replace(options, skip_loaded_targets=True)
    else:
        _archive(options.companyfacts_zip, {1: _payload(11), 2: _payload(2), 6: _payload(6)})
    before = _snapshot(tmp_store)
    with pytest.raises(ValueError, match="companyfacts resume"):
        SecCompanyFactsDataset().run(tmp_store, options)
    assert _snapshot(tmp_store) == before


def test_failed_resume_lineage_retains_ancestor_proof(tmp_store, tmp_path):
    options, first_id = _failed_prefix(tmp_store, tmp_path)
    with pytest.raises(RuntimeError, match="injected issuer failure"):
        _FailBeforeSix().run(tmp_store, replace(options, resume_from_run_id=first_id))
    second_id = tmp_store.con.execute(
        "SELECT run_id FROM dataset_runs WHERE dataset_id='sec_company_facts' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()[0]
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=second_id))
    assert result.details["resume_lineage"] == [first_id, second_id]
    assert result.details["previously_completed_targets"] == 2
    assert result.rows_loaded == 1


def test_legacy_missing_payload_cik_error_is_retried_and_classified_empty(tmp_store, tmp_path, monkeypatch):
    payload = {"entityName": "Fixture fund", "facts": {"cef": {}}}
    options = _options(_archive(tmp_path / "companyfacts.zip", {1: _payload(1), 2: payload, 6: _payload(6)}))
    validate = fundamentals._validate_archive_payload_cik

    def legacy_validate(payload, cik):
        if "cik" not in payload:
            raise ValueError("legacy missing payload CIK failure")
        return validate(payload, cik)

    monkeypatch.setattr(fundamentals, "_validate_archive_payload_cik", legacy_validate)
    with pytest.raises(RuntimeError, match="injected issuer failure"):
        _FailBeforeSix().run(tmp_store, options)
    prior_id = tmp_store.con.execute("SELECT run_id FROM dataset_runs WHERE status='failed'").fetchone()[0]
    monkeypatch.setattr(fundamentals, "_validate_archive_payload_cik", validate)
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert result.details["previously_completed_targets"] == 1
    assert result.details["empty_target_count"] == 1
    assert result.details["failed_target_count"] == 0
    assert result.details["loaded_targets"] == 2


@pytest.mark.parametrize("failure", ["candidate", "receipt"])
def test_issuer_receipt_and_candidate_failure_rolls_back_facts_and_points(tmp_store, tmp_path, monkeypatch, failure):
    options = _options(_archive(tmp_path / "companyfacts.zip", {1: _payload(1)}))
    dataset = SecCompanyFactsDataset()
    dataset.run(tmp_store, options)
    before = _snapshot(tmp_store)
    if failure == "candidate":
        original = fundamentals.insert_frame

        def insert_then_fail(store, frame, table, relation_name):
            result = original(store, frame, table, relation_name)
            if table == "identifier_resolution_candidates":
                raise RuntimeError("injected atomic failure")
            return result

        monkeypatch.setattr(fundamentals, "insert_frame", insert_then_fail)
    else:
        original = fundamentals.record_source_file

        def receipt_then_fail(store, **kwargs):
            original(store, **kwargs)
            raise RuntimeError("injected atomic failure")

        monkeypatch.setattr(fundamentals, "record_source_file", receipt_then_fail)
    with pytest.raises(RuntimeError, match="injected atomic failure"):
        dataset.run(tmp_store, options)
    assert _snapshot(tmp_store) == before
    assert tmp_store.con.execute("SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal").fetchall() == []
    assert tmp_store.con.execute("SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal").fetchall() == []
    assert tmp_store.con.execute("SELECT status FROM dataset_runs ORDER BY started_at DESC LIMIT 1").fetchone()[0] == "failed"


@pytest.mark.parametrize("cik,shares,filed", [(759828, 8744547, "2026-07-01"),
                                            (855886, 49185225, "2026-07-01"),
                                            (925683, 19833060, "2026-08-24")])
def test_real_shape_cef_members_without_payload_cik_are_empty(tmp_store, tmp_path, cik, shares, filed):
    payload = {"entityName": "Fixture closed-end fund", "facts": {"cef": {"OutstandingSecurityHeldShares": {
        "label": "", "description": "", "units": {"shares": [{"start": "2025-11-01",
            "end": "2026-04-30", "val": shares, "accn": f"{cik:010d}-26-000014",
            "fy": None, "fp": None, "form": "N-CSRS", "filed": filed}]},
    }}}}
    result = SecCompanyFactsDataset().run(tmp_store, _options(_archive(tmp_path / "companyfacts.zip", {cik: payload})))
    assert result.details["completed_targets"] == result.details["empty_target_count"] == 1
    assert result.details["loaded_targets"] == result.details["failed_target_count"] == result.rows_loaded == 0
    assert result.details["empty_target_reasons"] == {"unsupported_or_empty_taxonomy": 1}
    status, raw = tmp_store.con.execute("SELECT status,metadata_json FROM raw_source_files").fetchone()
    assert status == "empty"
    metadata = json.loads(raw)
    assert metadata["payload_cik_identity"] == "validated_archive_member_missing_payload_cik"
    assert metadata["archive_member"] == f"CIK{cik:010d}.json"


def test_supported_facts_without_payload_cik_keep_isolated_source_identity(tmp_store, tmp_path):
    payload = _payload(741313)
    del payload["cik"]
    result = SecCompanyFactsDataset().run(tmp_store, _options(_archive(tmp_path / "companyfacts.zip", {741313: payload})))
    assert result.rows_loaded == 1
    assert tmp_store.con.execute("SELECT cik,security_id,entity_id,available_at FROM sec_company_facts").fetchone() == (
        "0000741313", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000741313", None, dt.datetime(2024, 2, 15, 22),
    )


@pytest.mark.parametrize("payload_cik", [None, True, "", "invalid", 1.0, -1, "1e0", 2])
def test_explicit_invalid_or_conflicting_payload_cik_remains_a_source_error(tmp_store, tmp_path, payload_cik):
    payload = _payload(1)
    payload["cik"] = payload_cik
    result = SecCompanyFactsDataset().run(tmp_store, _options(_archive(tmp_path / "companyfacts.zip", {1: payload})))
    assert result.details["failed_target_count"] == 1
    assert result.details["completed_targets"] == result.rows_loaded == 0
    assert tmp_store.con.execute("SELECT status FROM raw_source_files").fetchone()[0] == "error"


def test_reopen_after_ten_commits_preserves_caps_and_leaves_no_relations(tmp_store, tmp_path, monkeypatch):
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("SET memory_limit='256MB'")
    tmp_store.con.execute("SET threads=1")
    tmp_store.con.execute("SET preserve_insertion_order=false")
    settings_sql = "SELECT current_setting('memory_limit'),current_setting('threads'),current_setting('preserve_insertion_order')"
    expected_settings = tmp_store.con.execute(settings_sql).fetchone()
    reopens = []
    original = tmp_store.reopen

    def observed_reopen():
        assert tmp_store.connection is None
        original()
        assert tmp_store.con.execute(settings_sql).fetchone() == expected_settings
        assert tmp_store.con.execute("SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal").fetchall() == []
        assert tmp_store.con.execute("SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal").fetchall() == []
        reopens.append(tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()[0])

    monkeypatch.setattr(tmp_store, "reopen", observed_reopen)
    options = _options(_archive(tmp_path / "companyfacts.zip", {cik: _payload(cik) for cik in range(1, 13)}))
    result = SecCompanyFactsDataset().run(tmp_store, options)
    assert reopens == [10, 12]
    assert result.details["connection_reopens"] == 2
    assert result.rows_loaded == 12
    assert tmp_store.con.execute("SELECT count(*) FROM raw_source_files WHERE status='loaded'").fetchone()[0] == 12


def test_reopen_refuses_caller_owned_temp_state_without_losing_it(tmp_store):
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("CREATE TEMP TABLE caller_state AS SELECT 1 AS value")
    with pytest.raises(RuntimeError, match="caller-owned temporary"):
        reopen_companyfacts_store(tmp_store)
    assert tmp_store.con.execute("SELECT value FROM caller_state").fetchone() == (1,)


def test_resume_reopens_after_proof_before_candidate_recovery(tmp_store, tmp_path, monkeypatch):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    tmp_store.con.execute("DELETE FROM identifier_resolution_candidates")
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("SET memory_limit='256MB'")
    tmp_store.con.execute("SET threads=1")
    observations = []
    original = tmp_store.close

    def observe_before_close():
        observations.append(tmp_store.con.execute(
            "SELECT (SELECT count(*) FROM sec_company_facts),"
            "(SELECT count(*) FROM identifier_resolution_candidates)"
        ).fetchone())
        original()

    monkeypatch.setattr(tmp_store, "close", observe_before_close)
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert observations == [(2, 0)] * 4 + [(3, 3)]
    assert result.details["connection_reopens"] == 2
    assert result.details["resume_proof_connection_reopens"] == 3


def test_empty_replacement_is_tenth_raw_commit_before_next_target(tmp_store, tmp_path, monkeypatch):
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("SET memory_limit='256MB'")
    tmp_store.con.execute("SET threads=1")
    tmp_store.con.execute("SET preserve_insertion_order=false")
    observations = []
    original = tmp_store.close

    def observe_before_close():
        observations.append(tmp_store.con.execute(
            "SELECT (SELECT count(*) FROM sec_company_facts),"
            "(SELECT count(*) FROM identifier_resolution_candidates),"
            "(SELECT count(*) FROM raw_source_files)"
        ).fetchone())
        original()

    monkeypatch.setattr(tmp_store, "close", observe_before_close)
    members = {cik: _payload(cik) for cik in range(1, 10)}
    members[10] = {"cik": 10, "facts": {"cef": {}}}
    members[11] = b"{}"
    result = SecCompanyFactsDataset().run(
        tmp_store, _options(_archive(tmp_path / "companyfacts.zip", members)),
    )
    assert observations == [(9, 9, 10)]
    assert result.details["connection_reopens"] == 1
    assert result.details["completed_targets"] == result.details["empty_target_count"] + 9 == 10
    assert result.details["unavailable_target_count"] == 1
    assert tmp_store.con.execute(
        "SELECT status,count(*) FROM raw_source_files GROUP BY status ORDER BY status"
    ).fetchall() == [("empty", 1), ("loaded", 9), ("unavailable", 1)]


def test_empty_replacement_alone_triggers_final_partial_reopen(tmp_store, tmp_path, monkeypatch):
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("SET memory_limit='256MB'")
    tmp_store.con.execute("SET threads=1")
    tmp_store.con.execute("SET preserve_insertion_order=false")
    observations = []
    original = tmp_store.close

    def observe_before_close():
        observations.append(tmp_store.con.execute(
            "SELECT (SELECT count(*) FROM sec_company_facts),"
            "(SELECT count(*) FROM identifier_resolution_candidates),"
            "(SELECT count(*) FROM raw_source_files)"
        ).fetchone())
        original()

    monkeypatch.setattr(tmp_store, "close", observe_before_close)
    result = SecCompanyFactsDataset().run(
        tmp_store,
        _options(_archive(tmp_path / "companyfacts.zip", {1: {"cik": 1, "facts": {"cef": {}}}})),
    )
    assert observations == [(0, 0, 1)]
    assert result.details["connection_reopens"] == 1
    assert result.details["completed_targets"] == result.details["empty_target_count"] == 1
    assert result.rows_loaded == result.details["loaded_targets"] == 0
    assert tmp_store.con.execute("SELECT status FROM raw_source_files").fetchall() == [("empty",)]


def test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows(
        tmp_store, tmp_path, monkeypatch):
    """Exercise verified, raw, and empty paths through the real resume proof."""
    raw_replays = tuple(range(10, 101, 10))
    members = {cik: _payload(cik) for cik in range(1, 202)}
    members[202] = {"cik": 202, "facts": {"cef": {}}}
    options = _options(_archive(tmp_path / "companyfacts.zip", members))
    with pytest.raises(RuntimeError, match="injected issuer failure"):
        _FailOnCik(201).run(tmp_store, options)
    prior_id = tmp_store.con.execute(
        "SELECT run_id FROM dataset_runs WHERE dataset_id='sec_company_facts' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()[0]
    replay_urls = [fundamentals._companyfacts_zip_member_url(str(cik)) for cik in raw_replays]
    placeholders = ", ".join("?" for _ in replay_urls)
    tmp_store.con.execute(f"DELETE FROM raw_source_files WHERE source_url IN ({placeholders})", replay_urls)
    tmp_store.con.execute("DELETE FROM identifier_resolution_candidates")
    tmp_store.analytical_memory_limit = "256MB"
    tmp_store.analytical_threads = 1
    tmp_store.con.execute("SET memory_limit='256MB'")
    tmp_store.con.execute("SET threads=1")
    tmp_store.con.execute("SET preserve_insertion_order=false")
    settings_sql = "SELECT current_setting('memory_limit'),current_setting('threads'),current_setting('preserve_insertion_order')"
    expected_settings = tmp_store.con.execute(settings_sql).fetchone()
    owned_before = {
        "facts": tmp_store.con.execute(
            "SELECT * FROM sec_company_facts WHERE cik IN ('0000000001', '0000000101') ORDER BY cik"
        ).fetchall(),
        "points": tmp_store.con.execute(
            "SELECT * FROM fundamental_points WHERE accession_number IN "
            "('0000000001-24-000001', '0000000101-24-000001') ORDER BY accession_number"
        ).fetchall(),
        "receipts": tmp_store.con.execute(
            "SELECT * FROM raw_source_files WHERE source_url IN (?, ?) ORDER BY source_url",
            [fundamentals._companyfacts_zip_member_url("1"), fundamentals._companyfacts_zip_member_url("101")],
        ).fetchall(),
    }
    observations = []
    original_close = tmp_store.close
    original_reopen = tmp_store.reopen

    def observe_before_close():
        observations.append(tmp_store.con.execute(
            "SELECT (SELECT count(*) FROM sec_company_facts),"
            "(SELECT count(*) FROM identifier_resolution_candidates)"
        ).fetchone())
        original_close()

    def observe_reopen():
        assert tmp_store.connection is None
        original_reopen()
        assert tmp_store.con.execute(settings_sql).fetchone() == expected_settings
        assert tmp_store.con.execute(
            "SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal"
        ).fetchall() == []
        assert tmp_store.con.execute(
            "SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal"
        ).fetchall() == []

    monkeypatch.setattr(tmp_store, "close", observe_before_close)
    monkeypatch.setattr(tmp_store, "reopen", observe_reopen)
    result = SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert observations == [(200, 0)] * 4 + [(200, 100), (200, 200), (201, 201)]
    assert result.details["connection_reopens"] == 4
    assert result.details["resume_proof_connection_reopens"] == 3
    assert result.details["connection_reopen_interval"] == 10
    assert result.rows_loaded == 11
    assert result.details["previously_completed_targets"] == result.details["resumed_loaded_targets"] == 190
    assert result.details["replayed_targets"] == 12
    assert result.details["loaded_targets"] == 201
    assert result.details["completed_targets"] == 202
    assert result.details["empty_target_count"] == 1
    assert tmp_store.con.execute("SELECT count(*) FROM identifier_resolution_candidates").fetchone() == (201,)
    assert {
        "facts": tmp_store.con.execute(
            "SELECT * FROM sec_company_facts WHERE cik IN ('0000000001', '0000000101') ORDER BY cik"
        ).fetchall(),
        "points": tmp_store.con.execute(
            "SELECT * FROM fundamental_points WHERE accession_number IN "
            "('0000000001-24-000001', '0000000101-24-000001') ORDER BY accession_number"
        ).fetchall(),
        "receipts": tmp_store.con.execute(
            "SELECT * FROM raw_source_files WHERE source_url IN (?, ?) ORDER BY source_url",
            [fundamentals._companyfacts_zip_member_url("1"), fundamentals._companyfacts_zip_member_url("101")],
        ).fetchall(),
    } == owned_before


def test_verified_candidate_reconciliation_failure_rolls_back_without_counting_work(
        tmp_store, tmp_path, monkeypatch):
    options, prior_id = _failed_prefix(tmp_store, tmp_path)
    tmp_store.con.execute("DELETE FROM identifier_resolution_candidates")
    before = _snapshot(tmp_store)
    original = fundamentals.insert_frame

    def insert_then_fail(store, frame, table, relation_name):
        result = original(store, frame, table, relation_name)
        if table == "identifier_resolution_candidates":
            raise RuntimeError("injected verified candidate failure")
        return result

    monkeypatch.setattr(fundamentals, "insert_frame", insert_then_fail)
    with pytest.raises(RuntimeError, match="injected verified candidate failure"):
        SecCompanyFactsDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert _snapshot(tmp_store) == before
    assert tmp_store.con.execute(
        "SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal"
    ).fetchall() == []
    assert tmp_store.con.execute(
        "SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal"
    ).fetchall() == []
    assert tmp_store.con.execute(
        "SELECT status FROM dataset_runs ORDER BY started_at DESC LIMIT 1"
    ).fetchone() == ("failed",)
