"""The `atx-db activate` subcommand."""

from __future__ import annotations

import json

import pytest

from atx_db import cli
from atx_db.activation import STAGE_ORDER


def test_activate_is_a_registered_subcommand():
    parser = cli._build_parser()
    action = next(a for a in parser._actions if a.dest == "command")
    assert "activate" in action.choices


def test_activate_dry_run_emits_one_json_line_per_selected_stage(built_warehouse, capsys):
    db_path = built_warehouse("cli_activate.duckdb")
    code = cli.main(
        [
            "activate",
            "--db-path",
            str(db_path),
            "--dry-run",
            "--only",
            "migrate",
            "--only",
            "provider_coverage",
        ]
    )
    assert code == 0
    lines = [json.loads(line) for line in capsys.readouterr().out.splitlines() if line.strip()]
    assert [line["stage"] for line in lines] == ["migrate", "provider_coverage"]
    assert {line["status"] for line in lines} == {"dry_run"}


def test_activate_dry_run_respects_start_and_stop(built_warehouse, capsys):
    db_path = built_warehouse("cli_activate_slice.duckdb")
    cli.main(
        ["activate", "--db-path", str(db_path), "--dry-run", "--start-stage", "periods", "--stop-stage", "standardized"]
    )
    lines = [json.loads(line) for line in capsys.readouterr().out.splitlines() if line.strip()]
    assert [line["stage"] for line in lines] == ["periods", "ttm", "calendarization", "standardized"]


def test_activate_rejects_an_unknown_stage(built_warehouse):
    db_path = built_warehouse("cli_activate_bad.duckdb")
    with pytest.raises(SystemExit):
        cli.main(["activate", "--db-path", str(db_path), "--only", "not-a-stage"])


def test_activate_exposes_every_ladder_stage_as_a_choice():
    parser = cli._build_parser()
    action = next(a for a in parser._actions if a.dest == "command")
    activate = action.choices["activate"]
    only = next(a for a in activate._actions if a.dest == "only")
    assert tuple(only.choices) == STAGE_ORDER
