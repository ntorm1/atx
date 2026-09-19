"""``scripts/warehouse_activate.py``: the governed-migrations seam and dry-run plan.

The script must run the governed migration path (checkpoint + backup + locked
apply + verify, restore-on-failure) BEFORE opening the ladder's own store when
the warehouse file already exists, but must never touch a fresh (non-existent)
file that way -- ``stage_migrate`` bootstraps that one directly -- and must
never touch anything at all under ``--dry-run``. These tests inject a fake
governed-migrations callable at the ``main(argv, *, governed_migrations=...)``
seam and stub out ``run_activation`` so no real ladder work (schema-from-
scratch, network, subprocess shard-outs) runs; that behaviour is already
covered by ``tests/test_activation_ladder.py``.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "warehouse_activate.py"


def _load_warehouse_activate_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location("warehouse_activate_cli", SCRIPT_PATH)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _stub_run_activation(monkeypatch, module: ModuleType) -> list[object]:
    """Replace run_activation with a no-op recorder so no real ladder work runs."""
    calls: list[object] = []

    def fake_run_activation(options, *, stages=None, **kwargs):
        calls.append(options)
        return []

    monkeypatch.setattr(module, "run_activation", fake_run_activation)
    return calls


def test_main_runs_governed_migrations_before_the_ladder_when_the_db_exists(monkeypatch, built_warehouse):
    module = _load_warehouse_activate_module()
    db_path = built_warehouse("existing.duckdb")
    migration_calls: list[Path] = []
    ladder_calls = _stub_run_activation(monkeypatch, module)

    exit_code = module.main(
        ["--db-path", str(db_path), "--only", "migrate"],
        governed_migrations=migration_calls.append,
    )

    assert exit_code == 0
    assert migration_calls == [db_path]
    assert len(ladder_calls) == 1


def test_main_skips_governed_migrations_for_a_fresh_database(monkeypatch, tmp_path):
    module = _load_warehouse_activate_module()
    db_path = tmp_path / "brand-new.duckdb"
    assert not db_path.exists()
    migration_calls: list[Path] = []
    ladder_calls = _stub_run_activation(monkeypatch, module)

    exit_code = module.main(
        ["--db-path", str(db_path), "--only", "migrate"],
        governed_migrations=migration_calls.append,
    )

    assert exit_code == 0
    assert migration_calls == []
    assert len(ladder_calls) == 1


def test_main_skips_governed_migrations_under_dry_run_even_if_the_db_exists(monkeypatch, built_warehouse):
    module = _load_warehouse_activate_module()
    db_path = built_warehouse("existing-dry-run.duckdb")
    migration_calls: list[Path] = []
    ladder_calls = _stub_run_activation(monkeypatch, module)

    exit_code = module.main(
        ["--db-path", str(db_path), "--only", "migrate", "--dry-run"],
        governed_migrations=migration_calls.append,
    )

    assert exit_code == 0
    assert migration_calls == []
    assert len(ladder_calls) == 1
    assert ladder_calls[0].dry_run is True


def test_select_stages_slice_is_forwarded_to_the_ladder(monkeypatch, built_warehouse):
    module = _load_warehouse_activate_module()
    db_path = built_warehouse("slice.duckdb")
    _stub_run_activation(monkeypatch, module)
    captured: dict[str, object] = {}

    def fake_run_activation(options, *, stages=None, **kwargs):
        captured["stages"] = stages
        return []

    monkeypatch.setattr(module, "run_activation", fake_run_activation)

    module.main(
        ["--db-path", str(db_path), "--only", "provider_coverage", "--only", "migrate", "--dry-run"],
        governed_migrations=lambda path: None,
    )

    # select_stages always returns STAGE_ORDER order regardless of --only order.
    assert captured["stages"] == ("migrate", "provider_coverage")


def test_script_dry_run_emits_exactly_the_requested_stage_lines(built_warehouse):
    db_path = built_warehouse("subprocess-smoke.duckdb")
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT_PATH),
            "--db-path",
            str(db_path),
            "--dry-run",
            "--only",
            "migrate",
            "--only",
            "provider_coverage",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = [json.loads(line) for line in completed.stdout.strip().splitlines()]
    assert [line["stage"] for line in lines] == ["migrate", "provider_coverage"]
    assert [line["status"] for line in lines] == ["dry_run", "dry_run"]
