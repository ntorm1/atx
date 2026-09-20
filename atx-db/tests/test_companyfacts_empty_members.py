"""Archive placeholders report source absence without replacing retained facts."""

from __future__ import annotations

import hashlib
import json
import zipfile
from dataclasses import replace
from types import SimpleNamespace

import pytest

from atx_db.activation import ActivationOptions, ActivationStageError, stage_companyfacts_load
from atx_db.fundamentals import (
    SEC_COMPANY_FACTS_ZIP_URL,
    SecCompanyFactsDataset,
    SecCompanyFactsOptions,
)

_CIK = "0000003521"
_MEMBER = f"CIK{_CIK}.json"
_SOURCE_URL = f"{SEC_COMPANY_FACTS_ZIP_URL}#{_MEMBER}"
_REASON = "empty_archive_placeholder"
_WARNING = "archive_empty_placeholders_preserved_prior_data"
_TABLES = ("sec_company_facts", "fundamental_points", "identifier_resolution_candidates")


@pytest.fixture(autouse=True)
def _forbid_network(monkeypatch):
    def forbidden(*_args, **_kwargs):
        pytest.fail("companyfacts placeholder tests must not open a network session")

    monkeypatch.setattr("atx_db.fundamentals.sec_session", forbidden)


def _payload(cik=_CIK):
    return {
        "cik": int(cik),
        "entityName": "Fixture issuer",
        "facts": {
            "us-gaap": {
                "Assets": {
                    "units": {
                        "USD": [{
                            "end": "2023-12-31",
                            "val": 100.0,
                            "accn": f"{cik}-24-000001",
                            "fy": 2023,
                            "fp": "FY",
                            "form": "10-K",
                            "filed": "2024-02-15",
                        }],
                    },
                },
            },
        },
    }


def _write_archive(path, members):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, payload in members.items():
            archive.writestr(name, payload if isinstance(payload, bytes) else json.dumps(payload))
    return path


def _options(path):
    return SecCompanyFactsOptions(
        symbol_source="archive_members",
        companyfacts_zip=path,
        concepts=("Assets",),
        refresh_derived_surfaces=False,
        run_id="placeholder-observation",
    )


def _snapshot(store):
    return {table: store.con.execute(f"SELECT * FROM {table}").fetchall() for table in _TABLES}


def _seed_retained_issuer(store, path):
    _write_archive(path, {_MEMBER: _payload()})
    result = SecCompanyFactsDataset().load(store, replace(_options(path), run_id="retained-history"))
    assert result.rows_loaded == 1
    before = _snapshot(store)
    assert all(len(rows) == 1 for rows in before.values())
    return before


def _assert_placeholder_counts(details, *, loaded=0, failed=0):
    assert details["unavailable_target_count"] == 1
    assert details["unavailable_target_reasons"] == {_REASON: 1}
    assert details["unavailable_targets"] == [{"cik": _CIK, "reason": _REASON}]
    assert details["loaded_targets"] == details["completed_targets"] == loaded
    assert details["failed_target_count"] == failed
    assert details["empty_target_count"] == 0
    assert details["target_count"] == loaded + failed + 1
    assert _WARNING in details["coverage_warnings"]
    assert details["listed_security_coverage_verified"] is False


@pytest.mark.parametrize("legacy_spelling", [False, True])
def test_exact_archive_placeholder_preserves_retained_rows_and_records_unavailability(
    tmp_store, tmp_path, legacy_spelling,
):
    path = tmp_path / "companyfacts.zip"
    before = _seed_retained_issuer(tmp_store, path)
    if legacy_spelling:
        tmp_store.con.execute("UPDATE sec_company_facts SET cik='3521', security_id='HISTORICAL'")
        tmp_store.con.execute("UPDATE fundamental_points SET security_id='SEC-CIK-0000003521'")
        tmp_store.con.execute("UPDATE identifier_resolution_candidates SET source_key_value='3521'")
        before = _snapshot(tmp_store)
    _write_archive(path, {_MEMBER: b"{}"})

    result = SecCompanyFactsDataset().load(tmp_store, _options(path))

    assert _snapshot(tmp_store) == before
    assert result.rows_loaded == 0
    assert result.details["facts"] == result.details["fundamental_points"] == 0
    assert result.details["unresolved_cik_candidate_rows"] == 0
    assert result.details["outcome"] == "source_unavailable"
    _assert_placeholder_counts(result.details)
    row = tmp_store.con.execute(
        "SELECT status, sha256, byte_count, metadata_json FROM raw_source_files "
        "WHERE dataset_id='sec_company_facts' AND source_url=?",
        [_SOURCE_URL],
    ).fetchone()
    assert row is not None
    assert row[:3] == ("unavailable", hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_size)
    metadata = json.loads(row[3])
    assert metadata["cik"] == _CIK
    assert metadata["source_mode"] == "bulk_zip"
    assert metadata["run_id"] == "placeholder-observation"
    assert metadata["rows"] == 0
    assert metadata["unavailable_reason"] == _REASON
    assert metadata["archive_member"] == _MEMBER
    assert metadata["archive_member_bytes"] == 2
    assert metadata["archive_sha256"] == result.details["archive_sha256"] == row[1]
    assert metadata["allowlist_sha256"] == result.details["allowlist_sha256"]
    warning = tmp_store.con.execute(
        "SELECT status, severity, observed_value, threshold_value, details_json "
        "FROM data_quality_checks WHERE dataset_id='sec_company_facts' "
        "AND check_name='source_availability' ORDER BY checked_at DESC LIMIT 1"
    ).fetchone()
    assert warning is not None
    assert warning[:4] == ("warning", "warning", 1.0, 0.0)
    assert json.loads(warning[4])["unavailable_target_count"] == 1


def test_placeholder_only_run_never_calls_financial_replacement_or_identity_resolution(
    tmp_store, tmp_path, monkeypatch,
):
    path = _write_archive(tmp_path / "companyfacts.zip", {_MEMBER: b"{}"})

    def forbidden(*_args, **_kwargs):
        pytest.fail("an unavailable archive member cannot enter the financial write path")

    monkeypatch.setattr(SecCompanyFactsDataset, "_replace_facts", forbidden)
    monkeypatch.setattr("atx_db.fundamentals.resolve_company_facts_identifiers", forbidden)
    monkeypatch.setattr("atx_db.fundamentals._companyfacts_cik_spellings", forbidden)

    result = SecCompanyFactsDataset().load(tmp_store, _options(path))

    assert result.rows_loaded == 0
    _assert_placeholder_counts(result.details)
    assert all(not rows for rows in _snapshot(tmp_store).values())


def test_placeholder_source_receipt_failure_remains_fatal(tmp_store, tmp_path, monkeypatch):
    path = tmp_path / "companyfacts.zip"
    before = _seed_retained_issuer(tmp_store, path)
    _write_archive(path, {_MEMBER: b"{}"})

    def broken_receipt(*_args, **_kwargs):
        raise RuntimeError("fixture source receipt write failed")

    monkeypatch.setattr("atx_db.fundamentals.record_source_file", broken_receipt)
    with pytest.raises(RuntimeError, match="fixture source receipt write failed"):
        SecCompanyFactsDataset().load(tmp_store, _options(path))
    assert _snapshot(tmp_store) == before


def test_mixed_archive_loads_usable_member_and_preserves_placeholder_history(tmp_store, tmp_path):
    path = tmp_path / "companyfacts.zip"
    before = _seed_retained_issuer(tmp_store, path)
    sibling_cik = "0000004000"
    _write_archive(path, {_MEMBER: b"{}", f"CIK{sibling_cik}.json": _payload(sibling_cik)})

    result = SecCompanyFactsDataset().load(tmp_store, _options(path))

    assert result.rows_loaded == result.details["facts"] == result.details["fundamental_points"] == 1
    assert result.details["outcome"] == "loaded_with_unavailable"
    _assert_placeholder_counts(result.details, loaded=1)
    for table, retained in before.items():
        current = tmp_store.con.execute(f"SELECT * FROM {table}").fetchall()
        assert len(current) == 2
        assert retained[0] in current
    assert tmp_store.con.execute(
        "SELECT cik FROM sec_company_facts WHERE run_id='placeholder-observation'"
    ).fetchall() == [(sibling_cik,)]
    assert tmp_store.con.execute(
        "SELECT status FROM data_quality_checks WHERE dataset_id='sec_company_facts' "
        "AND check_name='source_availability' ORDER BY checked_at DESC LIMIT 1"
    ).fetchone() == ("warning",)


@pytest.mark.parametrize(
    "invalid",
    [
        pytest.param(b" {}", id="leading-whitespace-is-unobserved"),
        pytest.param(b"{}\n", id="trailing-whitespace-is-unobserved"),
        pytest.param(b'{"cik":3521}', id="nonempty-missing-facts"),
        pytest.param(b'{"cik":3521,"facts":null}', id="null-facts"),
        pytest.param(b'{"cik":3521,"facts":[]}', id="nonobject-facts"),
        pytest.param(b'{"cik":99,"facts":{}}', id="mismatched-cik"),
        pytest.param(b"[]", id="nonobject-payload"),
        pytest.param(b"{", id="malformed-json"),
    ],
)
def test_other_invalid_archive_members_remain_failures_and_preserve_history(tmp_store, tmp_path, invalid):
    path = tmp_path / "companyfacts.zip"
    before = _seed_retained_issuer(tmp_store, path)
    _write_archive(path, {_MEMBER: invalid})

    result = SecCompanyFactsDataset().load(tmp_store, _options(path))

    assert result.rows_loaded == 0
    assert result.details["outcome"] == "failed_targets"
    assert result.details["failed_target_count"] == 1
    assert result.details["unavailable_target_count"] == 0
    assert result.details["empty_target_count"] == result.details["completed_targets"] == 0
    assert _snapshot(tmp_store) == before
    assert tmp_store.con.execute(
        "SELECT status FROM raw_source_files WHERE dataset_id='sec_company_facts' AND source_url=?",
        [_SOURCE_URL],
    ).fetchone() == ("error",)


@pytest.mark.parametrize("skip_failed", [False, True])
def test_network_empty_object_remains_a_failure(tmp_store, tmp_path, monkeypatch, skip_failed):
    path = tmp_path / "companyfacts.zip"
    before = _seed_retained_issuer(tmp_store, path)
    response = SimpleNamespace(raise_for_status=lambda: None, json=lambda: {})
    session = SimpleNamespace(get=lambda *_args, **_kwargs: response)
    monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda *_: session)
    monkeypatch.setattr(
        "atx_db.fundamentals.resolve_companyfacts_targets",
        lambda *_: [("FIXTURE", _CIK, f"SEC-CIK-{_CIK}")],
    )
    options = replace(_options(path), companyfacts_zip=None, symbol_source="symbols", skip_failed_targets=skip_failed)

    if skip_failed:
        result = SecCompanyFactsDataset().load(tmp_store, options)
        assert result.rows_loaded == 0
        assert result.details["failed_target_count"] == 1
        assert result.details["unavailable_target_count"] == 0
    else:
        with pytest.raises(RuntimeError, match="requires a facts object"):
            SecCompanyFactsDataset().load(tmp_store, options)

    assert _snapshot(tmp_store) == before
    assert tmp_store.con.execute(
        "SELECT status FROM raw_source_files WHERE dataset_id='sec_company_facts' "
        "AND source_url LIKE 'https://data.sec.gov/%'"
    ).fetchall() == [("error",)]


def test_valid_empty_facts_remain_authoritative_empty_replacement(tmp_store, tmp_path):
    path = tmp_path / "companyfacts.zip"
    _seed_retained_issuer(tmp_store, path)
    _write_archive(path, {_MEMBER: {"cik": int(_CIK), "facts": {}}})

    result = SecCompanyFactsDataset().load(tmp_store, _options(path))

    assert result.rows_loaded == 0
    assert result.details["empty_target_count"] == result.details["completed_targets"] == 1
    assert result.details["unavailable_target_count"] == result.details["failed_target_count"] == 0
    assert result.details["outcome"] == "supported_facts_empty"
    assert all(not rows for rows in _snapshot(tmp_store).values())
    assert tmp_store.con.execute(
        "SELECT status FROM raw_source_files WHERE dataset_id='sec_company_facts' AND source_url=?",
        [_SOURCE_URL],
    ).fetchone() == ("empty",)


@pytest.mark.parametrize("sibling", ["valid", "malformed", "absent"])
def test_activation_keeps_execution_outcome_separate_from_source_coverage(tmp_store, tmp_path, sibling):
    options = ActivationOptions(
        db_path=tmp_store.path,
        cache_dir=tmp_path,
        companyfacts_symbol_source="archive_members",
        companyfacts_progress_every=0,
        run_id="placeholder-activation",
    )
    members = {_MEMBER: b"{}"}
    if sibling != "absent":
        members["CIK0000004000.json"] = _payload("0000004000") if sibling == "valid" else b"{broken"
    _write_archive(options.companyfacts_zip, members)

    if sibling == "malformed":
        with pytest.raises(ActivationStageError, match="ingestion incomplete") as caught:
            stage_companyfacts_load(tmp_store, options)
        result = caught.value.result
        assert result.rows == 0
        assert result.detail["outcome"] == "failed_targets"
        _assert_placeholder_counts(result.detail, failed=1)
    elif sibling == "valid":
        result = stage_companyfacts_load(tmp_store, options)
        assert result.rows == 1
        assert result.detail["outcome"] == "loaded_with_unavailable"
        _assert_placeholder_counts(result.detail, loaded=1)
    else:
        result = stage_companyfacts_load(tmp_store, options)
        assert result.rows == 0
        assert result.detail["outcome"] == "source_unavailable"
        _assert_placeholder_counts(result.detail)

    assert result.detail["skip_loaded"] is False
