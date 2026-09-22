"""Run after applying companyfacts-archive3-integration.patch to activation.py."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import zipfile
from dataclasses import replace
from types import SimpleNamespace

import pytest

from atx_db.activation import (
    ActivationOptions,
    ActivationStageError,
    activation_options_from_args,
    add_activation_arguments,
    run_activation,
    stage_companyfacts_load,
)

_UA = "atx-db/0.2 atx-research@example.com"
_PRIOR = "758f7d5c-73ae-4b66-a8c9-f1996afa163a"


def test_activation_forwards_resume_uuid_and_explicit_user_agent(tmp_path, monkeypatch):
    options = ActivationOptions(cache_dir=tmp_path, companyfacts_symbol_source="archive_members",
                                sec_user_agent=_UA, companyfacts_resume_from_run_id=_PRIOR)
    options.companyfacts_zip.write_bytes(b"fixture stub")
    captured = []

    def run(_self, _store, fact_options):
        captured.append(fact_options)
        return SimpleNamespace(rows_loaded=0, details={"failed_target_count": 0, "outcome": "loaded"})

    monkeypatch.setattr("atx_db.fundamentals.SecCompanyFactsDataset.run", run)
    stage_companyfacts_load(None, options)
    assert captured[0].resume_from_run_id == _PRIOR
    assert captured[0].user_agent == _UA
    assert captured[0].skip_loaded_targets is False


def test_offline_activation_records_only_explicit_dummy_user_agent(tmp_store, tmp_path, monkeypatch):
    options = ActivationOptions(cache_dir=tmp_path, companyfacts_symbol_source="archive_members",
                                as_of_date=dt.date(2026, 9, 20), sec_user_agent=_UA)
    with zipfile.ZipFile(options.companyfacts_zip, "w") as archive:
        archive.writestr("CIK0000759828.json", json.dumps({"entityName": "Fixture fund", "facts": {"cef": {}}}))
    monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda *_: pytest.fail("offline stage opened HTTP"))
    result = stage_companyfacts_load(tmp_store, options)
    assert result.detail["empty_target_count"] == 1
    raw = tmp_store.con.execute("SELECT params_json FROM dataset_runs WHERE dataset_id='sec_company_facts'").fetchone()[0]
    assert json.loads(raw)["user_agent"] == _UA


def test_companyfacts_resume_cli_round_trip():
    parser = argparse.ArgumentParser()
    add_activation_arguments(parser)
    args = parser.parse_args(["--companyfacts-symbol-source", "archive_members",
                              "--companyfacts-resume-from-run-id", _PRIOR,
                              "--sec-user-agent", _UA, "--only", "companyfacts_load"])
    options = activation_options_from_args(args)
    assert options.companyfacts_resume_from_run_id == _PRIOR
    assert options.sec_user_agent == _UA


def test_source_incomplete_activation_resumes_its_newest_dataset_uuid(tmp_store, tmp_path, monkeypatch):
    options = ActivationOptions(cache_dir=tmp_path, companyfacts_symbol_source="archive_members",
                                as_of_date=dt.date(2026, 9, 20), sec_user_agent=_UA)
    with zipfile.ZipFile(options.companyfacts_zip, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps({"cik": 1, "facts": {"us-gaap": {"Assets": {
            "units": {"USD": [{"end": "2023-12-31", "val": 100, "accn": "0000000001-24-000001",
                                "form": "10-K", "filed": "2024-02-15"}]},
        }}}}))
        archive.writestr("CIK0000000002.json", b"{invalid")
    monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda *_: pytest.fail("offline stage opened HTTP"))
    with pytest.raises(ActivationStageError, match="ingestion incomplete") as first:
        stage_companyfacts_load(tmp_store, options)
    assert first.value.result.rows == 1
    newest_id, status = tmp_store.con.execute(
        "SELECT run_id,status FROM dataset_runs WHERE dataset_id='sec_company_facts' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()
    assert status == "succeeded"
    with pytest.raises(ActivationStageError, match="ingestion incomplete") as resumed:
        stage_companyfacts_load(tmp_store, replace(options, companyfacts_resume_from_run_id=newest_id))
    assert resumed.value.result.rows == 0
    assert resumed.value.result.detail["previously_completed_targets"] == 1
    assert resumed.value.result.detail["failed_target_count"] == 1


@pytest.mark.parametrize("change,stages", [
    ({"companyfacts_symbol_source": "sec_company_tickers"}, ("companyfacts_load",)),
    ({"companyfacts_limit": 10}, ("companyfacts_load",)),
    ({"skip_loaded_companyfacts": True}, ("companyfacts_load",)),
    ({}, ("sec_bulk_download", "companyfacts_load")),
])
def test_unsafe_resume_plan_rejected_even_for_dry_run(tmp_path, change, stages):
    options = ActivationOptions(db_path=tmp_path / "must-not-be-created.duckdb", dry_run=True,
                                companyfacts_symbol_source="archive_members", sec_user_agent=_UA,
                                companyfacts_resume_from_run_id=_PRIOR)
    with pytest.raises(ValueError, match="companyfacts resume"):
        run_activation(replace(options, **change), stages=stages)
    assert not options.db_path.exists()
