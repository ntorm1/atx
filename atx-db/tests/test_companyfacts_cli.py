"""Companyfacts archive CLI and scheduled-job option plumbing; no database or HTTP."""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

import pytest

from atx_db.activation import activation_options_from_args, add_activation_arguments
from atx_db.jobs import _company_facts_options


def _script(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_bulk_archive_cli_is_explicitly_offline_and_deferred():
    module = _script("build_companyfacts_bulk")
    args = module.parse_args(["--companyfacts-zip", "local.zip", "--symbol-source", "archive_members",
                              "--defer-derived-surfaces", "--as-of-date", "2026-09-20"])
    assert args.symbol_source == "archive_members"
    assert args.defer_derived_surfaces is True
    assert args.skip_loaded is False
    for incompatible in (["--download"], ["--factor-ids", "anything"]):
        with pytest.raises(SystemExit):
            module.parse_args(["--companyfacts-zip", "local.zip", "--symbol-source", "archive_members", *incompatible])


def test_activation_and_job_options_preserve_archive_and_append_policy():
    parser = argparse.ArgumentParser()
    add_activation_arguments(parser)
    flags = ["--companyfacts-symbol-source", "archive_members", "--as-of-date", "2026-09-20"]
    opts = activation_options_from_args(parser.parse_args(flags))
    assert opts.companyfacts_symbol_source == "archive_members"
    assert opts.skip_loaded_companyfacts is None  # archive stage defaults to replacement
    assert activation_options_from_args(parser.parse_args([*flags, "--companyfacts-append-missing"])).skip_loaded_companyfacts
    assert activation_options_from_args(parser.parse_args([*flags, "--companyfacts-replace-existing"])).skip_loaded_companyfacts is False
    job = _company_facts_options({"symbol_source": "archive_members", "companyfacts_zip": "local.zip",
                                 "symbol_offset": 2, "symbol_limit": 3, "skip_loaded_targets": True,
                                 "skip_failed_targets": True, "refresh_derived_surfaces": False,
                                 "progress_every_targets": 50, "run_id": "job-archive"})
    assert job.symbol_source == "archive_members"
    assert job.companyfacts_zip == Path("local.zip")
    assert (job.symbol_offset, job.symbol_limit) == (2, 3)
    assert job.skip_loaded_targets and job.skip_failed_targets
    assert job.refresh_derived_surfaces is False
    assert job.progress_every_targets == 50
    assert job.run_id == "job-archive"


def test_activation_wrapper_enables_info_logging(monkeypatch):
    module = _script("warehouse_activate")
    calls = []
    monkeypatch.setattr(module.logging, "basicConfig", lambda **kwargs: calls.append(kwargs))
    monkeypatch.setattr(module, "run_activation_from_args", lambda *args, **kwargs: 0)
    assert module.main(["--dry-run"]) == 0
    assert calls[0]["level"] == module.logging.INFO
